// The prism mesher: a chunk meshed as the cross-section of a 4D world.
//
// What is being pinned here is not "does it draw something". It is the
// four things that decide whether this is a 4D world or a wobbly 3D one:
//
//   - at a flat cut it must reproduce the cube world EXACTLY, or every
//     figure the engine publishes is being measured on different geometry
//     than the one it ships;
//   - the prisms must tile the chunk, because what is drawn and what the
//     player collides with are rasterised from the same tiling and a gap
//     in one is a gap in the other;
//   - the surface must be closed, or the world has holes in it;
//   - and a 4D block must keep its contents when the cut turns, which is
//     the difference between slicing a world and re-rolling one.

#include "world/prism_mesh.h"

#include "world/chunk_mesh.h"
#include "world/terrain_gen4d.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, label) do {                                            \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
        std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);      \
        ++g_failures;                                                       \
    }                                                                       \
} while (0)

world::TerrainGen4D::Slice slice_of(float w, float theta, float phi) {
    world::TerrainGen4D::Slice s{};
    s.w = w; s.theta = theta; s.phi = phi;
    return s;
}

// ---------------------------------------------------------------------------

void test_a_flat_cut_tiles_into_the_voxel_grid() {
    // The floor under every cost and correctness claim. With the cut
    // axis-aligned the 4D lattice IS the voxel grid, so the tiling has to
    // be 256 unit squares and nothing else.
    world::TerrainGen4D gen(1337);
    const auto pc = world::build_prism_chunk(gen, {2, -3}, slice_of(0.0f, 0.0f, 0.0f));
    EXPECT(pc.cells.size() == 256, "a flat cut tiles a chunk into 256 cells");

    bool all_squares = true;
    double area = 0.0;
    for (const auto& c : pc.cells) {
        if (c.poly.count != 4) all_squares = false;
        area += std::fabs(world::polygon_area(c.poly));
    }
    EXPECT(all_squares, "and every one of them is four-sided");
    EXPECT(std::fabs(area - 256.0) < 0.01, "and they cover the chunk exactly");
}

void test_a_flat_cut_rasterises_to_the_cube_world() {
    // Same world, two tessellations. Trees are excluded because the prism
    // path does not stamp them yet; every other block must match, or the
    // two paths are generating different terrain and any comparison
    // between them is meaningless.
    world::TerrainGen4D gen(1337);
    const auto s = slice_of(0.0f, 0.0f, 0.0f);

    world::Chunk cube;
    gen.fill_chunk(2, -3, s, cube);

    const auto pc = world::build_prism_chunk(gen, {2, -3}, s);
    world::Chunk prism;
    world::rasterize_to_chunk(pc, prism);

    long compared = 0, mismatched = 0;
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x)
            for (int y = 0; y < world::kChunkSizeY; ++y) {
                const auto a = cube.get(x, y, z);
                const auto b = prism.get(x, y, z);
                if (a == world::BlockId::Wood || a == world::BlockId::Leaves ||
                    b == world::BlockId::Wood || b == world::BlockId::Leaves)
                    continue;
                ++compared;
                if (a != b) ++mismatched;
            }
    EXPECT(compared > 60000, "the comparison covered the chunk");
    EXPECT(mismatched == 0, "a flat cut rasterises to the cube world");
}

void test_a_flat_cut_grows_at_least_the_trees_the_cube_world_does() {
    // Trees are the one thing the two paths cannot agree on exactly, and
    // the difference is in the prism path's favour.
    //
    // The cube path plants only from columns 2..13 of each chunk, because
    // a canopy is five wide and it has no way to reach across a chunk
    // boundary - so a cube world has a tree-free band around every chunk.
    // The prism path pushes from every lattice cell that can reach into
    // this chunk, so it grows those too.
    //
    // What must hold is containment: wherever the cube world has a tree
    // block, the prism world has one. A missing tree would mean the draw
    // or the planting rule diverged; extra ones are the boundary band
    // being filled in.
    world::TerrainGen4D gen(1337);
    const auto s = slice_of(0.0f, 0.0f, 0.0f);
    world::Chunk cube;
    gen.fill_chunk(2, -5, s, cube);
    world::Chunk prism;
    world::rasterize_to_chunk(world::build_prism_chunk(gen, {2, -5}, s), prism);

    auto is_tree = [](world::BlockId b) {
        return b == world::BlockId::Wood || b == world::BlockId::Leaves;
    };
    long cube_tree = 0, prism_tree = 0, missing = 0;
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x)
            for (int y = 0; y < world::kChunkSizeY; ++y) {
                const bool a = is_tree(cube.get(x, y, z));
                const bool b = is_tree(prism.get(x, y, z));
                if (a) ++cube_tree;
                if (b) ++prism_tree;
                if (a && !b) ++missing;
            }
    EXPECT(cube_tree > 0, "the cube world grew trees here to compare against");
    EXPECT(missing == 0, "every cube-world tree block is in the prism world");
    EXPECT(prism_tree >= cube_tree,
           "and the prism world grows the boundary trees the cube world drops");
}

void test_a_tree_belongs_to_one_slab_of_the_fourth_dimension() {
    // A tree spreads across lattice cells at a FIXED w, so it is one 4D
    // object: turning the cut cuts through it rather than deleting it.
    // Stamping across w instead would smear every tree through the fourth
    // dimension, which is both wrong and unmistakable - the world would
    // be nothing but forest.
    world::TerrainGen4D gen(1337);
    const auto pc = world::build_prism_chunk(gen, {2, -5},
                                             slice_of(0.0f, 0.35f, 0.25f));
    std::set<int> w_slabs_with_trees;
    long tree_cells = 0;
    for (const auto& c : pc.cells) {
        bool has = false;
        for (std::size_t y = 0; y < c.blocks.size(); ++y) {
            const auto b = static_cast<world::BlockId>(c.blocks[y]);
            if (b == world::BlockId::Wood || b == world::BlockId::Leaves) has = true;
        }
        if (has) { ++tree_cells; w_slabs_with_trees.insert(c.l); }
    }
    EXPECT(tree_cells > 0, "a tilted cut passes through trees");
    EXPECT(w_slabs_with_trees.size() > 1,
           "and through more than one slab of w, since the cut is tilted");
}

void test_the_tiling_covers_every_voxel_at_any_tilt() {
    // rasterize_to_chunk skips a voxel whose centre lands in no cell. If
    // that ever happens the player falls through a world they can see.
    for (const auto s : {slice_of(0.0f, 0.0f, 0.0f), slice_of(1.5f, 0.4f, 0.0f),
                         slice_of(-2.0f, 0.35f, 0.25f), slice_of(3.0f, 0.9f, -0.7f)}) {
        world::TerrainGen4D gen(99);
        const auto pc = world::build_prism_chunk(gen, {-1, 4}, s);
        double area = 0.0;
        for (const auto& c : pc.cells) area += std::fabs(world::polygon_area(c.poly));
        EXPECT(std::fabs(area - 256.0) < 0.05,
               "the prisms tile the chunk however the cut is turned");
    }
}

void test_a_compound_cut_produces_shapes_a_cube_cannot() {
    // The visible payoff, and the thing the cube path cannot express: at
    // a compound angle cells present five and six sides.
    world::TerrainGen4D gen(5);
    const auto flat = world::build_prism_chunk(gen, {0, 0}, slice_of(0.0f, 0.4f, 0.0f));
    int flat_odd = 0;
    for (const auto& c : flat.cells) if (c.poly.count > 4) ++flat_odd;
    EXPECT(flat_odd == 0, "one rotation plane presents nothing but rectangles");

    const auto both = world::build_prism_chunk(gen, {0, 0}, slice_of(0.0f, 0.4f, 0.4f));
    int odd = 0, most = 0;
    for (const auto& c : both.cells) {
        if (c.poly.count > 4) ++odd;
        most = std::max(most, c.poly.count);
    }
    EXPECT(odd > 0, "a compound cut presents five- and six-sided prisms");
    EXPECT(most <= 6, "and never more than six sides");
}

// Every directed triangle edge of the mesh, so a closed surface can be
// checked the way a closed surface is defined: each undirected edge
// carries exactly one triangle each way.
struct EdgeCount {
    std::map<std::pair<std::array<int, 3>, std::array<int, 3>>, int> dir;
};

void collect_edges(const world::ChunkMeshData& m, EdgeCount& out) {
    const std::size_t quads = m.vertices.size() / 4;
    auto key = [&](const gfx::VertexPacked& v) {
        return std::array<int, 3>{v.x, v.y, v.z};
    };
    for (std::size_t q = 0; q < quads; ++q) {
        const int tri[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& t : tri) {
            const auto a = key(m.vertices[4 * q + t[0]]);
            const auto b = key(m.vertices[4 * q + t[1]]);
            const auto c = key(m.vertices[4 * q + t[2]]);
            if (a == b || b == c || a == c) continue;  // degenerate fan filler
            ++out.dir[{a, b}];
            ++out.dir[{b, c}];
            ++out.dir[{c, a}];
        }
    }
}

void test_one_prism_is_a_closed_surface() {
    // A single solid cell in an otherwise empty chunk must mesh into a
    // closed manifold: caps top and bottom, one wall per side, every edge
    // shared by exactly two triangles running opposite ways.
    //
    // This is the check that catches a reversed cap, a missing wall, or a
    // wall emitted on the wrong edge - three bugs that all look like
    // "some faces are invisible" and none of which a triangle count would
    // find.
    for (const auto s : {slice_of(0.0f, 0.0f, 0.0f), slice_of(0.0f, 0.4f, 0.4f),
                         slice_of(2.0f, 0.8f, -0.6f)}) {
        world::TerrainGen4D gen(7);
        auto pc = world::build_prism_chunk(gen, {0, 0}, s);
        // Empty everything, then put one block in a cell well away from
        // the chunk boundary so its neighbours exist and are air.
        int chosen = -1;
        for (std::size_t ci = 0; ci < pc.cells.size(); ++ci) {
            pc.cells[ci].blocks.fill(static_cast<std::uint8_t>(world::BlockId::Air));
            bool interior = true;
            for (int e = 0; e < pc.cells[ci].poly.count; ++e)
                if (pc.cells[ci].across[e] < 0) interior = false;
            if (interior && chosen < 0) chosen = static_cast<int>(ci);
        }
        EXPECT(chosen >= 0, "the chunk has a cell with no boundary edge");
        if (chosen < 0) continue;
        pc.cells[static_cast<std::size_t>(chosen)].blocks[100] =
            static_cast<std::uint8_t>(world::BlockId::Stone);

        const auto mesh = world::build_prism_mesh(pc);
        EXPECT(mesh.quad_count > 0, "one solid cell meshes to something");

        EdgeCount ec;
        collect_edges(mesh, ec);
        int unpaired = 0;
        for (const auto& kv : ec.dir) {
            if (kv.second != 1) { ++unpaired; continue; }
            const auto rev = std::make_pair(kv.first.second, kv.first.first);
            const auto it = ec.dir.find(rev);
            if (it == ec.dir.end() || it->second != 1) ++unpaired;
        }
        EXPECT(unpaired == 0, "a lone prism is a closed surface");
    }
}

void test_the_neighbouring_chunk_hides_the_outer_walls() {
    // Interior walls are already 98% hidden by the cell across them, so a
    // chunk's OUTER walls are most of the wall geometry that survives.
    // Without the neighbour every one of them is emitted, buried in the
    // next chunk's rock.
    //
    // Checked in both directions, because only one of them is safe to get
    // wrong: a solid neighbour must remove walls, and an ABSENT one must
    // not. Guessing solid where nothing is known culls a face that might
    // be visible, which is a hole in the world.
    world::TerrainGen4D gen(1337);
    const auto s = slice_of(0.0f, 0.35f, 0.25f);
    const auto pc = world::build_prism_chunk(gen, {1, 1}, s);

    auto wall_quads = [](const world::ChunkMeshData& m) {
        int walls = 0;
        for (int q = 0; q * 4 + 3 < static_cast<int>(m.vertices.size()); ++q) {
            const int n = m.vertices[4 * q].normal;
            if (n != 2 && n != 3) ++walls;
        }
        return walls;
    };

    const int alone = wall_quads(world::build_prism_mesh(pc));

    world::NeighborPlanes solid;
    for (auto* p : {&solid.neg_x, &solid.pos_x, &solid.neg_z, &solid.pos_z}) {
        p->present = true;
        for (int y = 0; y < world::kChunkSizeY; ++y)
            for (int t = 0; t < world::kChunkSizeX; ++t)
                p->set(t, y, world::BlockId::Stone);
    }
    const int walled_in = wall_quads(world::build_prism_mesh(pc, solid));

    world::NeighborPlanes empty;
    for (auto* p : {&empty.neg_x, &empty.pos_x, &empty.neg_z, &empty.pos_z}) {
        p->present = true;
        for (int y = 0; y < world::kChunkSizeY; ++y)
            for (int t = 0; t < world::kChunkSizeX; ++t)
                p->set(t, y, world::BlockId::Air);
    }
    const int open_air = wall_quads(world::build_prism_mesh(pc, empty));

    // The safety-critical half, and a uniform plane cannot see it: a wall
    // segment spans several of the neighbour's voxels, and it may only be
    // hidden if ALL of them are solid. Hiding on ANY of them opens a gap
    // wherever the segment straddles the edge of the neighbour's rock.
    //
    // One column of air in an otherwise solid wall separates the rules,
    // but only if the count is of walls that STRADDLE that column - walls
    // lying wholly inside it survive either rule. The all-solid plane is
    // the baseline, because a handful of walls are on neither chunk
    // boundary and are counted the same way under both.
    world::NeighborPlanes one_gap = solid;
    for (auto* p : {&one_gap.neg_x, &one_gap.pos_x, &one_gap.neg_z, &one_gap.pos_z})
        for (int y = 0; y < world::kChunkSizeY; ++y)
            p->set(8, y, world::BlockId::Air);

    auto straddling_walls = [](const world::ChunkMeshData& m) {
        int found = 0;
        const auto& v = m.vertices;
        for (int q = 0; q * 4 + 3 < static_cast<int>(v.size()); ++q) {
            const int n = v[4 * q].normal;
            if (n == 2 || n == 3) continue;
            bool on_x = true, on_z = true;
            for (int i = 0; i < 4; ++i) {
                if (v[4 * q + i].x != 0 && v[4 * q + i].x != 255) on_x = false;
                if (v[4 * q + i].z != 0 && v[4 * q + i].z != 255) on_z = false;
            }
            if (!on_x && !on_z) continue;
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0; i < 4; ++i) {
                const float t = static_cast<float>(on_x ? v[4 * q + i].z
                                                        : v[4 * q + i].x)
                              * m.xz_scale;
                lo = std::min(lo, t);
                hi = std::max(hi, t);
            }
            if (lo < 8.0f - 1e-3f || hi > 9.0f + 1e-3f) ++found;
        }
        return found;
    };
    const auto gapped_mesh = world::build_prism_mesh(pc, one_gap);
    const int gapped = wall_quads(gapped_mesh);
    const int straddle_gapped = straddling_walls(gapped_mesh);
    const int straddle_solid  =
        straddling_walls(world::build_prism_mesh(pc, solid));

    EXPECT(alone > 0, "the chunk has outer walls to cull");
    EXPECT(gapped > walled_in,
           "one air column in the neighbour brings some outer walls back");
    EXPECT(straddle_gapped > straddle_solid,
           "a wall is hidden only if EVERY voxel it spans is solid");
    EXPECT(walled_in < alone, "a solid neighbour hides the outer walls");
    EXPECT(open_air == alone, "an all-air neighbour hides nothing");
    EXPECT(wall_quads(world::build_prism_mesh(pc, {})) == alone,
           "and an absent neighbour is treated as air, not as rock");
}

void test_every_vertex_fits_the_packed_range() {
    // Positions are quantised into a byte pair covering [0, 16]. A vertex
    // outside that clamps silently, which shows up as geometry pinned to
    // the chunk edge rather than as an error.
    world::TerrainGen4D gen(2024);
    int outside = 0, quads = 0;
    for (const auto s : {slice_of(0.0f, 0.0f, 0.0f), slice_of(1.0f, 0.5f, 0.3f),
                         slice_of(-4.0f, -0.8f, 0.9f)}) {
        const auto pc = world::build_prism_chunk(gen, {5, 5}, s);
        for (const auto& c : pc.cells)
            for (int v = 0; v < c.poly.count; ++v) {
                if (c.poly.x[v] < -1e-3f || c.poly.x[v] > 16.0f + 1e-3f) ++outside;
                if (c.poly.z[v] < -1e-3f || c.poly.z[v] > 16.0f + 1e-3f) ++outside;
            }
        const auto mesh = world::build_prism_mesh(pc);
        quads += mesh.quad_count;
    }
    EXPECT(outside == 0, "every prism corner is inside the chunk footprint");
    EXPECT(quads > 0, "the sweep meshed something");
}

void test_winding_matches_the_normal_it_carries() {
    // The engine culls back faces with GL_CCW, so a quad whose winding
    // disagrees with its own normal is invisible from the side it is
    // meant to be seen from and solid from the side it is not. Measured
    // rather than assumed: this convention was read wrong once already.
    // Several cuts, because the first version of this checked one and
    // passed while the engine flagged 610 triangles at a different pair
    // of angles. A winding rule that holds at one orientation says
    // nothing about the next.
    // Several cuts AND several chunks. The first version checked one of
    // each and passed while the engine flagged 610 triangles; the
    // offending walls were in chunk (-9, +12) at theta = phi = 0.45, and
    // there is nothing special about that chunk except that it is where
    // a sub-quantisation-step edge happened to fall. Chunk (-9, +12) is
    // in the list by name for exactly that reason.
    world::TerrainGen4D gen(1337);
    int backwards = 0, checked = 0;
    const world::ChunkCoord chunks[] = {{1, 1}, {-9, 12}, {-8, 11}, {0, 0}};
    for (const auto s : {slice_of(0.5f, 0.35f, 0.25f), slice_of(0.0f, 0.45f, 0.45f),
                         slice_of(0.0f, 0.9f, 0.7f),   slice_of(2.0f, -0.6f, 0.8f),
                         slice_of(-1.0f, 0.2f, -0.5f)}) {
      for (const auto coord : chunks) {
        const auto pc = world::build_prism_chunk(gen, coord, s);
        const auto mesh = world::build_prism_mesh(pc);
        const float xs = mesh.xz_scale;
        for (std::size_t q = 0; q * 4 + 3 < mesh.vertices.size(); ++q) {
            const auto& a = mesh.vertices[4 * q + 0];
            const auto& b = mesh.vertices[4 * q + 1];
            const auto& c = mesh.vertices[4 * q + 2];
            const glm::vec3 p0{a.x * xs, static_cast<float>(a.y), a.z * xs};
            const glm::vec3 p1{b.x * xs, static_cast<float>(b.y), b.z * xs};
            const glm::vec3 p2{c.x * xs, static_cast<float>(c.y), c.z * xs};
            const glm::vec3 g = glm::cross(p1 - p0, p2 - p0);
            if (glm::length(g) < 1e-7f) continue;  // quantised away
            ++checked;
            if (glm::dot(glm::normalize(g), a.nrm()) < 0.5f) ++backwards;
        }
      }
    }
    EXPECT(checked > 1000, "there is a mesh to check");
    EXPECT(backwards == 0, "every quad winds the way its normal points");
}

void test_reading_columns_back_out_of_voxels_is_lossy() {
    // Why a boundary re-mesh of ordinary terrain goes back to the
    // GENERATOR rather than reading the chunk it already holds.
    //
    // Both sources are legitimate - a chunk the player edited is the
    // authority on itself and has to be read back - but they are not
    // equal. Reading back asks each cell which voxel its centroid lands
    // in, and a cell too thin to contain a voxel centre has no source of
    // its own, so it inherits a neighbour's column. That merges cells
    // that should differ.
    //
    // If the two sources were mixed by path - fresh streams generated,
    // re-meshes read back - then whether a chunk took a re-mesh would
    // change its geometry, and which chunks take one depends on worker
    // timing. Same world, different mesh, decided by a race.
    world::TerrainGen4D gen(1337);
    const auto s = slice_of(0.0f, 0.45f, 0.45f);
    const auto from_gen = world::build_prism_chunk(gen, {1, 1}, s);

    world::Chunk voxels;
    world::rasterize_to_chunk(from_gen, voxels);
    const auto from_blocks = world::build_prism_chunk_from_blocks(voxels, {1, 1}, s);

    EXPECT(from_gen.cells.size() == from_blocks.cells.size(),
           "both sources tile the chunk into the same cells");

    int differing = 0;
    for (std::size_t i = 0; i < from_gen.cells.size() &&
                            i < from_blocks.cells.size(); ++i) {
        if (from_gen.cells[i].blocks != from_blocks.cells[i].blocks) ++differing;
    }
    EXPECT(differing > 0,
           "reading columns back out of voxels does not reproduce them");

    const int gen_quads    = world::build_prism_mesh(from_gen).quad_count;
    const int blocks_quads = world::build_prism_mesh(from_blocks).quad_count;
    EXPECT(gen_quads != blocks_quads,
           "and the two produce different geometry, so the source cannot "
           "be chosen by which code path a chunk happened to take");
}

void test_fuzz_random_cuts_and_chunks() {
    // The check that would have found the sub-step wall on its own.
    //
    // Every property this file pins is checked at hand-picked angles, and
    // a hand-picked angle is exactly what missed a defect that needed a
    // particular pair of angles AND a particular chunk. This walks a
    // pseudo-random sweep of both instead, and asserts the three things
    // that must hold at EVERY orientation: the tiling covers the chunk,
    // every corner is addressable, and every triangle winds the way its
    // normal points.
    //
    // Deterministic seed, so a failure is reproducible from the line it
    // prints rather than from a lucky rerun.
    std::mt19937 rng(20260907u);
    std::uniform_real_distribution<float> angle(-1.2f, 1.2f);
    std::uniform_real_distribution<float> off(-6.0f, 6.0f);
    std::uniform_int_distribution<int>    coord(-20, 20);

    world::TerrainGen4D gen(1337);
    int cases = 0, bad_tiling = 0, bad_range = 0, backwards = 0, tris = 0;
    for (int i = 0; i < 60; ++i) {
        world::TerrainGen4D::Slice s{};
        s.w       = off(rng);
        s.theta   = angle(rng);
        s.phi     = angle(rng);
        s.z_shift = off(rng);
        s.x_shift = off(rng);
        const world::ChunkCoord c{coord(rng), coord(rng)};
        const auto pc = world::build_prism_chunk(gen, c, s);
        const auto mesh = world::build_prism_mesh(pc);
        ++cases;

        double area = 0.0;
        for (const auto& cell : pc.cells) {
            area += std::fabs(world::polygon_area(cell.poly));
            for (int v = 0; v < cell.poly.count; ++v) {
                if (cell.poly.x[v] < -1e-3f || cell.poly.x[v] > 16.0f + 1e-3f ||
                    cell.poly.z[v] < -1e-3f || cell.poly.z[v] > 16.0f + 1e-3f)
                    ++bad_range;
            }
        }
        // Snapping moves a corner by up to half a quantisation step, so
        // the tiled area is no longer exact to a rounding - it is exact
        // to the grid the vertices live on. A tenth of a block of slack
        // over a 256-block patch is generous against that and still
        // catches a whole missing cell.
        if (std::fabs(area - 256.0) > 0.1) ++bad_tiling;

        const float xs = mesh.xz_scale;
        for (std::size_t q = 0; q * 4 + 3 < mesh.vertices.size(); ++q) {
            const auto& a = mesh.vertices[4 * q + 0];
            const auto& b = mesh.vertices[4 * q + 1];
            const auto& d = mesh.vertices[4 * q + 2];
            const glm::vec3 p0{a.x * xs, static_cast<float>(a.y), a.z * xs};
            const glm::vec3 p1{b.x * xs, static_cast<float>(b.y), b.z * xs};
            const glm::vec3 p2{d.x * xs, static_cast<float>(d.y), d.z * xs};
            const glm::vec3 g = glm::cross(p1 - p0, p2 - p0);
            if (glm::length(g) < 1e-7f) continue;
            ++tris;
            if (glm::dot(glm::normalize(g), a.nrm()) < 0.5f) ++backwards;
        }
    }
    EXPECT(cases == 60 && tris > 20000, "the fuzz sweep meshed real geometry");
    EXPECT(bad_tiling == 0, "the prisms tile the chunk at every orientation");
    EXPECT(bad_range == 0, "every corner is addressable at every orientation");
    EXPECT(backwards == 0, "every quad winds correctly at every orientation");
}

void test_a_cell_keeps_its_block_when_the_cut_turns() {
    // The property the whole design exists for. Turn the cut a little and
    // a 4D block must still be made of what it was made of; only the
    // shape it presents changes. Cells that leave the chunk's footprint
    // are simply not compared - what must never happen is a cell that is
    // in both and holds different terrain.
    world::TerrainGen4D gen(808);
    const auto a = world::build_prism_chunk(gen, {0, 0}, slice_of(1.0f, 0.30f, 0.20f));
    const auto b = world::build_prism_chunk(gen, {0, 0}, slice_of(1.0f, 0.34f, 0.24f));

    std::map<std::array<int, 3>, const world::PrismCell*> by_cell;
    for (const auto& c : a.cells) by_cell[{c.i, c.k, c.l}] = &c;

    int shared = 0, differing = 0, reshaped = 0;
    for (const auto& c : b.cells) {
        const auto it = by_cell.find({c.i, c.k, c.l});
        if (it == by_cell.end()) continue;
        ++shared;
        if (c.blocks != it->second->blocks) ++differing;
        if (c.poly.count != it->second->poly.count) ++reshaped;
    }
    EXPECT(shared > 100, "the two cuts share cells to compare");
    EXPECT(differing == 0, "a cell holds the same blocks after the cut turns");
    EXPECT(reshaped > 0, "and at least some of them present a different shape");
}

}  // namespace

int main() {
    std::printf("prism_tests: running...\n");
    test_a_flat_cut_tiles_into_the_voxel_grid();
    test_a_flat_cut_rasterises_to_the_cube_world();
    test_a_flat_cut_grows_at_least_the_trees_the_cube_world_does();
    test_a_tree_belongs_to_one_slab_of_the_fourth_dimension();
    test_the_tiling_covers_every_voxel_at_any_tilt();
    test_a_compound_cut_produces_shapes_a_cube_cannot();
    test_one_prism_is_a_closed_surface();
    test_the_neighbouring_chunk_hides_the_outer_walls();
    test_every_vertex_fits_the_packed_range();
    test_winding_matches_the_normal_it_carries();
    test_reading_columns_back_out_of_voxels_is_lossy();
    test_fuzz_random_cuts_and_chunks();
    test_a_cell_keeps_its_block_when_the_cut_turns();
    std::printf("\nprism_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
