// Unit tests for the voxel-side bookkeeping: chunk AABB, section bounds,
// greedy/naive mesh equivalence. No GL is touched. Run via ctest after a
// build:
//   cmake --build build -j
//   ctest --test-dir build --output-on-failure
//
// The harness is intentionally minimal: a single EXPECT macro, a failure
// counter, and a main that reports pass/fail. No external test framework.

#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_mesh.h"
#include "world/chunk_serialize.h"
#include "world/world.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

void fill_solid_column(world::Chunk& c, int x, int z, int y_lo, int y_hi,
                       world::BlockId block = world::BlockId::Stone) {
    for (int y = y_lo; y <= y_hi; ++y) c.set(x, y, z, block);
}

// ----- make_chunk_aabb -----------------------------------------------------

void test_aabb_empty_chunk() {
    world::Chunk c;
    auto box = world::make_chunk_aabb({0, 0}, c);
    // Empty chunk: tight_y_range returns (0, 0), so aabb max.y = 1 by the
    // +1 convention. Slot won't have a mesh so this is never tested in
    // practice, but the contract should still be honest.
    EXPECT(box.min.x == 0.0f, "empty chunk min.x = 0");
    EXPECT(box.min.y == 0.0f, "empty chunk min.y = 0");
    EXPECT(box.max.y == 1.0f, "empty chunk max.y = 1 (degenerate, ok)");
}

void test_aabb_single_block_in_offset_chunk() {
    world::Chunk c;
    c.set(/*lx*/ 8, /*y*/ 30, /*lz*/ 8, world::BlockId::Stone);
    // Chunk (1, 2): world XZ origin is (16, 32).
    auto box = world::make_chunk_aabb({1, 2}, c);
    EXPECT(box.min.x == 16.0f, "chunk (1,2) min.x = 16");
    EXPECT(box.max.x == 32.0f, "chunk (1,2) max.x = 32 (origin + 16)");
    EXPECT(box.min.z == 32.0f, "chunk (1,2) min.z = 32");
    EXPECT(box.max.z == 48.0f, "chunk (1,2) max.z = 48");
    EXPECT(box.min.y == 30.0f, "single block at y=30 -> min.y = 30");
    EXPECT(box.max.y == 31.0f, "single block at y=30 -> max.y = 31 (closed range +1)");
}

void test_aabb_tight_y_spans_full_column() {
    world::Chunk c;
    fill_solid_column(c, 4, 4, /*y_lo*/ 20, /*y_hi*/ 50);
    auto box = world::make_chunk_aabb({0, 0}, c);
    EXPECT(box.min.y == 20.0f, "solid 20..50 -> min.y = 20");
    EXPECT(box.max.y == 51.0f, "solid 20..50 -> max.y = 51");
}

void test_aabb_tight_y_ignores_air_above() {
    world::Chunk c;
    fill_solid_column(c, 0, 0, 10, 12);
    fill_solid_column(c, 1, 1, 200, 202);
    // Min Y is the lowest solid (10); max Y is highest solid (202).
    auto box = world::make_chunk_aabb({0, 0}, c);
    EXPECT(box.min.y == 10.0f, "mixed columns -> min.y picks lowest solid");
    EXPECT(box.max.y == 203.0f, "mixed columns -> max.y picks highest solid + 1");
}

// ----- compute_section_bounds ---------------------------------------------

void test_sections_empty_chunk() {
    world::Chunk c;
    auto bounds = world::compute_section_bounds({0, 0}, c);
    for (int i = 0; i < world::kSectionsPerChunk; ++i) {
        EXPECT(!bounds[i].has_mesh, "empty chunk -> section has_mesh=false");
    }
}

void test_sections_terrain_low() {
    // Single solid block at y=10 lands in section 0 (y range [0, 32)).
    world::Chunk c;
    c.set(8, 10, 8, world::BlockId::Stone);
    auto bounds = world::compute_section_bounds({0, 0}, c);
    EXPECT(bounds[0].has_mesh, "block at y=10 -> section 0 has mesh");
    for (int i = 1; i < world::kSectionsPerChunk; ++i) {
        EXPECT(!bounds[i].has_mesh, "higher sections empty");
    }
}

void test_sections_terrain_spanning_boundary() {
    // Vertical pillar y=20..50 spans sections 0 (y=20..31) and 1 (y=32..50).
    // Greedy meshing on a single-column pillar emits a stacked column of
    // faces; the bucketer assigns each face to its bottom-Y section. Both
    // sections should end up with meshes.
    world::Chunk c;
    fill_solid_column(c, 8, 8, 20, 50);
    auto bounds = world::compute_section_bounds({0, 0}, c);
    EXPECT(bounds[0].has_mesh, "pillar 20..50 -> section 0 (covers y=20..31) has mesh");
    EXPECT(bounds[1].has_mesh, "pillar 20..50 -> section 1 (covers y=32..50) has mesh");
    for (int i = 2; i < world::kSectionsPerChunk; ++i) {
        EXPECT(!bounds[i].has_mesh, "sections above the pillar are empty");
    }
}

void test_section_bounds_in_world_space() {
    world::Chunk c;
    fill_solid_column(c, 0, 0, 10, 12);
    // Chunk (2, -1): world XZ origin (32, -16).
    auto bounds = world::compute_section_bounds({2, -1}, c);
    const auto& s0 = bounds[0].aabb;
    EXPECT(bounds[0].has_mesh, "section 0 has mesh for 10..12 block column");
    EXPECT(s0.min.x >= 32.0f && s0.max.x <= 48.0f, "section X within chunk world bounds");
    EXPECT(s0.min.z >= -16.0f && s0.max.z <= 0.0f, "section Z within chunk world bounds");
}

// ----- greedy / naive mesh equivalence ------------------------------------

// Total surface area of a ChunkMeshData. Both meshers emit four vertices
// per quad with v[0] and v[2] sitting on the diagonal of an axis-aligned
// face, so the diagonal vector has exactly one zero component and the
// other two are the side lengths.
double total_quad_area(const world::ChunkMeshData& m) {
    double area = 0.0;
    const std::size_t quad_count = m.vertices.size() / 4;
    for (std::size_t q = 0; q < quad_count; ++q) {
        const auto& v0 = m.vertices[4 * q + 0];
        const auto& v2 = m.vertices[4 * q + 2];
        const double dx = std::abs(v2.position.x - v0.position.x);
        const double dy = std::abs(v2.position.y - v0.position.y);
        const double dz = std::abs(v2.position.z - v0.position.z);
        if      (dx == 0.0) area += dy * dz;
        else if (dy == 0.0) area += dx * dz;
        else                area += dx * dy;
    }
    return area;
}

void test_greedy_equals_naive_area_on_simple_terrain() {
    world::Chunk c;
    // A small stepped terrain: each (x,z) column rises with a deterministic
    // pattern so faces have varied shapes and the greedy mesher has work
    // to do. Surface area must still match the naive one-quad-per-face.
    for (int z = 0; z < world::kChunkSizeZ; ++z) {
        for (int x = 0; x < world::kChunkSizeX; ++x) {
            const int h = 20 + ((x + z) % 6);
            fill_solid_column(c, x, z, 0, h);
        }
    }
    const auto naive  = world::build_chunk_mesh_naive(c);
    const auto greedy = world::build_chunk_mesh_greedy(c);

    const double a_naive  = total_quad_area(naive);
    const double a_greedy = total_quad_area(greedy);

    EXPECT(std::abs(a_naive - a_greedy) < 1e-3,
           "greedy and naive cover the same total face area");
    EXPECT(greedy.quad_count < naive.quad_count,
           "greedy emits strictly fewer quads on non-trivial terrain");
}

// ----- chunk RLE serialize round-trip --------------------------------------

void test_rle_empty_roundtrip() {
    world::Chunk a;
    auto bytes = world::encode_chunk_rle(a);
    world::Chunk b;
    EXPECT(world::decode_chunk_rle(bytes, b), "decode empty chunk succeeds");
    EXPECT(b.solid_count() == 0, "round-tripped empty chunk has no solid blocks");
}

void test_rle_solid_roundtrip() {
    world::Chunk a;
    // A small varied pattern: stepped terrain + a few stones above.
    for (int z = 0; z < world::kChunkSizeZ; ++z) {
        for (int x = 0; x < world::kChunkSizeX; ++x) {
            const int h = 20 + ((x * 3 + z) % 7);
            fill_solid_column(a, x, z, 0, h, world::BlockId::Dirt);
            a.set(x, h, z, world::BlockId::Grass);
        }
    }
    a.set(3, 200, 9, world::BlockId::Stone);

    auto bytes = world::encode_chunk_rle(a);
    EXPECT(bytes.size() < static_cast<std::size_t>(world::kChunkVolume),
           "RLE encoding is smaller than raw kChunkVolume");

    world::Chunk b;
    EXPECT(world::decode_chunk_rle(bytes, b), "decode populated chunk succeeds");
    EXPECT(b.solid_count() == a.solid_count(),
           "round-trip preserves solid_count");

    bool block_match = true;
    for (int y = 0; y < world::kChunkSizeY && block_match; ++y) {
        for (int z = 0; z < world::kChunkSizeZ && block_match; ++z) {
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                if (a.get(x, y, z) != b.get(x, y, z)) {
                    block_match = false;
                    break;
                }
            }
        }
    }
    EXPECT(block_match, "round-trip preserves every block identity");
}

void test_rle_decode_garbage_fails_gracefully() {
    world::Chunk out;
    std::vector<std::uint8_t> junk{0xFF, 0x00, 0xAB};
    // Either decode returns false, or it returns true but the resulting
    // chunk is degenerate. Either way we should not crash.
    bool ok = world::decode_chunk_rle(junk, out);
    EXPECT(!ok || out.solid_count() >= 0,
           "decode of garbage either returns false or yields a degenerate chunk");
}

}  // namespace

int main() {
    std::printf("voxel_tests: running...\n");
    test_aabb_empty_chunk();
    test_aabb_single_block_in_offset_chunk();
    test_aabb_tight_y_spans_full_column();
    test_aabb_tight_y_ignores_air_above();
    test_sections_empty_chunk();
    test_sections_terrain_low();
    test_sections_terrain_spanning_boundary();
    test_section_bounds_in_world_space();
    test_greedy_equals_naive_area_on_simple_terrain();
    test_rle_empty_roundtrip();
    test_rle_solid_roundtrip();
    test_rle_decode_garbage_fails_gracefully();

    std::printf("\nvoxel_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
