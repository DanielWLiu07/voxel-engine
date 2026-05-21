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

    void debug_dump_visibility(const gfx::Frustum& frustum) const;

    std::size_t chunk_count() const { return chunks_.size(); }

private:
    struct FinishedChunk {
        ChunkCoord    coord;
        Chunk         chunk;
        ChunkMeshData mesh_data;
    };

    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkSlot>, ChunkCoordHash> chunks_;
    std::unordered_set<ChunkCoord, ChunkCoordHash> requested_;

    mutable std::mutex                 finished_mutex_;
    std::queue<FinishedChunk>          finished_;
    std::atomic<int>                   jobs_in_flight_{0};
};

}  // namespace world
