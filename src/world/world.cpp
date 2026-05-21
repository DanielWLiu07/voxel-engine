#include "world/world.h"

#include "world/chunk_mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>

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

gfx::AABB make_chunk_aabb(ChunkCoord c) {
    const float ox = static_cast<float>(c.x * kChunkSizeX);
    const float oz = static_cast<float>(c.z * kChunkSizeZ);
    return {{ox, 0.0f, oz},
            {ox + kChunkSizeX, static_cast<float>(kChunkSizeY), oz + kChunkSizeZ}};
}

std::unique_ptr<ChunkSlot> build_slot(ChunkCoord coord, Chunk&& chunk, ChunkMeshData&& mesh_data) {
    auto slot = std::make_unique<ChunkSlot>();
    slot->coord = coord;
    slot->chunk = std::move(chunk);
    slot->quad_count = mesh_data.quad_count;
    if (!mesh_data.indices.empty()) {
        slot->mesh.upload(mesh_data.vertices, mesh_data.indices);
        slot->has_mesh = true;
    }
    slot->aabb = make_chunk_aabb(coord);
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
            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data)));
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

            ChunkCoord c{cx, cz};
            chunks_.emplace(c, build_slot(c, std::move(chunk), std::move(mesh_data)));
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
                FinishedChunk fc;
                fc.coord = c;
                terrain.fill_chunk(c.x, c.z, fc.chunk);
                fc.mesh_data = build_chunk_mesh_greedy(fc.chunk);
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
                FinishedChunk fc;
                fc.coord = c;
                terrain.fill_chunk(c.x, c.z, fc.chunk);
                fc.mesh_data = build_chunk_mesh_greedy(fc.chunk);
                std::lock_guard<std::mutex> lock(finished_mutex_);
                finished_.push(std::move(fc));
            });
        }
    }
    return stats;
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

        jobs_in_flight_.fetch_sub(1);

        auto req_it = requested_.find(fc.coord);
        if (req_it == requested_.end()) continue;  // evicted mid-flight
        requested_.erase(req_it);

        chunks_.emplace(fc.coord,
                        build_slot(fc.coord, std::move(fc.chunk), std::move(fc.mesh_data)));
        ++uploaded;
    }
    return uploaded;
}

int World::pending_async() const { return jobs_in_flight_.load(); }

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
    slot.mesh.upload(mesh_data.vertices, mesh_data.indices);
    slot.has_mesh = !mesh_data.indices.empty();
    slot.quad_count = mesh_data.quad_count;
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
        bool vis = frustum.intersects_aabb(s.aabb);
        if (vis) ++drawn;
        std::printf("  chunk (%+3d,%+3d)  %s\n",
                    s.coord.x, s.coord.z, vis ? "VISIBLE" : "culled");
    }
    std::printf("  total visible: %d / %zu\n", drawn, chunks_.size());
}

DrawStats World::draw_visible(const gfx::Frustum& frustum,
                              const gfx::Shader& shader) const {
    DrawStats stats;
    stats.chunks_total = static_cast<int>(chunks_.size());
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.has_mesh || !frustum.intersects_aabb(slot.aabb)) continue;
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(slot.aabb.min.x, 0.0f, slot.aabb.min.z));
        shader.set_mat4("u_model", model);
        slot.mesh.draw();
        ++stats.chunks_drawn;
        stats.triangles_drawn += slot.mesh.index_count() / 3;
    }
    return stats;
}

DrawStats World::draw_visible_with(const gfx::Frustum& frustum,
    std::function<void(const glm::mat4& model)> set_model) const {
    DrawStats stats;
    stats.chunks_total = static_cast<int>(chunks_.size());
    for (const auto& kv : chunks_) {
        const ChunkSlot& slot = *kv.second;
        if (!slot.has_mesh || !frustum.intersects_aabb(slot.aabb)) continue;
        glm::mat4 model = glm::translate(glm::mat4(1.0f),
            glm::vec3(slot.aabb.min.x, 0.0f, slot.aabb.min.z));
        set_model(model);
        slot.mesh.draw();
        ++stats.chunks_drawn;
        stats.triangles_drawn += slot.mesh.index_count() / 3;
    }
    return stats;
}

}  // namespace world
