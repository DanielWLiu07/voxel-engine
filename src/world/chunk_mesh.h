#pragma once

#include "gfx/mesh.h"
#include "world/chunk.h"

#include <cstdint>
#include <vector>

namespace world {

// Four vertices per quad, in draw order. Triangulation is implicit: every
// quad uses the shared {0,1,2},{0,2,3} pattern (gfx::QuadIndexBuffer), so
// meshes carry no index data at all. The greedy mesher's AO diagonal flip
// is encoded by rotating the quad's vertex order at emit time.
struct ChunkMeshData {
    std::vector<gfx::VertexPacked> vertices;
    int    quad_count = 0;
    double build_ms = 0.0;
};

// One quad per visible face. The slow baseline.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk);

// Slice-sweep + maximal-rectangle merge. Typical 20-50x reduction on
// terrain-like data.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk);

}  // namespace world
