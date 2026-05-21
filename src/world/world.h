#pragma once

#include "core/thread_pool.h"
#include "gfx/frustum.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"

#include <glm/glm.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace world {

// 2D chunk coordinate (we don't shard vertically — Y is the full 256 column).
struct ChunkCoord {
    std::int32_t x;
    std::int32_t z;

    bool operator==(const ChunkCoord& o) const { return x == o.x && z == o.z; }
};

struct ChunkCoordHash {
    std::size_t operator()(const ChunkCoord& c) const noexcept {
        // Cantor-ish 64-bit mix.
        std::uint64_t ux = static_cast<std::uint32_t>(c.x);
        std::uint64_t uz = static_cast<std::uint32_t>(c.z);
        std::uint64_t h = (ux * 0x9E3779B97F4A7C15ull) ^ (uz + 0xBF58476D1CE4E5B9ull);
        h ^= h >> 27; h *= 0x94D049BB133111EBull; h ^= h >> 31;
        return static_cast<std::size_t>(h);
    }
};

// One slot in the world map: the block data + its GPU mesh + bounds.
struct ChunkSlot {
    ChunkCoord coord{};
    Chunk      chunk;
    gfx::Mesh  mesh;
    gfx::AABB  aabb{};
    int        quad_count = 0;
    bool       has_mesh = false;
};

struct DrawStats {
    int chunks_total = 0;
    int chunks_drawn = 0;
    std::size_t triangles_drawn = 0;
};

class World {
public:
    // Generate `(2*radius+1)^2` chunks centered at the origin chunk using
    // the given column filler. The filler is called once per (x,z) column
    // with world-space integer coords; it should populate the column in
    // the provided chunk.
    using ColumnFiller = std::function<void(int world_x, int world_z, Chunk& c, int local_x, int local_z)>;

    void generate_grid(int radius, const ColumnFiller& fill_column);

    // Generate `(2*radius+1)^2` chunks using a TerrainGen. Reports the
    // total time spent in chunk gen + meshing so we can put a real
    // throughput number on the resume.
    struct GenStats {
        int chunks_generated = 0;
        double gen_ms = 0.0;    // total time in terrain.fill_chunk
        double mesh_ms = 0.0;   // total time in build_chunk_mesh_greedy
        double total_ms = 0.0;  // wall-clock for the whole grid
    };
    GenStats generate_grid(int radius, const TerrainGen& terrain);

    // Async variant: enqueues `(2r+1)^2` jobs into the pool. Each job
    // runs terrain + greedy meshing on a worker thread and pushes the
    // result into an internal finished-queue. The main thread must call
    // drain_finished() each frame to upload completed chunks to GPU
    // (GL is single-threaded).
    void enqueue_grid_async(int radius, const TerrainGen& terrain, core::ThreadPool& pool);

    // Streaming: keep a (2*radius+1)^2 window of chunks loaded around
    // `center_chunk`. Any chunk outside the window is evicted. Any
    // chunk inside the window that isn't loaded (and isn't already in
    // flight) gets enqueued on the pool. Call each frame; cheap when
    // the center hasn't moved.
    struct StreamStats {
        int evicted = 0;
        int requested = 0;
        int loaded = 0;
    };
    StreamStats update_streaming(ChunkCoord center_chunk, int radius,
                                 const TerrainGen& terrain,
                                 core::ThreadPool& pool);

    // Pop up to `max_per_frame` finished chunks from the worker queue,
    // upload their meshes, and insert them into the world. Safe to call
    // every frame from the main (GL) thread. Returns the number of
    // chunks uploaded this call.
    int drain_finished(int max_per_frame = 8);

    // How many async jobs have not yet been uploaded (in-flight on
    // workers OR sitting in the finished queue).
    int pending_async() const;

    // Block lookup across chunks. Returns Air when outside any loaded
    // chunk. World-space integer block coords.
    BlockId block_at(int wx, int wy, int wz) const;

    // Replace the block at (wx, wy, wz). Rebuilds the affected chunk's
    // mesh synchronously. No-op outside loaded chunks. Returns true if
    // a change was made.
    bool set_block(int wx, int wy, int wz, BlockId b);

    // Walk a ray from `origin` along `direction` until it hits a solid
    // block, or `max_distance` is exceeded. Uses 3D DDA (Amanatides-Woo).
    struct RayHit {
        bool       hit = false;
        int        block_x = 0;
        int        block_y = 0;
        int        block_z = 0;
        // Face normal of the hit (one component +/-1, others 0). The
        // block at (block_*) + normal is the empty cell adjacent to the
        // hit face, which is where a new block would be placed.
        int        nx = 0;
        int        ny = 0;
        int        nz = 0;
        float      distance = 0.0f;
    };
    RayHit raycast(const glm::vec3& origin, const glm::vec3& direction,
                   float max_distance = 8.0f) const;

    // Draw every chunk whose AABB intersects the frustum. Caller is
    // responsible for shader.use() and uniforms that don't change per chunk
    // (view, proj, light, texture). We set u_model per chunk.
    DrawStats draw_visible(const gfx::Frustum& frustum, const gfx::Shader& shader) const;

    // Same iteration, but caller supplies the per-chunk model setter.
    // Used by the shadow pass where we want to draw with a different
    // shader and against the light's view-proj rather than the camera's.
    // The frustum here is the LIGHT's frustum (or a bigger one to
    // include shadow casters just outside the view).
    DrawStats draw_visible_with(const gfx::Frustum& frustum,
        std::function<void(const glm::mat4& model)> set_model) const;

    // Diagnostic: print every chunk's coord, AABB, frustum result. Used
    // to verify the culling math, not called per frame.
    void debug_dump_visibility(const gfx::Frustum& frustum) const;

    std::size_t chunk_count() const { return chunks_.size(); }

private:
    // Result handed from worker -> main thread. Holds the heavy data
    // (block grid + cpu mesh) ready for a GL upload.
    struct FinishedChunk {
        ChunkCoord    coord;
        Chunk         chunk;
        ChunkMeshData mesh_data;
    };

    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkSlot>, ChunkCoordHash> chunks_;
    // Coords that have been enqueued to the worker pool but haven't yet
    // been drained into chunks_. Prevents double-enqueueing on streaming
    // updates.
    std::unordered_set<ChunkCoord, ChunkCoordHash> requested_;

    mutable std::mutex                 finished_mutex_;
    std::queue<FinishedChunk>          finished_;
    std::atomic<int>                   jobs_in_flight_{0};
};

}  // namespace world
