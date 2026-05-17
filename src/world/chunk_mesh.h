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
// baseline the greedy mesher is benchmarked against.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk);

// Greedy mesher: sweeps each of 6 face directions one slice at a time,
// builds a 2D visibility mask, and merges the largest axis-aligned
// rectangle of matching face types per quad. Typical reduction on
// terrain-like data is 20-50x fewer quads vs. naive.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk);

}  // namespace world
