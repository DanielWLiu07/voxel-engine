// Differential fuzzing of the greedy mesher against the naive one.
//
// The existing mesher tests compare TOTAL SURFACE AREA on two hand-picked
// terrains. That is a weak oracle: it is a single scalar, so a face
// emitted in the wrong place and a face missing somewhere else cancel out,
// and it says nothing about block ids or facing. It is also chunk-local -
// every one of those tests meshes a chunk as if surrounded by air.
//
// Both of those gaps had already cost something. The greedy sweep was
// emitting faces owned by the NEIGHBOUR chunk at its outer slices, so
// every shared boundary face was built twice. Area tests could not see it
// (they never supplied a neighbour) and it shipped; `--validate` caught it
// later, on a real GPU, by reading 16,790 bad triangles back out of VRAM.
//
// This is the oracle that would have caught it on a laptop in 40 ms.
// Every quad, from either mesher, is decomposed back into the 1x1 unit
// faces it covers, and the two SETS have to be equal - same cells, same
// facing, same block id, no duplicates on either side. That is exact, not
// statistical: a merge is only legal if it changes how the surface is
// packed into rectangles and nothing else.
//
// It runs over randomized and adversarial chunk fills crossed with every
// neighbour configuration, including the one the shipped bug lived on.
//
// A test that passes on correct code proves nothing on its own, so this
// one was checked against the defect it was written for. Reintroducing it
// is a two-line edit in chunk_mesh.cpp - force the ownership guard on:
//
//   const bool owns_pos = true;   // was (s > 0)
//   const bool owns_neg = true;   // was (s < d_size)
//
// Measured with that in place: this binary fails 99 of its 189 checks and
// names the case ("fill=solid neighbors=all-solid ... greedy 15360 unit
// faces vs naive 512, 14848 extra"). The 247-check suite in
// tests/test_world.cpp passes all 247. That gap is the reason this file
// exists.
//
//   cmake --build build -j && ctest --test-dir build --output-on-failure

#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/terrain_gen.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define EXPECT(cond, label) do {                                            \
    ++g_checks;                                                             \
    if (!(cond)) {                                                          \
        std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, label);     \
        ++g_failures;                                                       \
    }                                                                       \
} while (0)

// One 1x1 cell of surface: which cell, which way it faces, what it is.
//
// Deliberately NOT carrying ao or light. The greedy mesher samples both at
// the four corners of a merged rectangle, so interior variation is lost by
// design - merging is allowed to change them and comparing them would fail
// on correct output. Position, facing and material are the part a merge
// must never touch.
struct UnitFace {
    std::uint16_t x = 0, y = 0, z = 0;
    std::uint8_t  normal = 0;
    std::uint8_t  block_id = 0;

    bool operator<(const UnitFace& o) const {
        if (normal != o.normal) return normal < o.normal;
        if (y != o.y) return y < o.y;
        if (x != o.x) return x < o.x;
        if (z != o.z) return z < o.z;
        return block_id < o.block_id;
    }
    bool operator==(const UnitFace& o) const {
        return x == o.x && y == o.y && z == o.z &&
               normal == o.normal && block_id == o.block_id;
    }
};

// Explodes a mesh back into unit faces. A naive quad yields exactly one; a
// greedy quad yields w*h of them, which is the whole point.
//
// Returns false if a quad is not a plane-aligned rectangle at all - that
// is a corruption the caller should report rather than silently expand.
bool unit_faces(const world::ChunkMeshData& mesh,
                std::vector<UnitFace>& out) {
    out.clear();
    const auto& v = mesh.vertices;
    if (v.size() != static_cast<std::size_t>(mesh.quad_count) * 4) return false;

    for (int q = 0; q < mesh.quad_count; ++q) {
        const auto* c = &v[static_cast<std::size_t>(q) * 4];
        const std::uint8_t n  = c[0].normal;
        const std::uint8_t id = c[0].block_id;
        if (n > 5) return false;

        std::uint16_t lo[3] = {c[0].x, c[0].y, c[0].z};
        std::uint16_t hi[3] = {c[0].x, c[0].y, c[0].z};
        for (int i = 1; i < 4; ++i) {
            const std::uint16_t p[3] = {c[i].x, c[i].y, c[i].z};
            // Every vertex of a quad agrees on facing and material; the
            // merge sweep keys its mask on the block id, so a mismatch
            // here means a rectangle spanned two materials.
            if (c[i].normal != n || c[i].block_id != id) return false;
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }

        // normal 0,1 -> +-X (x is the plane); 2,3 -> +-Y; 4,5 -> +-Z.
        const int fixed = n / 2;
        if (lo[fixed] != hi[fixed]) return false;   // not plane-aligned
        int va = (fixed == 0) ? 1 : 0;              // the two varying axes
        int vb = (fixed == 2) ? 1 : 2;
        if (fixed == 1) { va = 0; vb = 2; }
        if (hi[va] <= lo[va] || hi[vb] <= lo[vb]) return false;  // degenerate

        for (std::uint16_t a = lo[va]; a < hi[va]; ++a) {
            for (std::uint16_t b = lo[vb]; b < hi[vb]; ++b) {
                UnitFace f;
                std::uint16_t p[3];
                p[fixed] = lo[fixed];
                p[va] = a;
                p[vb] = b;
                f.x = p[0]; f.y = p[1]; f.z = p[2];
                f.normal = n;
                f.block_id = id;
                out.push_back(f);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- fills

const world::BlockId kMaterials[] = {
    world::BlockId::Stone, world::BlockId::Dirt, world::BlockId::Grass,
    world::BlockId::Sand,  world::BlockId::Wood, world::BlockId::Leaves,
    world::BlockId::Snow,  world::BlockId::Glow,
};
constexpr int kMaterialCount =
    static_cast<int>(sizeof(kMaterials) / sizeof(kMaterials[0]));

const char* kFillNames[] = {
    "empty", "solid", "noise-10%", "noise-50%", "noise-90%", "checkerboard",
    "thin-slab", "wall", "hollow-shell", "perlin-caves", "multi-material",
    "sparse-columns",
};
constexpr int kFillCount =
    static_cast<int>(sizeof(kFillNames) / sizeof(kFillNames[0]));

world::BlockId pick(std::mt19937& rng) {
    return kMaterials[std::uniform_int_distribution<int>(
        0, kMaterialCount - 1)(rng)];
}

// The y band the fills work in. Full-height chunks are 256 tall and mostly
// air; concentrating the geometry keeps the fuzz cases dense enough to be
// interesting without making each one slow.
constexpr int kLo = 30;
constexpr int kHi = 54;

void fill_chunk(world::Chunk& c, int style, std::mt19937& rng) {
    const auto solid_at = [&](int x, int y, int z, world::BlockId b) {
        c.set(x, y, z, b);
    };
    switch (style) {
        case 0:  // empty: the mesher must emit nothing at all
            break;
        case 1:  // solid band: only the shell of the band is visible
            for (int y = kLo; y < kHi; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        solid_at(x, y, z, world::BlockId::Stone);
            break;
        case 2: case 3: case 4: {
            // Random noise. Low density is nearly all isolated blocks (no
            // merges legal); high density is nearly solid (huge merges).
            const int pct = (style == 2) ? 10 : (style == 3) ? 50 : 90;
            std::uniform_int_distribution<int> roll(0, 99);
            for (int y = kLo; y < kHi; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        if (roll(rng) < pct)
                            solid_at(x, y, z, world::BlockId::Stone);
            break;
        }
        case 5:  // checkerboard: zero merges are legal anywhere
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x)
                    if ((x + z) % 2 == 0)
                        solid_at(x, kLo + 5, z, world::BlockId::Stone);
            break;
        case 6:  // one-thick slab: top and bottom merge into single quads
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x)
                    solid_at(x, kLo + 3, z, world::BlockId::Dirt);
            break;
        case 7:  // wall along z, one thick in x: tests the X boundary path
            for (int y = kLo; y < kHi; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    solid_at(0, y, z, world::BlockId::Stone);
            break;
        case 8: {  // hollow shell: interior faces must never be emitted
            for (int y = kLo; y < kHi; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x) {
                        const bool edge =
                            y == kLo || y == kHi - 1 || x == 0 ||
                            x == world::kChunkSizeX - 1 || z == 0 ||
                            z == world::kChunkSizeZ - 1;
                        if (edge) solid_at(x, y, z, world::BlockId::Stone);
                    }
            break;
        }
        case 9: {  // the real generator, caves on: overhangs and cavities
            static world::TerrainGen terrain(1337);
            std::uniform_int_distribution<int> coord(-4, 4);
            terrain.fill_chunk(coord(rng), coord(rng), c);
            break;
        }
        case 10: {  // multi-material noise: merges must stop at every seam
            std::uniform_int_distribution<int> roll(0, 99);
            for (int y = kLo; y < kHi; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        if (roll(rng) < 70) solid_at(x, y, z, pick(rng));
            break;
        }
        case 11: {  // sparse columns of varying height
            std::uniform_int_distribution<int> roll(0, 99);
            std::uniform_int_distribution<int> h(1, kHi - kLo);
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x)
                    if (roll(rng) < 30) {
                        const int top = kLo + h(rng);
                        for (int y = kLo; y < top; ++y)
                            solid_at(x, y, z, world::BlockId::Stone);
                    }
            break;
        }
        default: break;
    }
}

// ----------------------------------------------------------- neighbours

const char* kNeighborNames[] = {
    "absent", "all-solid", "all-air", "random", "half-solid",
};
constexpr int kNeighborCount =
    static_cast<int>(sizeof(kNeighborNames) / sizeof(kNeighborNames[0]));

void fill_plane(world::BoundaryPlane& p, int style, std::mt19937& rng) {
    p.present = true;
    std::uniform_int_distribution<int> roll(0, 99);
    for (int y = 0; y < world::kChunkSizeY; ++y) {
        for (int t = 0; t < world::kChunkSizeX; ++t) {
            world::BlockId b = world::BlockId::Air;
            switch (style) {
                case 1: b = world::BlockId::Stone; break;
                case 2: b = world::BlockId::Air; break;
                case 3: b = roll(rng) < 50 ? pick(rng) : world::BlockId::Air;
                        break;
                case 4: b = (t < world::kChunkSizeX / 2)
                            ? world::BlockId::Stone : world::BlockId::Air;
                        break;
                default: break;
            }
            p.set(t, y, b);
        }
    }
}

world::NeighborPlanes make_neighbors(int style, std::mt19937& rng) {
    world::NeighborPlanes n;
    if (style == 0) return n;  // all four absent: meshed against open air
    fill_plane(n.neg_x, style, rng);
    fill_plane(n.pos_x, style, rng);
    fill_plane(n.neg_z, style, rng);
    fill_plane(n.pos_z, style, rng);
    return n;
}

// ---------------------------------------------------------------- oracle

bool has_duplicates(const std::vector<UnitFace>& sorted) {
    return std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
}

// Runs one case and returns true when the two meshers agree exactly.
// Prints the first disagreement it finds, with enough detail to rebuild
// the case by hand.
bool compare_case(int fill_style, int neighbor_style, unsigned seed) {
    std::mt19937 rng(seed);
    world::Chunk chunk;
    fill_chunk(chunk, fill_style, rng);
    // Same rng stream for both meshers: the neighbour planes have to be
    // byte-identical or the comparison means nothing.
    std::mt19937 nrng(seed ^ 0x9e3779b9u);
    const auto neighbors = make_neighbors(neighbor_style, nrng);

    const auto naive  = world::build_chunk_mesh_naive(chunk, neighbors);
    const auto greedy = world::build_chunk_mesh_greedy(chunk, neighbors);

    std::vector<UnitFace> a, b;
    const bool ok_n = unit_faces(naive, a);
    const bool ok_g = unit_faces(greedy, b);
    const auto label = [&](const char* what) {
        std::printf("  FAIL fill=%s neighbors=%s seed=%u: %s\n",
                    kFillNames[fill_style], kNeighborNames[neighbor_style],
                    seed, what);
    };
    if (!ok_n) { label("naive mesh is not decomposable into unit faces"); return false; }
    if (!ok_g) { label("greedy mesh is not decomposable into unit faces"); return false; }

    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());

    if (has_duplicates(a)) { label("naive emitted a duplicate unit face"); return false; }
    // The shipped boundary bug landed exactly here: both sides of a shared
    // face built by both chunks, so the same cell appeared twice.
    if (has_duplicates(b)) { label("greedy emitted a duplicate unit face"); return false; }

    if (a == b) return true;

    std::printf("  FAIL fill=%s neighbors=%s seed=%u: naive %zu unit faces, "
                "greedy %zu\n", kFillNames[fill_style],
                kNeighborNames[neighbor_style], seed, a.size(), b.size());
    std::vector<UnitFace> only_naive, only_greedy;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(only_naive));
    std::set_difference(b.begin(), b.end(), a.begin(), a.end(),
                        std::back_inserter(only_greedy));
    const auto dump = [](const char* tag, const std::vector<UnitFace>& v) {
        std::printf("    %s: %zu\n", tag, v.size());
        for (std::size_t i = 0; i < v.size() && i < 4; ++i) {
            std::printf("      (%u,%u,%u) normal=%u id=%u\n",
                        v[i].x, v[i].y, v[i].z, v[i].normal, v[i].block_id);
        }
    };
    dump("missing from greedy", only_naive);
    dump("extra in greedy", only_greedy);
    return false;
}

// ----------------------------------------------------------------- tests

void test_unit_face_decomposition_is_sane() {
    // The oracle itself has to be right before it can judge anything. One
    // solid block in open air is six unit faces, one per direction, and
    // the naive mesher is the definition of that.
    world::Chunk c;
    c.set(8, 40, 8, world::BlockId::Stone);
    std::vector<UnitFace> f;
    EXPECT(unit_faces(world::build_chunk_mesh_naive(c), f),
           "a naive mesh decomposes");
    EXPECT(f.size() == 6, "one isolated block is six unit faces");
    std::sort(f.begin(), f.end());
    EXPECT(!has_duplicates(f), "and no two of them are the same face");

    // A merged rectangle has to explode back into exactly the cells it
    // covers. A 16x16 slab lid is one greedy quad and 256 unit faces.
    world::Chunk slab;
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x)
            slab.set(x, 40, z, world::BlockId::Stone);
    const auto g = world::build_chunk_mesh_greedy(slab);
    std::vector<UnitFace> gf;
    EXPECT(unit_faces(g, gf), "a greedy mesh decomposes");
    int up = 0;
    for (const auto& uf : gf) if (uf.normal == 2) ++up;
    EXPECT(up == world::kChunkSizeX * world::kChunkSizeZ,
           "the merged lid explodes back into one face per column");
}

void test_greedy_matches_naive_face_for_face() {
    // The sweep. Every fill against every neighbour configuration, three
    // seeds each, so the randomized fills get more than one draw.
    int cases = 0;
    int failures_before = g_failures;
    for (int fill = 0; fill < kFillCount; ++fill) {
        for (int nb = 0; nb < kNeighborCount; ++nb) {
            for (unsigned s = 0; s < 3; ++s) {
                const unsigned seed =
                    0x5eed0000u + static_cast<unsigned>(fill * 1000 + nb * 10) + s;
                ++cases;
                ++g_checks;
                if (!compare_case(fill, nb, seed)) ++g_failures;
            }
        }
    }
    std::printf("  %d fill x neighbour x seed cases compared face-for-face\n",
                cases);
    if (g_failures != failures_before) {
        std::printf("  (each FAIL line above names the exact case to rerun)\n");
    }
}

void test_greedy_actually_merges() {
    // The oracle above is satisfied by a greedy mesher that never merges
    // anything, which would pass every case and be useless. Pin the other
    // side of it: on mergeable input greedy must emit strictly fewer
    // quads, and on the checkerboard it must emit exactly as many.
    std::mt19937 rng(7);
    world::Chunk dense;
    fill_chunk(dense, 4, rng);  // noise-90%, highly mergeable
    const auto dn = world::build_chunk_mesh_naive(dense);
    const auto dg = world::build_chunk_mesh_greedy(dense);
    EXPECT(dg.quad_count < dn.quad_count,
           "greedy merges on dense input");

    world::Chunk board;
    fill_chunk(board, 5, rng);  // checkerboard, nothing may merge
    const auto bn = world::build_chunk_mesh_naive(board);
    const auto bg = world::build_chunk_mesh_greedy(board);
    EXPECT(bg.quad_count == bn.quad_count,
           "greedy merges nothing on a checkerboard");
}

void test_empty_chunk_meshes_to_nothing() {
    world::Chunk c;
    world::NeighborPlanes solid;
    std::mt19937 rng(1);
    for (auto* p : {&solid.neg_x, &solid.pos_x, &solid.neg_z, &solid.pos_z})
        fill_plane(*p, 1, rng);
    EXPECT(world::build_chunk_mesh_greedy(c, solid).quad_count == 0,
           "an air chunk emits nothing even against solid neighbours");
    EXPECT(world::build_chunk_mesh_naive(c, solid).quad_count == 0,
           "and the naive mesher agrees");
}

}  // namespace

int main() {
    std::printf("mesher differential fuzz\n");
    test_unit_face_decomposition_is_sane();
    test_greedy_matches_naive_face_for_face();
    test_greedy_actually_merges();
    test_empty_chunk_meshes_to_nothing();

    std::printf("%s  %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
