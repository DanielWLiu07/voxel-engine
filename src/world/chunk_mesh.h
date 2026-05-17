#pragma once

#include "gfx/mesh.h"
#include "world/chunk.h"

#include <cstdint>
#include <vector>

namespace world {

struct ChunkMeshData {
    std::vector<gfx::VertexPNT> vertices;
    std::vector<std::uint32_t>  indices;
    // Diagnostics used by the perf HUD and benchmarks.
    int quad_count = 0;
    double build_ms = 0.0;
};

// Naive mesher: for every solid block, emit a quad per face whose
// neighbor is non-solid. No merging across blocks. This is the slow
// baseline the greedy mesher will be compared against.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk);

}  // namespace world
