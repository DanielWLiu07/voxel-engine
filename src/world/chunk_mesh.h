#pragma once

#include "gfx/mesh.h"
#include "world/chunk.h"

#include <cstdint>
#include <vector>

namespace world {

struct ChunkMeshData {
    std::vector<gfx::VertexPNT> vertices;
    std::vector<std::uint32_t>  indices;
    int    quad_count = 0;
    double build_ms = 0.0;
};

// One quad per visible face. The slow baseline.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk);

// Slice-sweep + maximal-rectangle merge. Typical 20-50x reduction on
// terrain-like data.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk);

}  // namespace world
