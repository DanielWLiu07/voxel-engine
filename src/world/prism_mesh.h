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
//   - horizontal faces DO merge, and the claim that used to sit here
//     saying they could not was wrong. "Neighbouring cells present their
//     own polygons at their own angles" is true of the polygons and false
//     of the conclusion drawn from it: the preimage of a convex set under
//     a linear map is convex, to_4d is linear, and a lattice box is
//     convex - so a BOX of cells presents one convex polygon, at any
//     angle and however many cells it spans. Greedy meshing works here
//     exactly as it does on a voxel grid; it just has to sweep its mask
//     in LATTICE space, over (i, k), rather than in slice space, where
//     the faces are not axis-aligned and the sweep has nothing to grip.
//     At a flat cut the lattice IS the voxel grid, so it reduces to the
//     cube mesher's greedy pass. Measured: 2.47x -> 1.60x the greedy quad
//     count flat, 4.97x -> 3.32x with both planes turned.
//
//     None of the engine's published greedy figures are touched by any of
//     this: it is a separate path the 3D engine never enters.

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
    // The voxel this cell's centroid falls in, chunk-local.
    //
    // Only used to read per-voxel data that is indexed by the grid rather
    // than by the lattice - block light, which is flood-filled on the
    // rasterised chunk. A cell thinner than a voxel shares its
    // neighbour's light, which is a shading approximation of at most one
    // block and never a geometric one.
    std::int16_t vx = 0, vz = 0;
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

// The same tiling, with the columns read out of a Chunk instead of
// generated.
//
// Needed because not every chunk comes from the generator: one restored
// from disk, or one the player has just edited, IS the authority on its
// own contents and asking the generator would throw that away. Reading
// them back is lossy in one direction only - a cell too thin to contain
// any voxel centre has no exact source, so it takes the nearest one -
// which is the same approximation rasterize_to_chunk makes going the
// other way, and it is bounded by one block.
//
// The alternative was to mesh those chunks with the cube mesher, and that
// does not work at all: the mesh scale is a property of the whole world's
// draw call, so one cube mesh in a prism world draws crushed into a
// sixteenth of its chunk.
PrismChunk build_prism_chunk_from_blocks(const Chunk& chunk, ChunkCoord coord,
                                         TerrainGen4D::Slice s);

// Pushes a voxel edit into the cell that contains that voxel's centre, so
// the drawn prisms agree with the chunk the edit was applied to.
//
// Editing a voxel therefore edits a whole 4D block, which is the right
// behaviour rather than a compromise: what the player is looking at IS
// the cell, and breaking part of one would leave a shape the lattice
// cannot express.
void apply_voxel_edit_to_cells(PrismChunk& pc, int x, int y, int z, BlockId b);

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
//
// `neighbors` is the four boundary voxel layers of the adjacent chunks,
// the same snapshot the cube mesher takes, and it is worth roughly a
// quarter of the mesh. Interior walls are already 98% hidden by the cell
// across them, so a chunk's OUTER walls are about 80% of the wall
// geometry that survives - and without the neighbours every one of them
// is emitted, buried in the next chunk's rock where no camera will ever
// see it.
//
// Absent or not present means "unknown", which emits the wall. That is
// the safe direction, and it is the same one the cube mesher takes:
// guessing solid culls a face that might be visible, which is a hole in
// the world, and guessing air costs a quad nobody sees.
ChunkMeshData build_prism_mesh(const PrismChunk& pc,
                               const NeighborPlanes& neighbors = {},
                               const LightSource& light = {});

}  // namespace world
