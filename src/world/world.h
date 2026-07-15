#pragma once

#include "core/thread_pool.h"
#include "gfx/frustum.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/section_visibility.h"
#include "world/terrain_gen.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace world {

struct ChunkCoord {
    std::int32_t x;
    std::int32_t z;
    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};

struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const noexcept {
        std::uint64_t ux = static_cast<std::uint32_t>(c.x);
        std::uint64_t uz = static_cast<std::uint32_t>(c.z);
        std::uint64_t h = (ux * 0x9E3779B97F4A7C15ull) ^ (uz + 0xBF58476D1CE4E5B9ull);
        h ^= h >> 27; h *= 0x94D049BB133111EBull; h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

// Section constants (kSectionHeight, kSectionsPerChunk) live in chunk.h.
// Each section has its own tight AABB and is culled independently - the
// chunk AABB is the union.

// One section's slice of its chunk's shared mesh: an index range into the
// chunk EBO + the section's own world-space AABB for culling. Sharing one
// VBO per chunk (instead of one per section) is what keeps load-time GL
// throughput on par with the pre-section pipeline.
struct ChunkSection {
    gfx::AABB    aabb{};
    std::uint32_t index_offset = 0;
    std::uint32_t index_count  = 0;
    int           quad_count   = 0;
    bool          has_mesh     = false;
};

struct ChunkSlot {
    ChunkCoord coord{};
    Chunk      chunk;
    // One mesh per chunk; sections index into it via (index_offset, index_count).
    gfx::Mesh  chunk_mesh;
    std::array<ChunkSection, kSectionsPerChunk> sections{};
    // Per-section face-pair connectivity for occlusion culling. Computed on
    // the worker next to the greedy mesh; consumed by occlusion_bfs.
    SectionVisArray section_visibility{};
    // Union of section AABBs - the chunk-level fast-path test. If this misses
    // the frustum, we skip all section tests for the chunk.
    gfx::AABB  chunk_aabb{};
    int        quad_count_total = 0;  // sum of section quad counts
    bool       any_section_has_mesh = false;
    // Bytes this chunk holds in GPU buffers (VBO + EBO): the actual vertex
    // and index data uploaded for it. Summed across resident chunks to get
    // the engine's GPU mesh footprint, the VRAM analogue of RSS.
    std::size_t gpu_bytes = 0;
};

// Tight per-chunk AABB: XZ from the chunk's world origin, Y from the actual
// min/max of solid blocks (closed range, +1 on max). Exposed so the cull
// benchmark can build the same AABBs the renderer uses without going through
// the GL-backed ChunkSlot path.
gfx::AABB make_chunk_aabb(ChunkCoord coord, const Chunk& chunk);

// One row of the bench's section-AABB readout: the world-space AABB plus
// whether the section actually contains any meshed quads (empty sections
// shouldn't count against the cull ratio).
struct SectionBounds {
    gfx::AABB aabb{};
    bool      has_mesh = false;
};

// Runs the greedy mesher + the same per-section bucketing the renderer uses
// and returns just the section AABBs. CPU-only, no GL needed - meant for
// --bench, not the hot path.
std::array<SectionBounds, kSectionsPerChunk>
compute_section_bounds(ChunkCoord coord, const Chunk& chunk);

struct DrawStats {
    int chunks_total = 0;
    int chunks_drawn = 0;
    int sections_total = 0;
    int sections_drawn = 0;
    // Sections that passed the frustum test but were skipped because the
    // occlusion BFS couldn't reach them through air. 0 on frustum-only paths.
    int sections_occluded = 0;
    std::size_t triangles_drawn = 0;
};

// Sections reachable from the camera, one bitmask per chunk (bit sy set =
// section sy visible). Filled by occlusion_bfs, consumed by the draw path
// and the --bench cull harness.
static_assert(kSectionsPerChunk <= 8, "section reach mask is a uint8_t");
using SectionReachableMap =
    std::unordered_map<ChunkCoord, std::uint8_t, ChunkCoordHash>;

// Breadth-first traversal of the section visibility graph, seeded at the
// camera's section. A section is marked reachable when a sightline could
// get there: each BFS step must (a) stay inside the frustum (full section
// box test), (b) pass through the source section's air (face-pair
// connectivity from compute_section_visibility), and (c) never reverse a
// direction already taken on the path (Minecraft's cave-culling rule, which
// stops wrap-around false positives). visibility_of returns a chunk's masks
// or nullptr for unloaded chunks. Returns false without marking anything
// when the camera's own chunk isn't loaded - callers fall back to
// frustum-only culling.
bool occlusion_bfs(
    const glm::vec3& camera_pos,
    const gfx::Frustum& frustum,
    const std::function<const SectionVisArray*(ChunkCoord)>& visibility_of,
    SectionReachableMap& reachable);

// True if section sy (tight mesh AABB `aabb`) should draw given its chunk's
// reachable mask. Encodes the upward-spill rule (greedy quads bucket by
// bottom Y, so a section's AABB can span slabs above it); shared between the
// renderer and the --bench cull harness so they can't drift apart.
bool section_reachable_in_mask(std::uint8_t mask, int sy, const gfx::AABB& aabb);

class World {
public:
    using ColumnFiller = std::function<void(int world_x, int world_z,
                                            Chunk& c, int local_x, int local_z)>;
    void generate_grid(int radius, const ColumnFiller& fill_column);

    struct GenStats {
        int chunks_generated = 0;
        double gen_ms = 0.0;
        double mesh_ms = 0.0;
        double total_ms = 0.0;
    };
    GenStats generate_grid(int radius, const TerrainGen& terrain);

    void enqueue_grid_async(int radius, const TerrainGen& terrain, core::ThreadPool& pool);

    struct StreamStats {
        int evicted = 0;
        int requested = 0;
        int loaded = 0;
    };
    StreamStats update_streaming(ChunkCoord center, int radius,
                                 const TerrainGen& terrain,
                                 core::ThreadPool& pool);

    int  drain_finished(int max_per_frame = 8);
    int  pending_async() const;

    // Replaces (or inserts) a chunk slot. Builds the greedy mesh and uploads
    // it on the calling thread, so the caller must own a current GL context.
    void insert_chunk(ChunkCoord c, Chunk chunk);

    // Submits a worker job that greedy-meshes the already-decoded chunk and
    // pushes the result onto the finished queue. The caller drains via
    // drain_finished on the main (GL) thread. Used by load_world to
    // parallelize meshing during F6 / --bench-io load - mirrors the
    // enqueue_grid_async path but skips terrain.fill_chunk.
    void enqueue_decoded_chunk(ChunkCoord c, Chunk chunk, core::ThreadPool& pool);

    // Drops every chunk + pending request. Intended for full-world reload
    // (save/load); does not cancel in-flight worker jobs but their results
    // get discarded in drain_finished().
    void clear_all();

    // Iterates every loaded chunk slot in unspecified order. Read-only.
    void for_each_chunk(
        const std::function<void(ChunkCoord, const Chunk&)>& fn) const;

    BlockId block_at(int wx, int wy, int wz) const;
    bool    set_block(int wx, int wy, int wz, BlockId b);

    struct RayHit {
        bool  hit = false;
        int   block_x = 0, block_y = 0, block_z = 0;
        int   nx = 0, ny = 0, nz = 0;  // face normal (one of -1/0/+1)
        float distance = 0.0f;
    };
    RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                   float max_distance = 8.0f) const;

    DrawStats draw_visible(const gfx::Frustum& frustum, const gfx::Shader& shader) const;
    DrawStats draw_visible_with(const gfx::Frustum& frustum,
        std::function<void(const glm::mat4& model)> set_model) const;

    // draw_visible plus occlusion: only sections the camera can reach
    // through air (occlusion_bfs) are drawn. Falls back to plain
    // draw_visible when the camera's chunk isn't loaded. The shadow pass
    // must NOT use this - its frustum belongs to the light, not the camera.
    DrawStats draw_visible_occluded(const gfx::Frustum& frustum,
                                    const glm::vec3& camera_pos,
                                    const gfx::Shader& shader) const;

    void debug_dump_visibility(const gfx::Frustum& frustum) const;

    // Reads every chunk's VBO/EBO back off the GPU and checks each triangle
    // is an axis-aligned face backed by a solid block in that chunk's data.
    // Prints offenders; returns their count. Diagnostic for phantom-geometry
    // bugs - validates what the GPU draws, not what the CPU built.
    int debug_validate_gpu_meshes() const;

    std::size_t chunk_count() const { return chunks_.size(); }

    // Total bytes the resident chunks hold in GPU vertex + index buffers: the
    // engine's GPU mesh footprint, to sit alongside RSS. O(resident chunks),
    // so call it once per report, not per drawn section.
    std::size_t resident_gpu_bytes() const {
        std::size_t total = 0;
        for (const auto& [coord, slot] : chunks_) total += slot->gpu_bytes;
        return total;
    }

    // Cumulative timing counters across all completed chunks. Worker total
    // is wall time spent in terrain.fill_chunk + greedy meshing on a worker
    // thread (so 9 workers in parallel see this number race ahead of wall
    // clock); it splits cleanly into the terrain and mesh sub-totals.
    // Upload total is wall time spent in apply_sections on the main thread
    // (GL is single-threaded so this is a real serialization point).
    double total_worker_ms()  const { return total_worker_ms_; }
    double total_terrain_ms() const { return total_terrain_ms_; }
    double total_mesh_ms()    const { return total_mesh_ms_; }
    double total_upload_ms()  const { return total_upload_ms_; }

private:
    struct FinishedChunk {
        ChunkCoord      coord;
        Chunk           chunk;
        ChunkMeshData   mesh_data;
        SectionVisArray visibility{};
        double          worker_ms  = 0.0;
        double          terrain_ms = 0.0;
        std::uint64_t   generation = 0;  // job's world generation at submit
    };

    // Shared draw loop. reachable == nullptr means frustum-only; otherwise
    // a section draws only if some section its AABB vertically spans is in
    // the reachable mask (greedy quads bucket by their bottom Y, so a tall
    // side face can live in a lower section than the camera sees).
    DrawStats draw_impl(const gfx::Frustum& frustum,
                        const SectionReachableMap* reachable,
                        const std::function<void(const glm::mat4&)>& set_model) const;

    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkSlot>, ChunkCoordHash> chunks_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> requested_;

    mutable std::mutex                 finished_mutex_;
    std::queue<FinishedChunk>          finished_;
    std::atomic<int>                   jobs_in_flight_{0};
    // Bumped whenever the resident set is wiped (clear_all, enqueue_grid_async);
    // a finished job whose stamp no longer matches is discarded, so a load
    // cannot pick up regenerated terrain from a job the wipe outran. Only
    // touched on the main thread (submit, drain, wipe), so not atomic.
    std::uint64_t                      generation_ = 0;
    double                             total_worker_ms_  = 0.0;
    double                             total_terrain_ms_ = 0.0;
    double                             total_mesh_ms_    = 0.0;
    double                             total_upload_ms_  = 0.0;
};

}  // namespace world
