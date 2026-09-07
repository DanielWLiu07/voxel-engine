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
    world::TerrainGen4D gen(31);
    const auto pc = world::build_prism_chunk(gen, {1, 1}, slice_of(0.5f, 0.35f, 0.25f));
    const auto mesh = world::build_prism_mesh(pc);
    EXPECT(mesh.quad_count > 100, "there is a mesh to check");

    const float xs = mesh.xz_scale;
    int backwards = 0;
    for (std::size_t q = 0; q * 4 + 3 < mesh.vertices.size(); ++q) {
        const auto& a = mesh.vertices[4 * q + 0];
        const auto& b = mesh.vertices[4 * q + 1];
        const auto& c = mesh.vertices[4 * q + 2];
        const glm::vec3 p0{a.x * xs, static_cast<float>(a.y), a.z * xs};
        const glm::vec3 p1{b.x * xs, static_cast<float>(b.y), b.z * xs};
        const glm::vec3 p2{c.x * xs, static_cast<float>(c.y), c.z * xs};
        const glm::vec3 g = glm::cross(p1 - p0, p2 - p0);
        if (glm::length(g) < 1e-7f) continue;  // quantised away
        if (glm::dot(glm::normalize(g), a.nrm()) < 0.5f) ++backwards;
    }
    EXPECT(backwards == 0, "every quad winds the way its normal points");
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
    test_the_tiling_covers_every_voxel_at_any_tilt();
    test_a_compound_cut_produces_shapes_a_cube_cannot();
    test_one_prism_is_a_closed_surface();
    test_every_vertex_fits_the_packed_range();
    test_winding_matches_the_normal_it_carries();
    test_a_cell_keeps_its_block_when_the_cut_turns();
    std::printf("\nprism_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
