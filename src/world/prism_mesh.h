#pragma once

#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/hyperslice.h"
#include "world/terrain_gen4d.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace world {

// Meshing a chunk as the cross-section of a 4D world rather than as a
// grid of cubes.
//
// The cube path samples the 4D field once per voxel column and draws a
// box. That is exact while the cut is turned in at most one plane - the
// slice stays square-on to the axes it does not turn in, so a cell's
// cross-section really is a rectangle - and it stops being exact the
// moment both planes are turned at once, where a unit 4D cell presents a
// triangle, a pentagon or a hexagon. Drawing a box there is not a small
// error: it is drawing a different shape, and it is exactly the shape
// change a player scrolling through the fourth dimension is looking for.
//
// So this path inverts the question. Instead of asking "what is at this
// voxel", it asks "which cells of the 4D lattice does this chunk's
// footprint pass through, and what does each of them look like from
// here". The answer is a tiling of the footprint by convex polygons, each
// extruded through the y axis the slice rotation never touches - a set of
// prisms, at most hexagonal.
//
// Two things follow that are worth stating because they are what make it
// affordable at all:
//
//   - y is untouched by the rotation, so every cell in a column shares
//     one polygon. Vertical runs of the same block merge into a single
//     prism exactly, which is a greedy merge that costs one comparison.
//
//   - there is nothing to merge horizontally. Neighbouring cells present
//     their own polygons at their own angles, so no two cap faces are
//     coplanar-and-adjacent in the way the greedy mesher needs. That is
//     not a defect of this mesher; it is what a tilted cut of a lattice
//     is. It also means none of the engine's published greedy figures
//     apply here, and none of them are touched: this is a separate path
//     the 3D engine never enters.

// One 4D lattice cell, as this chunk sees it.
struct PrismCell {
    std::int32_t i = 0, k = 0, l = 0;
    // The polygon the cell presents, in CHUNK-LOCAL slice coordinates so
    // it packs into the same mesh-local [0, 16] range every other mesh
    // uses.
    SlicePolygon poly;
    std::array<std::uint8_t, kChunkSizeY> blocks{};
    // Per polygon edge, the cell on the other side of it: an index into
    // PrismChunk::cells, or -1 when the edge is the chunk's own boundary
    // and the neighbour belongs to a chunk this one does not hold.
    //
    // -1 means "unknown", and unknown is treated as air - the same safe
    // direction the cube mesher takes for a missing neighbour. Guessing
    // solid would cull a wall that might be visible, which is a hole in
    // the world; guessing air costs a wall nobody sees.
    std::array<std::int32_t, SlicePolygon::kMaxVerts> across{};
};

struct PrismChunk {
    ChunkCoord              coord{};
    TerrainGen4D::Slice     slice{};
    std::vector<PrismCell>  cells;
    // How many cells the search box offered before clipping rejected
    // them. Only a cost statistic; the bench reports the ratio.
    std::size_t             candidates = 0;
};

// Tiles the chunk's footprint into 4D cells and generates the column each
// one holds. Pure CPU, no GL, safe on a worker.
PrismChunk build_prism_chunk(const TerrainGen4D& gen, ChunkCoord coord,
                             TerrainGen4D::Slice s);

// Writes the cell world into an ordinary Chunk by asking, for each voxel,
// which cell contains its centre.
//
// This is what keeps collision, lighting, raycasts and the occlusion
// culler working unchanged, and keeps them agreeing with what is drawn:
// both come from the same tiling, so the block a player walks into is the
// block whose prism they can see. It is a rasterisation, so it is exact
// only to the voxel grid - a prism thinner than a block can fall between
// two voxel centres and be invisible to physics. That is the same
// approximation any voxel collider makes and it is bounded by one block.
void rasterize_to_chunk(const PrismChunk& pc, Chunk& out);

// The mesh: for each cell, vertical runs of one block become one prism,
// with polygon caps top and bottom and a wall per polygon edge.
ChunkMeshData build_prism_mesh(const PrismChunk& pc,
                               const LightSource& light = {});

}  // namespace world
