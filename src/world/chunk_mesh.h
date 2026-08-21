#pragma once

#include "gfx/mesh.h"
#include "world/chunk.h"
#include "world/chunk_light.h"

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

// Which mesher the world builds with.
//
// Naive exists in the engine, not just the benchmark, so the difference
// the greedy mesher makes can be looked at rather than only quoted. A
// wireframe render of the same view under each one is the clearest
// statement of what "5.3x fewer triangles" means, and a reader should not
// have to take the number on faith.
//
// Passed explicitly rather than kept in a global: the choice is made once
// before the world is built and then captured by value into every worker
// job, which keeps it thread-safe by construction.
enum class MesherKind { Greedy, Naive };

// Light the mesher bakes into each vertex, when it has any.
//
// Null means "no light data", which meshes at full brightness - exactly
// what the engine looked like before block light existed. That default is
// what lets the mesher change ahead of the streaming path without moving a
// single pixel until the two are connected.
struct LightSource {
    const LightGrid* grid = nullptr;
    const NeighborLight* neighbors = nullptr;
};

// One quad per visible face. The slow baseline.
ChunkMeshData build_chunk_mesh_naive(const Chunk& chunk,
                                     const NeighborPlanes& neighbors = {},
                                     const LightSource& light = {});

// Slice-sweep + maximal-rectangle merge. Typical 20-50x reduction on
// terrain-like data.
ChunkMeshData build_chunk_mesh_greedy(const Chunk& chunk,
                                      const NeighborPlanes& neighbors = {},
                                      const LightSource& light = {});

// Dispatches to one of the two above.
ChunkMeshData build_chunk_mesh(MesherKind kind, const Chunk& chunk,
                               const NeighborPlanes& neighbors = {},
                               const LightSource& light = {});

}  // namespace world
