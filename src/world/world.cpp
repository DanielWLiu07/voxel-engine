#include "world/world.h"

#include "core/profiler.h"
#include "world/chunk_mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

namespace {

constexpr int floor_div(int a, int n) {
    int q = a / n;
    int r = a % n;
    if ((r != 0) && ((r < 0) != (n < 0))) --q;
    return q;
}

constexpr int floor_mod(int a, int n) {
    int r = a % n;
    if (r < 0) r += n;
    return r;
}

}  // namespace

namespace world {

namespace {

// Min/max Y of any solid block in the chunk. Returned as a closed range
// (max_y is inclusive). Empty chunks return {0,0} — slot has no mesh so
// the AABB is never tested anyway.
std::pair<int,int> tight_y_range(const Chunk& chunk) {
    int min_y = -1, max_y = -1;
    for (int y = 0; y < kChunkSizeY; ++y) {
        bool any_solid = false;
        for (int z = 0; z < kChunkSizeZ && !any_solid; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                if (is_solid(chunk.get(x, y, z))) { any_solid = true; break; }
            }
        }
        if (any_solid) {
            if (min_y < 0) min_y = y;
            max_y = y;
        }
    }
    if (min_y < 0) return {0, 0};
    return {min_y, max_y};
}

}  // namespace

// Tight per-chunk AABB derived from the chunk's solid extents. With wide
// (0..256) Y, a horizontally-forward frustum intersects almost every column
// it overlaps in XZ — vertical pruning collapses to nothing. Tight Y means
// chunks whose terrain sits well above or below the camera get culled.
gfx::AABB make_chunk_aabb(ChunkCoord c, const Chunk& chunk) {
    const float ox = static_cast<float>(c.x * kChunkSizeX);
    const float oz = static_cast<float>(c.z * kChunkSizeZ);
    auto [min_y, max_y] = tight_y_range(chunk);
    return {{ox,                 static_cast<float>(min_y),    oz},
            {ox + kChunkSizeX,   static_cast<float>(max_y + 1), oz + kChunkSizeZ}};
}

namespace {

// One section's mesh in-build form. Vertices are the same chunk-LOCAL
// positions the mesher emits; the world-space AABB folds in the chunk
// origin so cull tests don't need the model matrix.
struct SectionBuild {
    std::vector<gfx::VertexPNT> vertices;
    std::vector<std::uint32_t>  indices;
    glm::vec3 aabb_min{};
    glm::vec3 aabb_max{};
    int  quad_count   = 0;
    bool initialized  = false;
};

// Bucket the chunk-wide greedy mesh into per-section slices. Each quad is
// assigned to the section that contains its bottom Y; the section's AABB
// extends to the quad's actual extent, so a side face that spans two
// sections still draws correctly when the higher section is in-frustum
// (the AABB pulls the lower section in too — conservative, correct).
//
// Bucketing instead of meshing-per-section keeps the greedy merger
// chunk-wide, so we don't lose face runs at section boundaries — bullet
// #1 (greedy ratio) doesn't regress.
std::array<SectionBuild, kSectionsPerChunk>
bucket_quads_by_section(const ChunkMeshData& src, ChunkCoord coord) {
    std::array<SectionBuild, kSectionsPerChunk> out;
    const float ox = static_cast<float>(coord.x * kChunkSizeX);
    const float oz = static_cast<float>(coord.z * kChunkSizeZ);

    const std::size_t quad_count = src.vertices.size() / 4;
    for (std::size_t q = 0; q < quad_count; ++q) {
        const auto& v0 = src.vertices[4 * q + 0];
        const auto& v1 = src.vertices[4 * q + 1];
        const auto& v2 = src.vertices[4 * q + 2];
        const auto& v3 = src.vertices[4 * q + 3];

        const float ymin = std::min({v0.position.y, v1.position.y,
                                     v2.position.y, v3.position.y});
        const float ymax = std::max({v0.position.y, v1.position.y,
                                     v2.position.y, v3.position.y});
        const float xmin = std::min({v0.position.x, v1.position.x,
                                     v2.position.x, v3.position.x});
        const float xmax = std::max({v0.position.x, v1.position.x,
                                     v2.position.x, v3.position.x});
        const float zmin = std::min({v0.position.z, v1.position.z,
                                     v2.position.z, v3.position.z});
        const float zmax = std::max({v0.position.z, v1.position.z,
                                     v2.position.z, v3.position.z});

        const int section_idx = std::clamp(
            static_cast<int>(std::floor(ymin)) / kSectionHeight,
            0, kSectionsPerChunk - 1);
        SectionBuild& s = out[section_idx];

        const std::uint32_t base = static_cast<std::uint32_t>(s.vertices.size());
        s.vertices.push_back(v0);
        s.vertices.push_back(v1);
        s.vertices.push_back(v2);
        s.vertices.push_back(v3);
        // Remap the mesher's own index pattern instead of re-synthesizing
        // {0,1,2,0,2,3}: the greedy mesher flips the quad diagonal when one
        // AO pair is more contrasty (ao_flip), and re-synthesizing here was
        // silently discarding that choice for everything the game renders.
        for (int k = 0; k < 6; ++k) {
            const std::uint32_t src_idx = src.indices[6 * q + k];
            s.indices.push_back(base + (src_idx - static_cast<std::uint32_t>(4 * q)));
        }
        ++s.quad_count;

        const glm::vec3 lo{xmin + ox, ymin, zmin + oz};
        const glm::vec3 hi{xmax + ox, ymax, zmax + oz};
        if (!s.initialized) {
            s.aabb_min = lo;
            s.aabb_max = hi;
            s.initialized = true;
        } else {
            s.aabb_min = glm::min(s.aabb_min, lo);
            s.aabb_max = glm::max(s.aabb_max, hi);
        }
    }
    return out;
}

// Concatenate all non-empty section meshes into one vertex+index buffer
// per chunk and upload once. Each section keeps an (index_offset,
// index_count) slice into that shared EBO so culling and drawing happen
// at section granularity, but the GL upload + VAO bind happen at chunk
// granularity. Without this, each chunk does up to 8 glBufferData calls
// at load time and one glBindVertexArray per visible section at draw.
void apply_sections(ChunkSlot& slot,
                    std::array<SectionBuild, kSectionsPerChunk>&& built) {
    slot.any_section_has_mesh = false;
    slot.quad_count_total     = 0;
    bool union_init = false;

    std::vector<gfx::VertexPNT> all_vertices;
    std::vector<std::uint32_t>  all_indices;
    std::size_t reserved_v = 0, reserved_i = 0;
    for (const auto& s : built) { reserved_v += s.vertices.size(); reserved_i += s.indices.size(); }
    all_vertices.reserve(reserved_v);
    all_indices.reserve(reserved_i);

    for (int i = 0; i < kSectionsPerChunk; ++i) {
        auto& dst = slot.sections[i];
        auto& src = built[i];
        if (src.indices.empty()) {
            dst.has_mesh     = false;
            dst.quad_count   = 0;
            dst.index_offset = 0;
            dst.index_count  = 0;
            continue;
        }
        const std::uint32_t vert_base = static_cast<std::uint32_t>(all_vertices.size());
        const std::uint32_t idx_start = static_cast<std::uint32_t>(all_indices.size());
        all_vertices.insert(all_vertices.end(), src.vertices.begin(), src.vertices.end());
        for (auto idx : src.indices) all_indices.push_back(idx + vert_base);

        dst.aabb         = {src.aabb_min, src.aabb_max};
        dst.quad_count   = src.quad_count;
        dst.index_offset = idx_start;
        dst.index_count  = static_cast<std::uint32_t>(src.indices.size());
        dst.has_mesh     = true;
        slot.any_section_has_mesh = true;
        slot.quad_count_total += src.quad_count;

        if (!union_init) {
            slot.chunk_aabb = dst.aabb;
            union_init = true;
        } else {
            slot.chunk_aabb.min = glm::min(slot.chunk_aabb.min, dst.aabb.min);
            slot.chunk_aabb.max = glm::max(slot.chunk_aabb.max, dst.aabb.max);
        }
    }
    if (slot.any_section_has_mesh) {
        slot.chunk_mesh.upload(all_vertices, all_indices);
    }
    if (!union_init) {
        // Fully empty chunk - fall back to the block-extent AABB (returns a
        // zero-extent box; nothing will pass the cull test).
        slot.chunk_aabb = make_chunk_aabb(slot.coord, slot.chunk);
    }
}

}  // namespace

std::array<SectionBounds, kSectionsPerChunk>
compute_section_bounds(ChunkCoord coord, const Chunk& chunk) {
    auto mesh_data = build_chunk_mesh_greedy(chunk);
    auto built     = bucket_quads_by_section(mesh_data, coord);
    std::array<SectionBounds, kSectionsPerChunk> out;
    for (int i = 0; i < kSectionsPerChunk; ++i) {
        if (built[i].initialized) {
            out[i].aabb     = {built[i].aabb_min, built[i].aabb_max};
            out[i].has_mesh = true;
        }
    }
    return out;
}

namespace {

std::unique_ptr<ChunkSlot> build_slot(ChunkCoord coord, Chunk&& chunk,
                                      ChunkMeshData&& mesh_data,
                                      const SectionVisArray& visibility) {
    auto slot = std::make_unique<ChunkSlot>();
    slot->coord = coord;
    slot->chunk = std::move(chunk);
    slot->section_visibility = visibility;
    auto built = bucket_quads_by_section(mesh_data, coord);
    apply_sections(*slot, std::move(built));
    return slot;
}

}  // namespace

void World::generate_grid(int radius, const ColumnFiller& fill_column) {
    chunks_.clear();
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            Chunk chunk;
            const int origin_x = cx * kChunkSizeX;
            const int origin_z = cz * kChunkSizeZ;
            for (int z = 0; z < kChunkSizeZ; ++z) {
                for (int x = 0; x < kChunkSizeX; ++x) {
                    fill_column(origin_x + x, origin_z + z, chunk, x, z);
                }
            }
            auto mesh_data = build_chunk_mesh_greedy(chunk);
            auto vis = compute_section_visibility(chunk);
            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data), vis));
        }
    }
}

World::GenStats World::generate_grid(int radius, const TerrainGen& terrain) {
    using clock = std::chrono::steady_clock;
    GenStats stats;
    auto wall_t0 = clock::now();

    chunks_.clear();
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            Chunk chunk;
            auto gen_t0 = clock::now();
            terrain.fill_chunk(cx, cz, chunk);
            stats.gen_ms += std::chrono::duration<double, std::milli>(clock::now() - gen_t0).count();

            auto mesh_t0 = clock::now();
            auto mesh_data = build_chunk_mesh_greedy(chunk);
            stats.mesh_ms += std::chrono::duration<double, std::milli>(clock::now() - mesh_t0).count();

            auto vis = compute_section_visibility(chunk);
            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data), vis));
            ++stats.chunks_generated;
        }
    }

    stats.total_ms = std::chrono::duration<double, std::milli>(clock::now() - wall_t0).count();
    return stats;
}

void World::enqueue_grid_async(int radius, const TerrainGen& terrain,
                               core::ThreadPool& pool) {
    chunks_.clear();
    requested_.clear();
    {
        std::lock_guard<std::mutex> lock(finished_mutex_);
        std::queue<FinishedChunk> empty;
        finished_.swap(empty);
    }
    jobs_in_flight_.store(0);

    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            ChunkCoord c{cx, cz};
            requested_.insert(c);
            jobs_in_flight_.fetch_add(1);
            pool.submit([this, &terrain, c]() {
                ZoneScopedN("chunk_worker_job");
                using clock = std::chrono::steady_clock;
                const auto t0 = clock::now();
                FinishedChunk fc;
                fc.coord = c;
                terrain.fill_chunk(c.x, c.z, fc.chunk);
                const auto t_after_terrain = clock::now();
                fc.terrain_ms = std::chrono::duration<double, std::milli>(
                    t_after_terrain - t0).count();
                fc.mesh_data = build_chunk_mesh_greedy(fc.chunk);
                fc.visibility = compute_section_visibility(fc.chunk);
                fc.worker_ms = std::chrono::duration<double, std::milli>(
                    clock::now() - t0).count();
                std::lock_guard<std::mutex> lock(finished_mutex_);
                finished_.push(std::move(fc));
            });
        }
    }
}

World::StreamStats World::update_streaming(ChunkCoord center, int radius,
                                           const TerrainGen& terrain,
                                           core::ThreadPool& pool) {
    StreamStats stats;
    auto in_window = [&](ChunkCoord c) {
        return std::abs(c.x - center.x) <= radius
            && std::abs(c.z - center.z) <= radius;
    };

    for (auto it = chunks_.begin(); it != chunks_.end(); ) {
        if (!in_window(it->first)) { it = chunks_.erase(it); ++stats.evicted; }
        else ++it;
    }
    // In-flight jobs that fell out of window stay scheduled but their
    // results get dropped in drain_finished.
    for (auto it = requested_.begin(); it != requested_.end(); ) {
        if (!in_window(*it)) it = requested_.erase(it);
        else ++it;
    }

    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            ChunkCoord c{center.x + dx, center.z + dz};
            if (chunks_.count(c) || requested_.count(c)) continue;
            requested_.insert(c);
            jobs_in_flight_.fetch_add(1);
            ++stats.requested;
            pool.submit([this, &terrain, c]() {
                ZoneScopedN("chunk_worker_job");
                using clock = std::chrono::steady_clock;
                const auto t0 = clock::now();
                FinishedChunk fc;
                fc.coord = c;
                terrain.fill_chunk(c.x, c.z, fc.chunk);
                const auto t_after_terrain = clock::now();
                fc.terrain_ms = std::chrono::duration<double, std::milli>(
                    t_after_terrain - t0).count();
                fc.mesh_data = build_chunk_mesh_greedy(fc.chunk);
                fc.visibility = compute_section_visibility(fc.chunk);
                fc.worker_ms = std::chrono::duration<double, std::milli>(
                    clock::now() - t0).count();
                std::lock_guard<std::mutex> lock(finished_mutex_);
                finished_.push(std::move(fc));
            });
        }
    }
    return stats;
}

int World::drain_finished(int max_per_frame) {
    ZoneScopedN("drain_finished");
    using clock = std::chrono::steady_clock;
    int uploaded = 0;
    for (int i = 0; i < max_per_frame; ++i) {
        FinishedChunk fc;
        {
            std::lock_guard<std::mutex> lock(finished_mutex_);
            if (finished_.empty()) break;
            fc = std::move(finished_.front());
            finished_.pop();
        }

        jobs_in_flight_.fetch_sub(1);
        total_worker_ms_  += fc.worker_ms;
        total_terrain_ms_ += fc.terrain_ms;
        total_mesh_ms_    += fc.mesh_data.build_ms;

        auto req_it = requested_.find(fc.coord);
        if (req_it == requested_.end()) continue;  // evicted mid-flight
        requested_.erase(req_it);

        const auto up_t0 = clock::now();
        chunks_.emplace(fc.coord,
                        build_slot(fc.coord, std::move(fc.chunk), std::move(fc.mesh_data),
                                   fc.visibility));
        total_upload_ms_ += std::chrono::duration<double, std::milli>(
            clock::now() - up_t0).count();
        ++uploaded;
    }
    return uploaded;
}

int World::pending_async() const { return jobs_in_flight_.load(); }

void World::enqueue_decoded_chunk(ChunkCoord c, Chunk chunk,
                                  core::ThreadPool& pool) {
    requested_.insert(c);
    jobs_in_flight_.fetch_add(1);
    pool.submit([this, c, chunk = std::move(chunk)]() mutable {
        ZoneScopedN("chunk_loaded_worker_job");
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        FinishedChunk fc;
        fc.coord = c;
        fc.chunk = std::move(chunk);
        // terrain step is skipped on the load path; the chunk came off disk
        // already populated, so worker time is just the mesh build.
        fc.terrain_ms = 0.0;
        fc.mesh_data  = build_chunk_mesh_greedy(fc.chunk);
        fc.visibility = compute_section_visibility(fc.chunk);
        fc.worker_ms  = std::chrono::duration<double, std::milli>(
            clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(finished_mutex_);
        finished_.push(std::move(fc));
    });
}

void World::insert_chunk(ChunkCoord c, Chunk chunk) {
    auto mesh_data = build_chunk_mesh_greedy(chunk);
    auto vis = compute_section_visibility(chunk);
    chunks_[c] = build_slot(c, std::move(chunk), std::move(mesh_data), vis);
    requested_.erase(c);
}

void World::clear_all() {
    chunks_.clear();
    requested_.clear();
    std::lock_guard<std::mutex> lock(finished_mutex_);
    std::queue<FinishedChunk> empty;
    finished_.swap(empty);
}

void World::for_each_chunk(
    const std::function<void(ChunkCoord, const Chunk&)>& fn) const {
    for (const auto& kv : chunks_) fn(kv.first, kv.second->chunk);
}

BlockId World::block_at(int wx, int wy, int wz) const {
    if (wy < 0 || wy >= kChunkSizeY) return BlockId::Air;
    ChunkCoord cc{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    auto it = chunks_.find(cc);
    if (it == chunks_.end()) return BlockId::Air;
    return it->second->chunk.get(floor_mod(wx, kChunkSizeX), wy,
                                 floor_mod(wz, kChunkSizeZ));
}

bool World::set_block(int wx, int wy, int wz, BlockId b) {
    if (wy < 0 || wy >= kChunkSizeY) return false;
    ChunkCoord cc{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    auto it = chunks_.find(cc);
    if (it == chunks_.end()) return false;

    ChunkSlot& slot = *it->second;
    int lx = floor_mod(wx, kChunkSizeX);
    int lz = floor_mod(wz, kChunkSizeZ);
    if (slot.chunk.get(lx, wy, lz) == b) return false;

    slot.chunk.set(lx, wy, lz, b);
    auto mesh_data = build_chunk_mesh_greedy(slot.chunk);
    // Edits can shift quads across section boundaries (placing a block on
    // top of a tall column, breaking the lowest solid in a section), so
    // every section is re-bucketed and the chunk_aabb gets rebuilt. Greedy
    // meshing on a 16x256x16 chunk is sub-millisecond, so doing it again
    // per edit is fine.
    auto built = bucket_quads_by_section(mesh_data, slot.coord);
    apply_sections(slot, std::move(built));
    slot.section_visibility = compute_section_visibility(slot.chunk);
    return true;
}

// Amanatides-Woo DDA. Steps one voxel along whichever axis next crosses
// an integer plane.
World::RayHit World::raycast(const glm::vec3& origin, const glm::vec3& dir,
                             float max_distance) const {
    RayHit hit;
    glm::vec3 d = dir;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) return hit;
    d /= std::sqrt(len2);

    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    int step_x = (d.x > 0) ? 1 : (d.x < 0 ? -1 : 0);
    int step_y = (d.y > 0) ? 1 : (d.y < 0 ? -1 : 0);
    int step_z = (d.z > 0) ? 1 : (d.z < 0 ? -1 : 0);

    auto next_boundary = [](float p, int step) {
        if (step > 0) return std::floor(p) + 1.0f;
        if (step < 0) return std::floor(p);
        return p;
    };

    constexpr float kInf = std::numeric_limits<float>::infinity();
    float t_max_x = step_x ? (next_boundary(origin.x, step_x) - origin.x) / d.x : kInf;
    float t_max_y = step_y ? (next_boundary(origin.y, step_y) - origin.y) / d.y : kInf;
    float t_max_z = step_z ? (next_boundary(origin.z, step_z) - origin.z) / d.z : kInf;
    float t_delta_x = step_x ? std::abs(1.0f / d.x) : kInf;
    float t_delta_y = step_y ? std::abs(1.0f / d.y) : kInf;
    float t_delta_z = step_z ? std::abs(1.0f / d.z) : kInf;

    float t = 0.0f;
    int last_axis = -1;

    while (t <= max_distance) {
        if (is_solid(block_at(x, y, z))) {
            hit.hit = true;
            hit.block_x = x; hit.block_y = y; hit.block_z = z;
            hit.distance = t;
            if      (last_axis == 0) hit.nx = -step_x;
            else if (last_axis == 1) hit.ny = -step_y;
            else if (last_axis == 2) hit.nz = -step_z;
            return hit;
        }
        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            t = t_max_x; t_max_x += t_delta_x; x += step_x; last_axis = 0;
        } else if (t_max_y < t_max_z) {
            t = t_max_y; t_max_y += t_delta_y; y += step_y; last_axis = 1;
        } else {
            t = t_max_z; t_max_z += t_delta_z; z += step_z; last_axis = 2;
        }
    }
    return hit;
}

void World::debug_dump_visibility(const gfx::Frustum& frustum) const {
    int drawn = 0;
    for (const auto& kv : chunks_) {
        const ChunkSlot& s = *kv.second;
        bool vis = frustum.intersects_aabb(s.chunk_aabb);
        if (vis) ++drawn;
        int meshed = 0;
        for (const auto& sec : s.sections) meshed += sec.has_mesh ? 1 : 0;
        std::printf("  chunk (%+3d,%+3d)  %s  aabb y[%.0f..%.0f] xz[%.0f,%.0f..%.0f,%.0f] sections=%d solids=%d\n",
                    s.coord.x, s.coord.z, vis ? "VISIBLE" : "culled",
                    s.chunk_aabb.min.y, s.chunk_aabb.max.y,
                    s.chunk_aabb.min.x, s.chunk_aabb.min.z,
                    s.chunk_aabb.max.x, s.chunk_aabb.max.z,
                    meshed, s.chunk.solid_count());
    }
    std::printf("  total visible: %d / %zu\n", drawn, chunks_.size());
}

namespace {

// True if section `i` of a chunk should draw given the chunk's reachable
// mask. Greedy quads bucket by their bottom Y, so a section's AABB can
// extend above its own slab — the section must draw if ANY slab its AABB
// spans is reachable, or a tall cliff face would vanish when only its
// upper half is in view.
bool section_reachable(std::uint8_t mask, int i, const gfx::AABB& aabb) {
    int hi = static_cast<int>(std::floor(aabb.max.y - 0.001f)) / kSectionHeight;
    hi = std::clamp(hi, i, kSectionsPerChunk - 1);
    for (int s = i; s <= hi; ++s) {
        if (mask & (1u << s)) return true;
    }
    return false;
}

bool section_box_in_frustum(const gfx::Frustum& frustum, ChunkCoord c, int sy) {
    const float ox = static_cast<float>(c.x * kChunkSizeX);
    const float oz = static_cast<float>(c.z * kChunkSizeZ);
    const float oy = static_cast<float>(sy * kSectionHeight);
    return frustum.intersects_aabb({{ox, oy, oz},
                                    {ox + kChunkSizeX, oy + kSectionHeight,
                                     oz + kChunkSizeZ}});
}

}  // namespace

bool section_reachable_in_mask(std::uint8_t mask, int sy, const gfx::AABB& aabb) {
    return section_reachable(mask, sy, aabb);
}

bool occlusion_bfs(
    const glm::vec3& camera_pos,
    const gfx::Frustum& frustum,
    const std::function<const SectionVisArray*(ChunkCoord)>& visibility_of,
    SectionReachableMap& reachable) {
    const int wx = static_cast<int>(std::floor(camera_pos.x));
    const int wz = static_cast<int>(std::floor(camera_pos.z));
    const ChunkCoord start{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    if (!visibility_of(start)) return false;

    // Clamping Y keeps a camera above the build limit (or below bedrock)
    // working: it seeds from the nearest section with unconstrained exits,
    // which is conservative.
    const int start_sy = std::clamp(
        static_cast<int>(std::floor(camera_pos.y)) / kSectionHeight,
        0, kSectionsPerChunk - 1);

    struct Node {
        ChunkCoord   c;
        std::int8_t  sy;
        std::int8_t  entry_face;  // face of THIS section we entered through; -1 at seed
        std::uint8_t dirs;        // directions taken on the path so far
    };
    std::vector<Node> queue;
    queue.reserve(512);

    // Frustum-test is skipped for the seed: the camera sits inside it.
    reachable[start] |= static_cast<std::uint8_t>(1u << start_sy);
    queue.push_back({start, static_cast<std::int8_t>(start_sy), -1, 0});

    std::size_t head = 0;
    while (head < queue.size()) {
        const Node n = queue[head++];
        const SectionVisArray* vis = visibility_of(n.c);  // non-null: checked at enqueue

        for (int d = 0; d < 6; ++d) {
            // Never step back along an axis direction the path already used
            // in reverse — stops sightlines that would have to bend around
            // a corner and come back.
            if (n.dirs & (1u << opposite_face(d))) continue;
            if (n.entry_face >= 0 &&
                !faces_connected((*vis)[n.sy], n.entry_face, d)) continue;

            ChunkCoord nc = n.c;
            int nsy = n.sy;
            switch (d) {
                case kFaceNegX: nc.x -= 1; break;
                case kFacePosX: nc.x += 1; break;
                case kFaceNegY: nsy -= 1;  break;
                case kFacePosY: nsy += 1;  break;
                case kFaceNegZ: nc.z -= 1; break;
                case kFacePosZ: nc.z += 1; break;
            }
            if (nsy < 0 || nsy >= kSectionsPerChunk) continue;
            if (!visibility_of(nc)) continue;

            auto& mask = reachable[nc];
            const auto bit = static_cast<std::uint8_t>(1u << nsy);
            if (mask & bit) continue;
            if (!section_box_in_frustum(frustum, nc, nsy)) continue;
            mask |= bit;
            queue.push_back({nc, static_cast<std::int8_t>(nsy),
                             static_cast<std::int8_t>(opposite_face(d)),
                             static_cast<std::uint8_t>(n.dirs | (1u << d))});
        }
    }
    return true;
}

int World::debug_validate_gpu_meshes() const {
    std::vector<gfx::VertexPNT> verts;
    std::vector<std::uint32_t> idx;
    int bad = 0;
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.any_section_has_mesh) continue;
        slot.chunk_mesh.debug_read_back(verts, idx);
        for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
            const auto& p0 = verts[idx[t]].position;
            const auto& p1 = verts[idx[t + 1]].position;
            const auto& p2 = verts[idx[t + 2]].position;
            const glm::vec3 n = verts[idx[t]].normal;
            const int d = (std::abs(n.x) > 0.5f) ? 0 : (std::abs(n.y) > 0.5f ? 1 : 2);
            const bool coplanar = (p0[d] == p1[d]) && (p1[d] == p2[d]);
            bool backed = false;
            if (coplanar) {
                const int s = static_cast<int>(std::lround(p0[d]));
                const int u_axis = (d + 1) % 3, v_axis = (d + 2) % 3;
                const int u0 = static_cast<int>(std::floor(std::min({p0[u_axis], p1[u_axis], p2[u_axis]})));
                const int u1 = static_cast<int>(std::ceil (std::max({p0[u_axis], p1[u_axis], p2[u_axis]})));
                const int v0 = static_cast<int>(std::floor(std::min({p0[v_axis], p1[v_axis], p2[v_axis]})));
                const int v1 = static_cast<int>(std::ceil (std::max({p0[v_axis], p1[v_axis], p2[v_axis]})));
                for (int u = u0; u < u1 && !backed; ++u) {
                    for (int v = v0; v < v1 && !backed; ++v) {
                        int cell[3];
                        cell[u_axis] = u; cell[v_axis] = v;
                        cell[d] = (n[d] > 0.0f) ? s - 1 : s;
                        if (in_chunk_bounds(cell[0], cell[1], cell[2]) &&
                            is_solid(slot.chunk.get(cell[0], cell[1], cell[2]))) backed = true;
                    }
                }
            }
            if (!coplanar || !backed) {
                ++bad;
                if (bad <= 16) {
                    std::printf("[validate] chunk(%+d,%+d) tri %zu %s: "
                                "(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f)(%.1f,%.1f,%.1f) n=(%.0f,%.0f,%.0f) ids=%u,%u,%u\n",
                                slot.coord.x, slot.coord.z, t / 3,
                                coplanar ? "unbacked" : "NON-COPLANAR",
                                p0.x, p0.y, p0.z, p1.x, p1.y, p1.z, p2.x, p2.y, p2.z,
                                n.x, n.y, n.z, idx[t], idx[t+1], idx[t+2]);
                }
            }
        }
    }
    std::printf("[validate] GPU mesh triangles flagged: %d\n", bad);
    return bad;
}

DrawStats World::draw_impl(const gfx::Frustum& frustum,
                           const SectionReachableMap* reachable,
                           const std::function<void(const glm::mat4&)>& set_model) const {
    DrawStats stats;
    stats.chunks_total   = static_cast<int>(chunks_.size());
    stats.sections_total = stats.chunks_total * kSectionsPerChunk;
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.any_section_has_mesh) continue;
        if (!frustum.intersects_aabb(slot.chunk_aabb)) continue;

        std::uint8_t reach_mask = 0xFF;
        if (reachable) {
            auto it = reachable->find(kv.first);
            reach_mask = (it != reachable->end()) ? it->second : 0;
        }

        const float ox = static_cast<float>(slot.coord.x * kChunkSizeX);
        const float oz = static_cast<float>(slot.coord.z * kChunkSizeZ);
        glm::mat4 model = glm::translate(glm::mat4(1.0f), {ox, 0.0f, oz});
        bool vao_bound = false;
        bool drew_any  = false;
        for (int i = 0; i < kSectionsPerChunk; ++i) {
            const auto& sec = slot.sections[i];
            if (!sec.has_mesh) continue;
            if (!frustum.intersects_aabb(sec.aabb)) continue;
            if (reachable && !section_reachable(reach_mask, i, sec.aabb)) {
                ++stats.sections_occluded;
                continue;
            }
            if (!vao_bound) {
                set_model(model);
                slot.chunk_mesh.bind();
                vao_bound = true;
            }
            slot.chunk_mesh.draw_range_bound(sec.index_offset, sec.index_count);
            ++stats.sections_drawn;
            stats.triangles_drawn += sec.index_count / 3;
            drew_any = true;
        }
        if (drew_any) ++stats.chunks_drawn;
    }
    return stats;
}

DrawStats World::draw_visible(const gfx::Frustum& frustum,
                              const gfx::Shader& shader) const {
    return draw_impl(frustum, nullptr,
        [&](const glm::mat4& m) { shader.set_mat4("u_model", m); });
}

DrawStats World::draw_visible_with(const gfx::Frustum& frustum,
    std::function<void(const glm::mat4& model)> set_model) const {
    return draw_impl(frustum, nullptr, set_model);
}

DrawStats World::draw_visible_occluded(const gfx::Frustum& frustum,
                                       const glm::vec3& camera_pos,
                                       const gfx::Shader& shader) const {
    ZoneScopedN("occlusion_bfs");
    SectionReachableMap reachable;
    const bool ok = occlusion_bfs(
        camera_pos, frustum,
        [this](ChunkCoord c) -> const SectionVisArray* {
            auto it = chunks_.find(c);
            return it == chunks_.end() ? nullptr
                                       : &it->second->section_visibility;
        },
        reachable);
    if (!ok) return draw_visible(frustum, shader);
    return draw_impl(frustum, &reachable,
        [&](const glm::mat4& m) { shader.set_mat4("u_model", m); });
}

}  // namespace world
