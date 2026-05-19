#include "world/world.h"

#include "world/chunk_mesh.h"

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdio>

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
