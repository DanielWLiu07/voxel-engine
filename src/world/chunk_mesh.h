#pragma once

#include "gfx/mesh.h"
#include "world/chunk.h"

#include <array>
#include <cstddef>
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

// The four horizontally adjacent chunks, when they are known.
//
// Without these, a chunk is meshed as if surrounded by air, so every face
// on its four vertical boundaries is emitted even where the neighbour has
// solid rock pressed against it. Those faces can never be seen and are
// still uploaded, drawn, and counted in the merge ratio.
//
// A null pointer means "not known", and is treated as air: the face is
// emitted. That is the safe direction. Guessing solid would cull a face
// that might be visible, which is a hole in the world; guessing air only
// costs a wasted quad, and the chunk is re-meshed once the neighbour
// arrives.
//
// Y has no entry because chunks span the full world height: above and
// below a chunk really is air.
struct NeighborChunks {
    const Chunk* neg_x = nullptr;
    const Chunk* pos_x = nullptr;
    const Chunk* neg_z = nullptr;
    const Chunk* pos_z = nullptr;

    bool any() const {
        return neg_x || pos_x || neg_z || pos_z;
    }
};

// One layer of blocks copied off a neighbouring chunk: the slab pressed
// against this chunk's boundary.
//
// That layer is everything the mesher reads outside its own chunk. Face
// visibility looks exactly one cell across a face, ambient occlusion
// samples the same outside layer, and the merge sweep never steps further
// than one slice past either end. So a copy of it meshes identically to
// holding the whole neighbour - and a copy is what lets meshing run on a
// worker thread while the main thread owns the chunk map and is free to
// evict any chunk it likes.
//
// 4 KB per side, 16 KB for all four, against a 64 KB chunk.
struct BoundaryPlane {
    bool present = false;
    // Indexed by (t, y), where t runs along the boundary: z for the two
    // X-facing planes, x for the two Z-facing ones.
    std::array<std::uint8_t, kChunkSizeY * kChunkSizeX> blocks{};

    BlockId at(int t, int y) const {
        return static_cast<BlockId>(blocks[static_cast<std::size_t>(y) *
                                           kChunkSizeX + t]);
    }
    void set(int t, int y, BlockId b) {
        blocks[static_cast<std::size_t>(y) * kChunkSizeX + t] =
            static_cast<std::uint8_t>(b);
    }
};

struct NeighborPlanes {
    BoundaryPlane neg_x, pos_x, neg_z, pos_z;

    // Copies the four boundary layers out of live chunks. Call this on the
    // thread that owns them; the result is safe to hand anywhere.
    static NeighborPlanes from(const NeighborChunks& n);

    bool any() const {
        return neg_x.present || pos_x.present ||
               neg_z.present || pos_z.present;
    }
};

// Reads a block in chunk-local coordinates, following the neighbour links
// when the coordinate leaves the chunk. Exposed for tests.
BlockId sample_with_neighbors(const Chunk& chunk, const NeighborPlanes& n,
                              int x, int y, int z);

// One quad per visible face. The slow baseline.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk,
                                     const NeighborPlanes& neighbors = {});

// Slice-sweep + maximal-rectangle merge. Typical 20-50x reduction on
// terrain-like data.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk,
                                      const NeighborPlanes& neighbors = {});

}  // namespace world
