#include "world/world.h"

#include "world/chunk_mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

namespace {

// Floor-division by `n`: result floored toward -inf, matching the
// chunk grid intuition (block x=-1 is in chunk -1, not chunk 0).
constexpr int floor_div(int a, int n) {
    int q = a / n;
    int r = a % n;
    if ((r != 0) && ((r < 0) != (n < 0))) --q;
    return q;
}

// Euclidean modulo (always in [0, n)).
constexpr int floor_mod(int a, int n) {
    int r = a % n;
    if (r < 0) r += n;
    return r;
}

}  // namespace

namespace world {

void World::generate_grid(int radius, const ColumnFiller& fill_column) {
    chunks_.clear();
    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            auto slot = std::make_unique<ChunkSlot>();
            slot->coord = {cx, cz};

            const int origin_x = cx * kChunkSizeX;
            const int origin_z = cz * kChunkSizeZ;

            for (int z = 0; z < kChunkSizeZ; ++z) {
                for (int x = 0; x < kChunkSizeX; ++x) {
                    fill_column(origin_x + x, origin_z + z, slot->chunk, x, z);
                }
            }

            auto mesh_data = build_chunk_mesh_greedy(slot->chunk);
            if (!mesh_data.indices.empty()) {
                slot->mesh.upload(mesh_data.vertices, mesh_data.indices);
                slot->has_mesh = true;
            }
            slot->quad_count = mesh_data.quad_count;

            slot->aabb.min = {static_cast<float>(origin_x),
                              0.0f,
                              static_cast<float>(origin_z)};
            slot->aabb.max = {static_cast<float>(origin_x + kChunkSizeX),
                              static_cast<float>(kChunkSizeY),
                              static_cast<float>(origin_z + kChunkSizeZ)};

            chunks_.emplace(slot->coord, std::move(slot));
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
            auto slot = std::make_unique<ChunkSlot>();
            slot->coord = {cx, cz};

            auto gen_t0 = clock::now();
            terrain.fill_chunk(cx, cz, slot->chunk);
            auto gen_t1 = clock::now();
            stats.gen_ms += std::chrono::duration<double, std::milli>(gen_t1 - gen_t0).count();

            auto mesh_t0 = clock::now();
            auto mesh_data = build_chunk_mesh_greedy(slot->chunk);
            auto mesh_t1 = clock::now();
            stats.mesh_ms += std::chrono::duration<double, std::milli>(mesh_t1 - mesh_t0).count();

            if (!mesh_data.indices.empty()) {
                slot->mesh.upload(mesh_data.vertices, mesh_data.indices);
                slot->has_mesh = true;
            }
            slot->quad_count = mesh_data.quad_count;

            const int origin_x = cx * kChunkSizeX;
            const int origin_z = cz * kChunkSizeZ;
            slot->aabb.min = {static_cast<float>(origin_x),
                              0.0f,
                              static_cast<float>(origin_z)};
            slot->aabb.max = {static_cast<float>(origin_x + kChunkSizeX),
                              static_cast<float>(kChunkSizeY),
                              static_cast<float>(origin_z + kChunkSizeZ)};

            chunks_.emplace(slot->coord, std::move(slot));
            ++stats.chunks_generated;
        }
    }

    auto wall_t1 = clock::now();
    stats.total_ms = std::chrono::duration<double, std::milli>(wall_t1 - wall_t0).count();
    return stats;
}

void World::enqueue_grid_async(int radius, const TerrainGen& terrain,
                               core::ThreadPool& pool) {
    chunks_.clear();
    {
        std::lock_guard<std::mutex> lock(finished_mutex_);
        std::queue<FinishedChunk> empty;
        finished_.swap(empty);
    }
    jobs_in_flight_.store(0);

    for (int cz = -radius; cz <= radius; ++cz) {
        for (int cx = -radius; cx <= radius; ++cx) {
            jobs_in_flight_.fetch_add(1);
            // Capture by value: terrain ref is fine (stateless), cx/cz are
            // ints. The worker writes a FinishedChunk into finished_.
            pool.submit([this, &terrain, cx, cz]() {
                FinishedChunk fc;
                fc.coord = {cx, cz};
                terrain.fill_chunk(cx, cz, fc.chunk);
                fc.mesh_data = build_chunk_mesh_greedy(fc.chunk);
                {
                    std::lock_guard<std::mutex> lock(finished_mutex_);
                    finished_.push(std::move(fc));
                }
            });
        }
    }
}

int World::drain_finished(int max_per_frame) {
    int uploaded = 0;
    for (int i = 0; i < max_per_frame; ++i) {
        FinishedChunk fc;
        {
            std::lock_guard<std::mutex> lock(finished_mutex_);
            if (finished_.empty()) break;
            fc = std::move(finished_.front());
            finished_.pop();
        }

        auto slot = std::make_unique<ChunkSlot>();
        slot->coord = fc.coord;
        slot->chunk = std::move(fc.chunk);
        slot->quad_count = fc.mesh_data.quad_count;
        if (!fc.mesh_data.indices.empty()) {
            slot->mesh.upload(fc.mesh_data.vertices, fc.mesh_data.indices);
            slot->has_mesh = true;
        }

        const int origin_x = fc.coord.x * kChunkSizeX;
        const int origin_z = fc.coord.z * kChunkSizeZ;
        slot->aabb.min = {static_cast<float>(origin_x),
                          0.0f,
                          static_cast<float>(origin_z)};
        slot->aabb.max = {static_cast<float>(origin_x + kChunkSizeX),
                          static_cast<float>(kChunkSizeY),
                          static_cast<float>(origin_z + kChunkSizeZ)};

        chunks_.emplace(slot->coord, std::move(slot));
        jobs_in_flight_.fetch_sub(1);
        ++uploaded;
    }
    return uploaded;
}

int World::pending_async() const {
    return jobs_in_flight_.load();
}

BlockId World::block_at(int wx, int wy, int wz) const {
    if (wy < 0 || wy >= kChunkSizeY) return BlockId::Air;
    ChunkCoord cc{floor_div(wx, kChunkSizeX), floor_div(wz, kChunkSizeZ)};
    auto it = chunks_.find(cc);
    if (it == chunks_.end()) return BlockId::Air;
    int lx = floor_mod(wx, kChunkSizeX);
    int lz = floor_mod(wz, kChunkSizeZ);
    return it->second->chunk.get(lx, wy, lz);
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

    // Re-mesh this chunk on the main thread (simple and correct; the
    // edit volume is small enough that a full chunk re-mesh is well
    // under 2 ms on the bench).
    auto mesh_data = build_chunk_mesh_greedy(slot.chunk);
    slot.mesh.upload(mesh_data.vertices, mesh_data.indices);
    slot.has_mesh = !mesh_data.indices.empty();
    slot.quad_count = mesh_data.quad_count;

    return true;
}

World::RayHit World::raycast(const glm::vec3& origin, const glm::vec3& dir,
                             float max_distance) const {
    RayHit hit;
    // Direction must be non-zero. Caller normally hands us a unit
    // vector but we don't assume.
    glm::vec3 d = dir;
    float len2 = glm::dot(d, d);
    if (len2 < 1e-12f) return hit;
    d /= std::sqrt(len2);

    // Amanatides-Woo voxel traversal. Step one block at a time along
    // whichever axis hits its next integer plane first.
    int x = static_cast<int>(std::floor(origin.x));
    int y = static_cast<int>(std::floor(origin.y));
    int z = static_cast<int>(std::floor(origin.z));

    int step_x = (d.x > 0) ? 1 : (d.x < 0 ? -1 : 0);
    int step_y = (d.y > 0) ? 1 : (d.y < 0 ? -1 : 0);
    int step_z = (d.z > 0) ? 1 : (d.z < 0 ? -1 : 0);

    auto next_boundary = [](float p, int step) -> float {
        if (step > 0) return std::floor(p) + 1.0f;
        if (step < 0) return std::floor(p);
        return p;  // unused when t_delta is +inf
    };

    constexpr float kInf = std::numeric_limits<float>::infinity();

    float t_max_x = (step_x != 0) ? (next_boundary(origin.x, step_x) - origin.x) / d.x : kInf;
    float t_max_y = (step_y != 0) ? (next_boundary(origin.y, step_y) - origin.y) / d.y : kInf;
    float t_max_z = (step_z != 0) ? (next_boundary(origin.z, step_z) - origin.z) / d.z : kInf;

    float t_delta_x = (step_x != 0) ? std::abs(1.0f / d.x) : kInf;
    float t_delta_y = (step_y != 0) ? std::abs(1.0f / d.y) : kInf;
    float t_delta_z = (step_z != 0) ? std::abs(1.0f / d.z) : kInf;

    float t = 0.0f;
    int last_axis = -1;  // 0=x, 1=y, 2=z — the axis we just crossed

    while (t <= max_distance) {
        BlockId b = block_at(x, y, z);
        if (is_solid(b)) {
            hit.hit = true;
            hit.block_x = x; hit.block_y = y; hit.block_z = z;
            hit.distance = t;
            if (last_axis == 0) { hit.nx = -step_x; }
            else if (last_axis == 1) { hit.ny = -step_y; }
            else if (last_axis == 2) { hit.nz = -step_z; }
            return hit;
        }

        // Advance along whichever axis is closest to its next plane.
        if (t_max_x < t_max_y && t_max_x < t_max_z) {
            t = t_max_x;
            t_max_x += t_delta_x;
            x += step_x;
            last_axis = 0;
        } else if (t_max_y < t_max_z) {
            t = t_max_y;
            t_max_y += t_delta_y;
            y += step_y;
            last_axis = 1;
        } else {
            t = t_max_z;
            t_max_z += t_delta_z;
            z += step_z;
            last_axis = 2;
        }
    }
    return hit;
}

void World::debug_dump_visibility(const gfx::Frustum& frustum) const {
    int drawn = 0;
    for (const auto& kv : chunks_) {
        const ChunkSlot& s = *kv.second;
        bool vis = frustum.intersects_aabb(s.aabb);
        if (vis) ++drawn;
        std::printf("  chunk (%+3d,%+3d) aabb=[%.0f..%.0f, %.0f..%.0f, %.0f..%.0f]  %s\n",
                    s.coord.x, s.coord.z,
                    s.aabb.min.x, s.aabb.max.x,
                    s.aabb.min.y, s.aabb.max.y,
                    s.aabb.min.z, s.aabb.max.z,
                    vis ? "VISIBLE" : "culled");
    }
    std::printf("  total visible: %d / %zu\n", drawn, chunks_.size());
}

DrawStats World::draw_visible(const gfx::Frustum& frustum,
                              const gfx::Shader& shader) const {
    DrawStats stats;
    stats.chunks_total = static_cast<int>(chunks_.size());
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.has_mesh) continue;
        if (!frustum.intersects_aabb(slot.aabb)) continue;

        glm::mat4 model = glm::translate(
            glm::mat4(1.0f),
            glm::vec3(slot.aabb.min.x, 0.0f, slot.aabb.min.z));
        shader.set_mat4("u_model", model);
        slot.mesh.draw();

        ++stats.chunks_drawn;
        stats.triangles_drawn += slot.mesh.index_count() / 3;
    }
    return stats;
}

}  // namespace world
