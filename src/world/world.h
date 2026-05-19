#pragma once

#include "gfx/frustum.h"
#include "gfx/mesh.h"
#include "gfx/shader.h"
#include "world/chunk.h"
#include "world/terrain_gen.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

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

    // Draw every chunk whose AABB intersects the frustum. Caller is
    // responsible for shader.use() and uniforms that don't change per chunk
    // (view, proj, light, texture). We set u_model per chunk.
    DrawStats draw_visible(const gfx::Frustum& frustum, const gfx::Shader& shader) const;

    // Diagnostic: print every chunk's coord, AABB, frustum result. Used
    // to verify the culling math, not called per frame.
    void debug_dump_visibility(const gfx::Frustum& frustum) const;

    std::size_t chunk_count() const { return chunks_.size(); }

private:
    std::unordered_map<ChunkCoord, std::unique_ptr<ChunkSlot>, ChunkCoordHash> chunks_;
};

}  // namespace world
