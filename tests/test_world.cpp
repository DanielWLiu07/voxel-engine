// Unit tests for the voxel-side bookkeeping: chunk AABB, section bounds,
// greedy/naive mesh equivalence. No GL is touched. Run via ctest after a
// build:
//   cmake --build build -j
//   ctest --test-dir build --output-on-failure
//
// The harness is intentionally minimal: a single EXPECT macro, a failure
// counter, and a main that reports pass/fail. No external test framework.

#include "core/frame_stats.h"
#include "world/block.h"
#include "world/chunk.h"
#include "world/chunk_light.h"
#include "world/chunk_mesh.h"
#include "world/chunk_serialize.h"
#include "world/section_visibility.h"
#include "world/world_io.h"
#include "render/lighting.h"
#include "world/world.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
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
        const auto p0 = m.vertices[4 * q + 0].pos();
        const auto p2 = m.vertices[4 * q + 2].pos();
        const double dx = std::abs(p2.x - p0.x);
        const double dy = std::abs(p2.y - p0.y);
        const double dz = std::abs(p2.z - p0.z);
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

void test_greedy_equals_naive_area_on_perlin_cave_terrain() {
    // Real generator output (caves on) exercises overhangs and interior
    // surfaces the synthetic stepped terrain can't. Any area mismatch means
    // the greedy mesher emitted stray or missing faces - this is the
    // regression test for the floating-quad artifact.
    world::TerrainGen terrain(1337);
    for (int cz = -2; cz <= 2; ++cz) {
        for (int cx = -2; cx <= 2; ++cx) {
            world::Chunk c;
            terrain.fill_chunk(cx, cz, c);
            const auto naive  = world::build_chunk_mesh_naive(c);
            const auto greedy = world::build_chunk_mesh_greedy(c);
            const double a_naive  = total_quad_area(naive);
            const double a_greedy = total_quad_area(greedy);
            if (std::abs(a_naive - a_greedy) >= 1e-3) {
                std::printf("  chunk (%d,%d): naive area %.1f vs greedy %.1f\n",
                            cx, cz, a_naive, a_greedy);
            }
            EXPECT(std::abs(a_naive - a_greedy) < 1e-3,
                   "greedy area matches naive on Perlin cave terrain");
        }
    }
}

void test_greedy_checkerboard_degrades_to_naive() {
    // Adversarial worst case for the merge sweep: isolated blocks in a
    // checkerboard at one y level. No two faces are contiguous, so zero
    // merges are legal - greedy must emit exactly the naive mesh rather
    // than over-merge across the air gaps (a rectangle spanning a gap
    // would invent surface area over cells that have no face).
    world::Chunk c;
    int blocks = 0;
    for (int z = 0; z < world::kChunkSizeZ; ++z) {
        for (int x = 0; x < world::kChunkSizeX; ++x) {
            if ((x + z) % 2 == 0) {
                c.set(x, 10, z, world::BlockId::Stone);
                ++blocks;
            }
        }
    }
    const auto naive  = world::build_chunk_mesh_naive(c);
    const auto greedy = world::build_chunk_mesh_greedy(c);

    EXPECT(naive.quad_count == blocks * 6,
           "naive emits 6 faces per isolated block");
    EXPECT(greedy.quad_count == naive.quad_count,
           "greedy quad count equals naive when nothing can merge");
    EXPECT(std::abs(total_quad_area(naive) - total_quad_area(greedy)) < 1e-3,
           "checkerboard area matches between meshers");
}

void test_greedy_never_merges_across_block_types() {
    // Flat slab, west half Stone, east half Sand. The material seam is
    // invisible to a geometry-only merge, so this guards the rule that
    // quads split on block id - one merged top quad would smear a single
    // texture layer across both halves.
    world::Chunk c;
    for (int z = 0; z < world::kChunkSizeZ; ++z) {
        for (int x = 0; x < world::kChunkSizeX; ++x) {
            c.set(x, 10, z, x < 8 ? world::BlockId::Stone
                                  : world::BlockId::Sand);
        }
    }
    const auto naive  = world::build_chunk_mesh_naive(c);
    const auto greedy = world::build_chunk_mesh_greedy(c);

    bool uniform = true;
    for (std::size_t q = 0; q < greedy.vertices.size() / 4; ++q) {
        const auto id = greedy.vertices[4 * q].block_id;
        for (int k = 1; k < 4; ++k) {
            if (greedy.vertices[4 * q + k].block_id != id) uniform = false;
        }
    }
    EXPECT(uniform, "every greedy quad carries a single block id");

    // Same diagonal trick as total_quad_area, bucketed per material: the
    // per-block-id surface area must survive merging untouched.
    auto area_of = [](const world::ChunkMeshData& m, world::BlockId b) {
        const auto want = static_cast<std::uint8_t>(b);
        double area = 0.0;
        for (std::size_t q = 0; q < m.vertices.size() / 4; ++q) {
            const auto& v0 = m.vertices[4 * q + 0];
            const auto& v2 = m.vertices[4 * q + 2];
            const auto p0 = v0.pos();
            const auto p2 = v2.pos();
            if (v0.block_id != want) continue;
            const double dx = std::abs(p2.x - p0.x);
            const double dy = std::abs(p2.y - p0.y);
            const double dz = std::abs(p2.z - p0.z);
            if      (dx == 0.0) area += dy * dz;
            else if (dy == 0.0) area += dx * dz;
            else                area += dx * dy;
        }
        return area;
    };
    EXPECT(std::abs(area_of(naive, world::BlockId::Stone) -
                    area_of(greedy, world::BlockId::Stone)) < 1e-3,
           "stone surface area survives the merge");
    EXPECT(std::abs(area_of(naive, world::BlockId::Sand) -
                    area_of(greedy, world::BlockId::Sand)) < 1e-3,
           "sand surface area survives the merge");
    EXPECT(greedy.quad_count < naive.quad_count,
           "slab still merges within each material");
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

// Fill every cell of the chunk with one block id.
void fill_chunk_solid(world::Chunk& c, world::BlockId b) {
    for (int y = 0; y < world::kChunkSizeY; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x)
                c.set(x, y, z, b);
}

// Recompute and patch a v3 chunk buffer's CRC (header bytes 0..7 plus the
// payload, skipping the CRC field itself), so corruption tests can get
// past the integrity check and exercise the validation behind it.
void restamp_v3_crc(std::vector<std::uint8_t>& b) {
    std::vector<std::uint8_t> span(b.begin(), b.begin() + 8);
    span.insert(span.end(), b.begin() + world::kChunkFormatHeaderBytes, b.end());
    const std::uint32_t crc = world::crc32_ieee(span.data(), span.size());
    b[8]  = static_cast<std::uint8_t>(crc & 0xFF);
    b[9]  = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    b[10] = static_cast<std::uint8_t>((crc >> 16) & 0xFF);
    b[11] = static_cast<std::uint8_t>((crc >> 24) & 0xFF);
}

void test_rle_decode_rejects_unknown_block_ids() {
    // Encode a valid chunk, then corrupt the first run's id byte to a value
    // no BlockId defines. A corrupt save must fail decode, not smuggle
    // garbage ids into the world (they would render as a clamped wrong
    // texture and break the id -> block invariant everywhere else).
    world::Chunk a;
    fill_chunk_solid(a, world::BlockId::Stone);
    auto bytes = world::encode_chunk_rle(a);
    world::Chunk out;
    EXPECT(world::decode_chunk_rle(bytes, out), "control: intact bytes decode");
    // Re-stamp the CRC after each corruption so decode gets past the
    // integrity check and the id validation itself is what rejects.
    bytes[world::kChunkFormatHeaderBytes] = world::kMaxBlockId + 1;
    restamp_v3_crc(bytes);
    EXPECT(!world::decode_chunk_rle(bytes, out),
           "decode rejects a run whose id no BlockId defines");
    bytes[world::kChunkFormatHeaderBytes] = 0xFF;
    restamp_v3_crc(bytes);
    EXPECT(!world::decode_chunk_rle(bytes, out),
           "decode rejects an 0xFF id byte");
}

void test_edited_flag_roundtrip_and_integrity() {
    // The v3 header carries the edited bit and the CRC covers it: the
    // flag must survive a round trip both ways, and a flipped flag bit in
    // otherwise valid bytes must fail decode rather than silently turn a
    // hand-edited chunk back into a regenerable one.
    world::Chunk a;
    a.set(3, 40, 5, world::BlockId::Stone);
    for (bool edited : {false, true}) {
        auto bytes = world::encode_chunk_rle(a, edited);
        world::Chunk out;
        bool decoded_edited = !edited;  // must be overwritten
        EXPECT(world::decode_chunk_rle(bytes, out, &decoded_edited),
               "control: v3 bytes decode");
        EXPECT(decoded_edited == edited, "edited bit survives the round trip");
        bytes[5] ^= 0x01;  // flip the edited bit, nothing else
        EXPECT(!world::decode_chunk_rle(bytes, out),
               "a flipped edited bit fails the CRC");
    }
    // Unknown flag bits are a format error even with a matching CRC.
    auto bytes = world::encode_chunk_rle(a, false);
    bytes[5] |= 0x02;
    restamp_v3_crc(bytes);
    world::Chunk out;
    EXPECT(!world::decode_chunk_rle(bytes, out),
           "unknown flag bits are rejected even with a matching CRC");
}

void test_world_manifest_roundtrip() {
    namespace fs = std::filesystem;
    const auto dir = (fs::temp_directory_path() / "voxel_manifest_test").string();
    fs::create_directories(dir);
    EXPECT(world::write_world_manifest(dir, 0xDEADBEEFu), "manifest writes");
    std::uint32_t seed = 0;
    EXPECT(world::read_world_manifest(dir, seed), "manifest reads back");
    EXPECT(seed == 0xDEADBEEFu, "manifest seed survives the round trip");
    // Garbage contents are a read failure, not a zero seed.
    {
        std::ofstream f(fs::path(dir) / "world.manifest", std::ios::trunc);
        f << "not a manifest";
    }
    EXPECT(!world::read_world_manifest(dir, seed), "garbage manifest rejected");
    fs::remove_all(dir);
}

void test_crc_known_answer() {
    // The IEEE 802.3 check value: CRC-32 of the ASCII digits "123456789".
    const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT(world::crc32_ieee(digits, sizeof(digits)) == 0xCBF43926u,
           "crc32_ieee matches the standard check value");
}

void test_crc_catches_a_valid_looking_bit_flip() {
    // Corrupt one run's id from Stone to another *defined* id, leaving the
    // run structure intact. Every structural check passes on these bytes;
    // only the checksum knows they are not what was saved.
    world::Chunk a;
    fill_chunk_solid(a, world::BlockId::Stone);
    auto bytes = world::encode_chunk_rle(a);
    world::Chunk out;
    EXPECT(world::decode_chunk_rle(bytes, out), "control: intact bytes decode");
    const auto id_offset = world::kChunkFormatHeaderBytes;
    EXPECT(bytes[id_offset] == static_cast<std::uint8_t>(world::BlockId::Stone),
           "fixture: first run is Stone");
    bytes[id_offset] = static_cast<std::uint8_t>(world::BlockId::Dirt);
    EXPECT(!world::decode_chunk_rle(bytes, out),
           "a bit flip to another valid id fails the CRC, not the structure");
    // And a flipped bit in the stored CRC itself is also fatal.
    bytes[id_offset] = static_cast<std::uint8_t>(world::BlockId::Stone);
    bytes[8] ^= 0x01;
    EXPECT(!world::decode_chunk_rle(bytes, out),
           "a corrupted stored CRC fails decode");
}

// Fill a chunk with one of four random distributions (chosen by style) that
// stress the RLE codec differently. Deterministic given rng.
void fuzz_fill_chunk(world::Chunk& c, std::mt19937& rng, int style) {
    auto rand_block = [&](bool allow_air) {
        const int lo = allow_air ? 0 : 1;
        std::uniform_int_distribution<int> d(lo, 7);
        return static_cast<world::BlockId>(d(rng));
    };
    switch (style % 4) {
        case 0:  // per-cell noise: defeats RLE, exercises many tiny runs
            for (int i = 0; i < world::kChunkVolume; ++i) {
                int y = i / (world::kChunkSizeZ * world::kChunkSizeX);
                int rem = i % (world::kChunkSizeZ * world::kChunkSizeX);
                int z = rem / world::kChunkSizeX, x = rem % world::kChunkSizeX;
                c.set(x, y, z, rand_block(true));
            }
            break;
        case 1: {  // layered terrain: long vertical runs
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    std::uniform_int_distribution<int> hd(1, 200);
                    int h = hd(rng);
                    fill_solid_column(c, x, z, 0, h, rand_block(false));
                }
            break;
        }
        case 2: {  // sparse: mostly air with scattered solids
            std::uniform_int_distribution<int> nd(0, 300);
            int n = nd(rng);
            std::uniform_int_distribution<int> xd(0, world::kChunkSizeX - 1);
            std::uniform_int_distribution<int> yd(0, world::kChunkSizeY - 1);
            std::uniform_int_distribution<int> zd(0, world::kChunkSizeZ - 1);
            for (int k = 0; k < n; ++k)
                c.set(xd(rng), yd(rng), zd(rng), rand_block(false));
            break;
        }
        case 3: {  // wide solid bands: very long single-block runs
            std::uniform_int_distribution<int> bd(1, 40);
            int bands = bd(rng);
            for (int b = 0; b < bands; ++b) {
                std::uniform_int_distribution<int> yd(0, world::kChunkSizeY - 1);
                int y0 = yd(rng), y1 = std::min(world::kChunkSizeY - 1, y0 + 6);
                world::BlockId blk = rand_block(false);
                for (int y = y0; y <= y1; ++y)
                    for (int z = 0; z < world::kChunkSizeZ; ++z)
                        for (int x = 0; x < world::kChunkSizeX; ++x)
                            c.set(x, y, z, blk);
            }
            break;
        }
    }
}

bool chunks_identical(const world::Chunk& a, const world::Chunk& b) {
    for (int y = 0; y < world::kChunkSizeY; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x)
                if (a.get(x, y, z) != b.get(x, y, z)) return false;
    return true;
}

void test_rle_fuzz_roundtrip() {
    // Many randomized chunks across all distribution styles must survive
    // encode -> decode byte-for-byte. Seeded per trial so any failure is
    // reproducible from the trial index.
    bool all_ok = true;
    for (int trial = 0; trial < 240 && all_ok; ++trial) {
        std::mt19937 rng(static_cast<std::uint32_t>(trial * 2654435761u));
        world::Chunk a;
        fuzz_fill_chunk(a, rng, trial);

        auto bytes = world::encode_chunk_rle(a);
        world::Chunk b;
        if (!world::decode_chunk_rle(bytes, b)) { all_ok = false; break; }
        if (a.solid_count() != b.solid_count()) { all_ok = false; break; }
        if (!chunks_identical(a, b)) { all_ok = false; break; }
    }
    EXPECT(all_ok, "RLE round-trip is identity for 240 randomized chunks");
}

void test_rle_full_chunk_boundary() {
    // kChunkVolume (65,536) exceeds the u16 max run length (65,535) by one, so
    // an all-solid chunk must split into two runs. Pin that boundary.
    world::Chunk a;
    fill_chunk_solid(a, world::BlockId::Stone);
    EXPECT(a.solid_count() == world::kChunkVolume, "chunk fully solid");

    auto bytes = world::encode_chunk_rle(a);
    world::Chunk b;
    EXPECT(world::decode_chunk_rle(bytes, b), "decode full-solid chunk succeeds");
    EXPECT(b.solid_count() == world::kChunkVolume,
           "round-trip preserves a fully solid chunk across the run-split");
    EXPECT(chunks_identical(a, b), "every block identical after full-chunk round-trip");
}

void test_rle_decoder_fuzz_no_crash() {
    // The decoder runs on untrusted save files. Feed it thousands of random
    // byte buffers (random lengths, including valid-looking headers) and
    // assert it never crashes and never reports a chunk outside [0, volume].
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> len_d(0, 256);
    std::uniform_int_distribution<int> byte_d(0, 255);
    bool safe = true;
    for (int trial = 0; trial < 5000 && safe; ++trial) {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(len_d(rng)));
        for (auto& byte : buf) byte = static_cast<std::uint8_t>(byte_d(rng));
        // Half the time, prefix a valid magic+version so we exercise the
        // run-parsing path rather than the header rejection path.
        if ((trial & 1) && buf.size() >= world::kChunkFormatHeaderBytes) {
            buf[0] = 'V'; buf[1] = 'C'; buf[2] = 'H'; buf[3] = 'K';
            buf[4] = world::kChunkFormatVersion;
        }
        world::Chunk out;
        bool ok = world::decode_chunk_rle(buf, out);
        if (ok && (out.solid_count() < 0 || out.solid_count() > world::kChunkVolume))
            safe = false;
    }
    EXPECT(safe, "decoder survives 5000 random buffers with no crash / bad state");
}

// ----- section visibility (occlusion culling) ------------------------------

void test_face_pair_bits_unique() {
    unsigned seen = 0;
    for (int a = 0; a < 6; ++a) {
        for (int b = a + 1; b < 6; ++b) {
            const int bit = world::face_pair_bit(a, b);
            EXPECT(bit >= 0 && bit < 15, "pair bit in [0,15)");
            EXPECT(!((seen >> bit) & 1u), "pair bit not reused");
            seen |= 1u << bit;
            EXPECT(world::face_pair_bit(b, a) == bit,
                   "pair bit is order-independent");
        }
    }
    EXPECT(seen == 0x7FFFu, "15 pairs cover exactly 15 bits");
}

void test_visibility_empty_and_solid() {
    world::Chunk c;
    auto vis = world::compute_section_visibility(c);
    for (int sy = 0; sy < world::kSectionsPerChunk; ++sy) {
        EXPECT(vis[sy] == world::kSectionVisAll, "air section fully connected");
    }

    // Fill section 1 (y 32..63) solid; it must block everything while its
    // neighbors stay open.
    for (int y = 32; y < 64; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x)
                c.set(x, y, z, world::BlockId::Stone);
    vis = world::compute_section_visibility(c);
    EXPECT(vis[1] == 0, "solid section has no connectivity");
    EXPECT(vis[0] == world::kSectionVisAll, "section below stays open");
    EXPECT(vis[2] == world::kSectionVisAll, "section above stays open");
}

void test_visibility_slab_blocks_vertical_only() {
    world::Chunk c;
    // Full horizontal slab at y=16: section 0 splits into a lower and an
    // upper air component.
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x)
            c.set(x, 16, z, world::BlockId::Stone);
    auto vis = world::compute_section_visibility(c);
    EXPECT(!world::faces_connected(vis[0], world::kFaceNegY, world::kFacePosY),
           "slab cuts -Y/+Y");
    EXPECT(world::faces_connected(vis[0], world::kFaceNegX, world::kFacePosX),
           "slab keeps -X/+X (both components bridge)");
    EXPECT(world::faces_connected(vis[0], world::kFaceNegY, world::kFaceNegX),
           "lower component links -Y to -X");
    EXPECT(world::faces_connected(vis[0], world::kFacePosY, world::kFacePosZ),
           "upper component links +Y to +Z");
}

void test_visibility_wall_blocks_x_only() {
    world::Chunk c;
    // Full vertical wall at x=8 through section 0.
    for (int y = 0; y < world::kSectionHeight; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            c.set(8, y, z, world::BlockId::Stone);
    auto vis = world::compute_section_visibility(c);
    EXPECT(!world::faces_connected(vis[0], world::kFaceNegX, world::kFacePosX),
           "wall cuts -X/+X");
    EXPECT(world::faces_connected(vis[0], world::kFaceNegZ, world::kFacePosZ),
           "wall keeps -Z/+Z");
    EXPECT(world::faces_connected(vis[0], world::kFaceNegY, world::kFacePosY),
           "wall keeps -Y/+Y");
}

// A second, independent answer to the same question: which pairs of a
// section's six faces are joined by air. compute_section_visibility walks
// components with an explicit DFS stack and accumulates faces as it pops;
// this unions cells pairwise and only looks at faces afterwards. Different
// traversal, different order, same claim - which is the point, because a
// reimplementation that shared the original's structure would share its
// mistakes.
world::SectionVisArray flood_fill_oracle(const world::Chunk& chunk) {
    constexpr int kW = world::kChunkSizeX;
    constexpr int kD = world::kChunkSizeZ;
    constexpr int kH = world::kSectionHeight;
    constexpr int kCells = kW * kH * kD;
    auto idx = [](int x, int ly, int z) { return (ly * kD + z) * kW + x; };

    world::SectionVisArray out{};
    std::vector<int> parent(kCells);
    for (int sy = 0; sy < world::kSectionsPerChunk; ++sy) {
        const int y0 = sy * kH;
        for (int i = 0; i < kCells; ++i) parent[i] = i;

        std::function<int(int)> find = [&](int a) {
            while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; }
            return a;
        };
        auto join = [&](int a, int b) {
            const int ra = find(a), rb = find(b);
            if (ra != rb) parent[ra] = rb;
        };
        auto air = [&](int x, int ly, int z) {
            return !world::is_solid(chunk.get(x, y0 + ly, z));
        };

        for (int ly = 0; ly < kH; ++ly)
            for (int z = 0; z < kD; ++z)
                for (int x = 0; x < kW; ++x) {
                    if (!air(x, ly, z)) continue;
                    if (x + 1 < kW && air(x + 1, ly, z)) join(idx(x, ly, z), idx(x + 1, ly, z));
                    if (ly + 1 < kH && air(x, ly + 1, z)) join(idx(x, ly, z), idx(x, ly + 1, z));
                    if (z + 1 < kD && air(x, ly, z + 1)) join(idx(x, ly, z), idx(x, ly, z + 1));
                }

        // Faces each component touches, then the pairs within each.
        std::vector<std::uint8_t> touched(kCells, 0);
        for (int ly = 0; ly < kH; ++ly)
            for (int z = 0; z < kD; ++z)
                for (int x = 0; x < kW; ++x) {
                    if (!air(x, ly, z)) continue;
                    std::uint8_t m = 0;
                    if (x == 0)      m |= 1u << world::kFaceNegX;
                    if (x == kW - 1) m |= 1u << world::kFacePosX;
                    if (ly == 0)     m |= 1u << world::kFaceNegY;
                    if (ly == kH - 1) m |= 1u << world::kFacePosY;
                    if (z == 0)      m |= 1u << world::kFaceNegZ;
                    if (z == kD - 1) m |= 1u << world::kFacePosZ;
                    touched[find(idx(x, ly, z))] |= m;
                }

        world::SectionVisMask mask = 0;
        for (int i = 0; i < kCells; ++i) {
            if (find(i) != i) continue;
            const std::uint8_t faces = touched[i];
            for (int a = 0; a < 6; ++a) {
                if (!((faces >> a) & 1)) continue;
                for (int b = a + 1; b < 6; ++b) {
                    if ((faces >> b) & 1) {
                        mask |= world::SectionVisMask(1) << world::face_pair_bit(a, b);
                    }
                }
            }
        }
        out[sy] = mask;
    }
    return out;
}

// Randomized fills covering the shapes the occlusion culler actually meets.
//
// Two of these are here because fault injection said so. The worm carves a
// random walk through solid rock, which is the only style that reliably
// makes a component whose lowest cell cannot reach the rest by going up -
// without one, deleting the downward step from the flood fill changed no
// mask in 25 trials. The sparse style leaves a handful of blocks in an
// otherwise empty chunk, which is the only input that can tell
// `chunk.empty()` apart from a fast path that triggers too eagerly.
void fill_for_visibility(world::Chunk& c, std::mt19937& rng, int style) {
    switch (style % 7) {
        case 0: {  // scattered rock: many small components
            std::uniform_int_distribution<int> coin(0, 3);
            for (int y = 0; y < world::kChunkSizeY; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        if (coin(rng) == 0) c.set(x, y, z, world::BlockId::Stone);
            break;
        }
        case 1: {  // horizontal slabs: -Y/+Y cut, sides open
            std::uniform_int_distribution<int> gap(2, 9);
            for (int y = 0; y < world::kChunkSizeY; y += gap(rng))
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        c.set(x, y, z, world::BlockId::Stone);
            break;
        }
        case 2: {  // solid with a few vertical shafts
            for (int y = 0; y < world::kChunkSizeY; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        c.set(x, y, z, world::BlockId::Stone);
            std::uniform_int_distribution<int> pos(0, world::kChunkSizeX - 1);
            for (int shaft = 0; shaft < 4; ++shaft) {
                const int sx = pos(rng), sz = pos(rng);
                for (int y = 0; y < world::kChunkSizeY; ++y)
                    c.set(sx, y, sz, world::BlockId::Air);
            }
            break;
        }
        case 3: {  // real terrain, caves included
            static world::TerrainGen terrain(1337);
            std::uniform_int_distribution<int> coord(-40, 40);
            terrain.fill_chunk(coord(rng), coord(rng), c);
            break;
        }
        case 4: {  // worms: random walks bored through solid rock
            for (int y = 0; y < world::kChunkSizeY; ++y)
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x)
                        c.set(x, y, z, world::BlockId::Stone);
            std::uniform_int_distribution<int> px(0, world::kChunkSizeX - 1);
            std::uniform_int_distribution<int> py(0, world::kChunkSizeY - 1);
            std::uniform_int_distribution<int> step(0, 5);
            static constexpr int kOff[6][3] = {
                {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1}};
            for (int worm = 0; worm < 6; ++worm) {
                int x = px(rng), y = py(rng), z = px(rng);
                for (int i = 0; i < 900; ++i) {
                    c.set(x, y, z, world::BlockId::Air);
                    const auto& o = kOff[step(rng)];
                    const int nx = x + o[0], ny = y + o[1], nz = z + o[2];
                    if (!world::in_chunk_bounds(nx, ny, nz)) continue;
                    x = nx; y = ny; z = nz;
                }
            }
            break;
        }
        case 5: {  // a handful of blocks in an otherwise empty chunk
            std::uniform_int_distribution<int> px(0, world::kChunkSizeX - 1);
            std::uniform_int_distribution<int> py(0, world::kChunkSizeY - 1);
            for (int i = 0; i < 12; ++i) {
                c.set(px(rng), py(rng), px(rng), world::BlockId::Stone);
            }
            break;
        }
        default: break;  // empty
    }
}

// The mask is what the occlusion BFS walks, and getting it wrong is
// invisible in the direction that matters most: a bit set that should not
// be lets the BFS through a wall, which draws more, and drawing more looks
// like an open scene rather than like a bug. The published occlusion
// ratios (up to 70x underground) are exactly the number that would move.
void test_section_visibility_matches_an_independent_flood_fill() {
    std::mt19937 rng(90210);
    int compared = 0, disagreements = 0, nontrivial = 0;
    for (int trial = 0; trial < 28; ++trial) {
        world::Chunk c;
        fill_for_visibility(c, rng, trial);
        const auto got = world::compute_section_visibility(c);
        const auto want = flood_fill_oracle(c);
        for (int sy = 0; sy < world::kSectionsPerChunk; ++sy) {
            ++compared;
            if (got[sy] != want[sy]) ++disagreements;
            if (got[sy] != 0 && got[sy] != world::kSectionVisAll) ++nontrivial;
        }
    }
    EXPECT(compared == 28 * world::kSectionsPerChunk, "every section compared");
    EXPECT(nontrivial > 20,
           "the fills produced partly-connected sections, not just open and solid");
    EXPECT(disagreements == 0,
           "the flood fill agrees with an independent union-find on every section");
}

// Carving air can only join components, never split one, so a section's
// mask can only gain bits. This is the direction the culler is allowed to
// be wrong in - a bit that should be set and is not hides geometry - and
// it is the direction the boundary work already went wrong in once, when
// section_reachable derived its range from mesh AABBs and deleting a wall
// collapsed it.
void test_carving_air_only_ever_adds_sightlines() {
    std::mt19937 rng(31337);
    std::uniform_int_distribution<int> px(0, world::kChunkSizeX - 1);
    std::uniform_int_distribution<int> py(0, world::kChunkSizeY - 1);

    int lost_bits = 0, gained_bits = 0;
    for (int trial = 0; trial < 14; ++trial) {
        world::Chunk c;
        fill_for_visibility(c, rng, trial);
        const auto before = world::compute_section_visibility(c);
        for (int carve = 0; carve < 200; ++carve) {
            c.set(px(rng), py(rng), px(rng), world::BlockId::Air);
        }
        const auto after = world::compute_section_visibility(c);
        for (int sy = 0; sy < world::kSectionsPerChunk; ++sy) {
            if (before[sy] & ~after[sy]) ++lost_bits;
            if (after[sy] & ~before[sy]) ++gained_bits;
        }
    }
    EXPECT(lost_bits == 0, "removing blocks never removes a sightline");
    EXPECT(gained_bits > 0, "the carving actually opened new sightlines");
}

// ----- terrain generation ---------------------------------------------------
//
// terrain_gen.cpp is the input to everything the repo measures. Every
// bench figure, the 10.99 MB the engine validates against, and the
// byte-identical --bench output the CI invariance gate compares are all
// downstream of it, and none of them checks it: they check that it does
// the same thing twice, not that what it does is right. These tests are
// the missing half - the relationships fill_chunk is supposed to hold,
// stated where a change that quietly breaks one has to fail.

// Every solid block a column holds, top-down, for comparing two fills.
// BlockId, not a bool: "the caves pass turned stone into air" and "the
// caves pass turned stone into dirt" are different bugs.
struct ColumnDiff {
    int only_in_a = 0;   // solid in a, air in b
    int only_in_b = 0;   // solid in b, air in a
    int different = 0;   // solid in both, different block
};

ColumnDiff diff_chunks(const world::Chunk& a, const world::Chunk& b) {
    ColumnDiff d;
    for (int y = 0; y < world::kChunkSizeY; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                const world::BlockId ba = a.get(x, y, z);
                const world::BlockId bb = b.get(x, y, z);
                if (ba == bb) continue;
                if (!world::is_solid(bb))      ++d.only_in_a;
                else if (!world::is_solid(ba)) ++d.only_in_b;
                else                           ++d.different;
            }
    return d;
}

void test_height_at_is_deterministic_and_in_range() {
    world::TerrainGen a(1337), b(1337);
    bool same_twice = true, same_instance = true;
    for (int wz = -300; wz <= 300; wz += 7) {
        for (int wx = -300; wx <= 300; wx += 7) {
            const int h = a.height_at(wx, wz);
            if (h != a.height_at(wx, wz)) same_twice = false;
            if (h != b.height_at(wx, wz)) same_instance = false;
        }
    }
    EXPECT(same_twice, "height_at is pure: asking twice gives one answer");
    EXPECT(same_instance,
           "two generators on one seed agree everywhere (no hidden state)");
    // Not checked: that height_at stays inside [1, kChunkSizeY). The
    // clamp is there and it is correct, but the noise it clamps spans
    // roughly 10..66, so the bound cannot be reached and a test for it
    // cannot fail. Deleting the clamp would pass it.
}

void test_the_seed_actually_reaches_the_noise() {
    // The failure this catches is --seed ceasing to reach the generator at
    // all. The world would still generate, still be self-consistent, still
    // pass the invariance gate; it would just stop answering to --seed,
    // and every per-seed figure in the repo would quietly be one world's.
    //
    // What it does not catch, established by injection rather than
    // assumed: one of the six noise fields losing its seed offset while
    // the others keep theirs. continents_ carries 0.65 of the height, so
    // breaking hills_ alone still leaves nearly every column different.
    // Isolating a single field would need a seam in the class that does
    // not exist and should not be added for a test.
    world::TerrainGen a(1337), b(1338);
    int differing = 0, sampled = 0;
    for (int wz = -200; wz <= 200; wz += 13) {
        for (int wx = -200; wx <= 200; wx += 13) {
            ++sampled;
            if (a.height_at(wx, wz) != b.height_at(wx, wz)) ++differing;
        }
    }
    EXPECT(differing > sampled / 2,
           "two seeds disagree about most of the world");
}

void test_neighbouring_chunks_agree_across_the_seam() {
    // Nothing in fill_chunk knows about its neighbours, so the only reason
    // the terrain has no cliffs at chunk borders is that height_at is a
    // function of world position alone. Stated here because the boundary
    // meshing path now depends on it: chunks are meshed against a copy of
    // the neighbour's edge layers, and a seam in the heightfield would show
    // up as a wall of faces rather than as a visible crack.
    world::TerrainGen t(1337);
    world::Chunk left, right;
    t.fill_chunk(0, 0, left);
    t.fill_chunk(1, 0, right);

    int mismatches = 0;
    for (int z = 0; z < world::kChunkSizeZ; ++z) {
        const int wz = z;
        const int h_left  = t.height_at(world::kChunkSizeX - 1, wz);
        const int h_right = t.height_at(world::kChunkSizeX, wz);
        // The two columns are adjacent in the world, so their surfaces
        // must be within a block or two of each other - and each chunk's
        // own contents must match what height_at says for that column.
        if (!world::is_solid(left.get(world::kChunkSizeX - 1, h_left, wz))) ++mismatches;
        if (!world::is_solid(right.get(0, h_right, wz))) ++mismatches;
        if (std::abs(h_left - h_right) > 4) ++mismatches;
    }
    EXPECT(mismatches == 0, "the heightfield is continuous across a chunk seam");
}

void test_fill_chunk_puts_the_surface_where_height_at_says() {
    // The comment in fill_chunk calls height_at the single source of truth
    // for surface height, because a divergent inline copy would desync the
    // physics raycast, the bench grid, and the chunk contents from each
    // other. That is a claim about two functions agreeing, so it is
    // checkable.
    world::TerrainGen t(1337);
    t.set_caves_enabled(false);
    int wrong_surface = 0, wrong_above = 0, checked = 0;
    for (int cz = -1; cz <= 1; ++cz) {
        for (int cx = -1; cx <= 1; ++cx) {
            world::Chunk c;
            t.fill_chunk(cx, cz, c);
            for (int z = 0; z < world::kChunkSizeZ; ++z) {
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    ++checked;
                    const int h = t.height_at(cx * world::kChunkSizeX + x,
                                              cz * world::kChunkSizeZ + z);
                    if (!world::is_solid(c.get(x, h, z))) ++wrong_surface;
                    if (h + 1 >= world::kChunkSizeY) continue;
                    // Above the surface is air, or the bottom of a tree.
                    const world::BlockId above = c.get(x, h + 1, z);
                    if (above != world::BlockId::Air &&
                        above != world::BlockId::Wood &&
                        above != world::BlockId::Leaves) {
                        ++wrong_above;
                    }
                }
            }
        }
    }
    EXPECT(checked == 9 * 16 * 16, "the sweep covered nine chunks");
    EXPECT(wrong_surface == 0, "height_at names a solid block in every column");
    EXPECT(wrong_above == 0, "nothing but foliage sits above the surface");
}

void test_a_column_is_solid_all_the_way_down_without_caves() {
    // Caves off is the bench's configuration, and the greedy ratio it
    // reports means "merged over contiguous terrain". A hole in a column
    // that nothing carved would quietly make that baseline something else.
    world::TerrainGen t(1337);
    t.set_caves_enabled(false);
    world::Chunk c;
    t.fill_chunk(3, -2, c);
    int gaps = 0;
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x) {
            const int h = t.height_at(3 * world::kChunkSizeX + x,
                                      -2 * world::kChunkSizeZ + z);
            for (int y = 0; y <= h; ++y)
                if (!world::is_solid(c.get(x, y, z))) ++gaps;
        }
    EXPECT(gaps == 0, "with caves off every column is solid from 0 to height");
}

void test_caves_only_ever_remove() {
    // The cave pass writes Air and nothing else, so caves-on has to be
    // caves-off minus some blocks: never a different block, never an extra
    // one. If it ever added, the bench's caves-off baseline would stop
    // being an upper bound on the caves-on world and the two greedy ratios
    // (5.33x contiguous, 2.7x with caves) would not be comparable.
    world::TerrainGen with(1337), without(1337);
    without.set_caves_enabled(false);
    int total_carved = 0;
    bool subtractive = true;
    for (int cz = -1; cz <= 1; ++cz) {
        for (int cx = -1; cx <= 1; ++cx) {
            world::Chunk a, b;
            without.fill_chunk(cx, cz, a);
            with.fill_chunk(cx, cz, b);
            const ColumnDiff d = diff_chunks(a, b);
            total_carved += d.only_in_a;
            if (d.only_in_b != 0 || d.different != 0) subtractive = false;
        }
    }
    EXPECT(subtractive, "the cave pass removes blocks and never adds or changes one");
    EXPECT(total_carved > 0, "the cave pass actually carved something");
}

void test_surface_material_follows_altitude() {
    // The bands are the reason mid-altitude terrain reads as grassland
    // rather than as monochrome stone, and they are pure functions of
    // height. Pinned as the ordering between them, not as the numbers:
    // moving kStoneBand is a design change, snow appearing below the snow
    // line is a bug.
    world::TerrainGen t(1337);
    t.set_caves_enabled(false);
    int snow_below_line = 0, grass_above_line = 0, dry_shore = 0;
    int snow_seen = 0, grass_seen = 0, sand_seen = 0;
    // Radius 4, not 2: seed 1337's spawn bowl is high ground and the
    // nearest shoreline is four chunks out, so a smaller sweep sees no
    // sand at all and the coverage check below would be the only thing
    // failing - a test that passes because it looked at nothing.
    for (int cz = -4; cz <= 4; ++cz) {
        for (int cx = -4; cx <= 4; ++cx) {
            world::Chunk c;
            t.fill_chunk(cx, cz, c);
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    const int h = t.height_at(cx * world::kChunkSizeX + x,
                                              cz * world::kChunkSizeZ + z);
                    const world::BlockId top = c.get(x, h, z);
                    if (top == world::BlockId::Snow) {
                        ++snow_seen;
                        if (h < world::kSnowBand) ++snow_below_line;
                    }
                    if (top == world::BlockId::Grass) {
                        ++grass_seen;
                        if (h >= world::kSnowBand) ++grass_above_line;
                    }
                    if (top == world::BlockId::Sand) ++sand_seen;
                    // Everything at or just above the waterline is beach.
                    if (h <= world::kSeaLevel + world::kSandBand &&
                        top != world::BlockId::Sand) {
                        ++dry_shore;
                    }
                }
        }
    }
    EXPECT(snow_seen > 0 && grass_seen > 0 && sand_seen > 0,
           "the sweep saw all three surface materials");
    EXPECT(snow_below_line == 0, "no snow below the snow line");
    EXPECT(grass_above_line == 0, "no grass above the snow line");
    EXPECT(dry_shore == 0, "everything at the waterline is sand");
}

void test_lakes_are_carved_below_the_waterline() {
    // The lake pass exists so the water plane has something to show. It
    // only ever lowers terrain, and the clamp keeps it off the floor.
    world::TerrainGen t(1337);
    int below_sea = 0, at_floor = 0, sampled = 0;
    for (int wz = -600; wz <= 0; wz += 3) {
        for (int wx = 0; wx <= 600; wx += 3) {
            const int h = t.height_at(wx, wz);
            ++sampled;
            if (h < world::kSeaLevel) ++below_sea;
            if (h < 1) ++at_floor;
        }
    }
    EXPECT(below_sea > 0, "somewhere in the lake region the surface is under water");
    EXPECT(below_sea < sampled / 2, "the carve is a lake, not a flooded world");
    // Not checked: that the carve never reaches y = 0. The clamp says so
    // and the lake floor bottoms out around y = 17, so the check passes
    // with the clamp deleted - it was in the first draft and injection
    // took it out.
    (void)at_floor;
}

// The lake pass smoothsteps its basin and fades out as terrain rises,
// which the comment on it explains is so shores slope instead of cliff.
// That is the claim worth pinning, because it is what a change to either
// factor would break, and it breaks invisibly: a cliff-edged lake is a
// perfectly good world, just not the one that was designed.
void test_the_heightfield_has_no_cliffs() {
    int worst = 0;
    for (unsigned seed : {1337u, 7u, 99u}) {
        world::TerrainGen t(seed);
        for (int wz = -220; wz <= 220; wz += 3) {
            for (int wx = -220; wx <= 220; wx += 3) {
                const int h = t.height_at(wx, wz);
                worst = std::max(worst, std::abs(t.height_at(wx + 1, wz) - h));
                worst = std::max(worst, std::abs(t.height_at(wx, wz + 1) - h));
            }
        }
    }
    // Measured at 3 over a million adjacent column pairs on each of these
    // three seeds. The bound is 4 rather than 3 so ordinary noise tuning
    // does not trip it; a lake carving straight down drops ~20 at once,
    // which is what this is here to catch.
    EXPECT(worst <= 4, "no adjacent columns differ by more than four blocks");
    EXPECT(worst >= 2, "the terrain has relief (the sweep was not flat)");
}

void test_trees_stand_on_the_ground_they_were_planted_on() {
    // The three stamps differ only in trunk height and canopy shape, so
    // this is the one place their sizes are written down anywhere the
    // build reads. Their comments said 4-tall and 3x3x2 and had been
    // wrong for a while; a measured test is what makes the corrected
    // comments stay true.
    world::TerrainGen t(1337);
    t.set_caves_enabled(false);
    int trees = 0, floating = 0, wrong_height = 0, bald = 0, misplaced = 0;
    for (int cz = -6; cz <= 6; ++cz) {
        for (int cx = -6; cx <= 6; ++cx) {
            world::Chunk c;
            t.fill_chunk(cx, cz, c);
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    const int h = t.height_at(cx * world::kChunkSizeX + x,
                                              cz * world::kChunkSizeZ + z);
                    if (h + 2 >= world::kChunkSizeY) continue;
                    // Find the trunk rather than assume where it starts.
                    // Looking only at h + 1 was the first draft, and it
                    // could not see the fault it exists to catch: a stamp
                    // planted at h + 2 leaves nothing at h + 1, so the
                    // tree is not found at all and a floating conifer
                    // reads as a chunk with fewer trees in it.
                    int base = -1;
                    for (int y = h + 1; y < h + 12 && y < world::kChunkSizeY; ++y) {
                        if (c.get(x, y, z) == world::BlockId::Wood) { base = y; break; }
                    }
                    if (base < 0) continue;
                    ++trees;
                    if (base != h + 1) ++floating;
                    int trunk = 0;
                    while (base + trunk < world::kChunkSizeY &&
                           c.get(x, base + trunk, z) == world::BlockId::Wood) {
                        ++trunk;
                    }
                    // bush 1, oak 5, conifer 7 - the three stamps, and
                    // nothing in between.
                    if (trunk != 1 && trunk != 5 && trunk != 7) ++wrong_height;
                    // Leaves within three blocks of the trunk top, not
                    // immediately above it: the conifer stacks its canopy
                    // discs at every other level, so on the trunk axis the
                    // block straight above the wood is air and the tip
                    // disc sits one higher. Measured, not assumed - the
                    // first draft of this check required the next block
                    // and every conifer failed it.
                    bool crowned = false;
                    for (int dy = 0; dy < 3 && !crowned; ++dy) {
                        const int y = base + trunk + dy;
                        if (y < world::kChunkSizeY &&
                            c.get(x, y, z) == world::BlockId::Leaves) {
                            crowned = true;
                        }
                    }
                    if (!crowned) ++bald;
                    // Trees are planted only on grass, which is what
                    // keeps them off beaches, deserts, rock and snow -
                    // those bands each put a different block on top, so
                    // this one condition carries all four rules. Checking
                    // the height bands instead was the first draft, and
                    // injection showed it unfalsifiable: the grass gate
                    // already excludes every height they name.
                    if (c.get(x, h, z) != world::BlockId::Grass) ++misplaced;
                }
        }
    }
    EXPECT(trees > 50, "the sweep found trees to check");
    EXPECT(floating == 0, "every trunk starts one block above the surface");
    EXPECT(wrong_height == 0, "every trunk is one of the three stamp heights");
    EXPECT(bald == 0, "every trunk carries leaves above it");
    EXPECT(misplaced == 0, "every tree stands on a grass block");
}

void test_fill_chunk_is_reproducible_from_one_generator() {
    // The generator is shared by reference across nine worker threads and
    // called const. Two fills of one coordinate from one instance have to
    // be byte-identical, or the invariance gate is measuring luck.
    world::TerrainGen t(1337);
    world::Chunk first, second, elsewhere;
    t.fill_chunk(2, -3, first);
    t.fill_chunk(9, 9, elsewhere);   // interleave an unrelated fill
    t.fill_chunk(2, -3, second);
    const ColumnDiff d = diff_chunks(first, second);
    EXPECT(d.only_in_a == 0 && d.only_in_b == 0 && d.different == 0,
           "one coordinate fills the same way twice, whatever ran between");
    EXPECT(first.solid_count() == second.solid_count(),
           "and holds the same number of solid blocks");
}

// ----- frustum --------------------------------------------------------------
//
// Frustum is the one piece of the cull chain with no oracle above it: the
// occlusion BFS is checked against line-of-sight and the mesher against a
// naive reference, but nothing checked the six planes themselves. It is
// also where a wrong answer is least visible. Culling too much punches
// holes in the world, which anyone would see; culling too little just
// draws more than it needed to and reports a smaller cull ratio, which
// looks like a scene that happens to be open.

glm::mat4 view_proj_from(const glm::vec3& eye, const glm::vec3& forward,
                         float zfar) {
    return glm::perspective(glm::radians(70.0f), 16.0f / 9.0f, 0.1f, zfar) *
           glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 1.0f, 0.0f));
}

// True when p lands inside the clip volume the projection defines: what
// the GPU will do with the vertex, computed the long way. This is the
// oracle, and it never touches the plane extraction under test.
bool inside_clip_volume(const glm::mat4& vp, const glm::vec3& p) {
    const glm::vec4 c = vp * glm::vec4(p, 1.0f);
    return c.w > 0.0f &&
           -c.w <= c.x && c.x <= c.w &&
           -c.w <= c.y && c.y <= c.w &&
           -c.w <= c.z && c.z <= c.w;
}

// The property the header states: "No false negatives." A box holding a
// point the GPU would draw must never be culled. Randomized over cameras
// and boxes rather than over hand-picked cases, because the failure this
// guards against - one plane reading the wrong AABB corner - only shows up
// on the side of the volume that plane bounds, and a hand-picked box tends
// to sit in the middle of the screen where every plane agrees.
void test_frustum_never_rejects_a_box_it_can_see() {
    std::mt19937 rng(20260905);
    std::uniform_real_distribution<float> pos(-200.0f, 200.0f);
    std::uniform_real_distribution<float> extent(0.5f, 40.0f);
    std::uniform_real_distribution<float> dir(-1.0f, 1.0f);

    int visible_boxes = 0;
    int false_negatives = 0;
    for (int trial = 0; trial < 4000; ++trial) {
        glm::vec3 fwd(dir(rng), dir(rng) * 0.5f, dir(rng));
        if (glm::length(fwd) < 0.1f) continue;
        fwd = glm::normalize(fwd);
        const glm::vec3 eye(pos(rng), pos(rng) * 0.25f, pos(rng));
        const glm::mat4 vp = view_proj_from(eye, fwd, 300.0f);
        gfx::Frustum f;
        f.from_view_proj(vp);

        const glm::vec3 lo(pos(rng), pos(rng) * 0.25f, pos(rng));
        const gfx::AABB box{lo,
                            lo + glm::vec3(extent(rng), extent(rng), extent(rng))};

        bool any_point_visible = false;
        for (int c = 0; c < 8 && !any_point_visible; ++c) {
            const glm::vec3 p((c & 1) ? box.max.x : box.min.x,
                              (c & 2) ? box.max.y : box.min.y,
                              (c & 4) ? box.max.z : box.min.z);
            any_point_visible = inside_clip_volume(vp, p);
        }
        if (!any_point_visible) {
            any_point_visible = inside_clip_volume(vp, (box.min + box.max) * 0.5f);
        }
        if (!any_point_visible) continue;

        ++visible_boxes;
        if (!f.intersects_aabb(box)) ++false_negatives;
    }
    // Counted rather than short-circuited: bailing on the first failure
    // leaves visible_boxes low and fails the coverage check too, which
    // reads as "the sweep was empty" rather than "the culler is wrong".
    EXPECT(visible_boxes > 100, "the random sweep actually produced visible boxes");
    EXPECT(false_negatives == 0, "no box containing a drawable point was culled");
}

// Each of the six planes has to be doing something on its own. Every box
// here is outside exactly one plane and comfortably inside the other five,
// so a plane that is missing, mis-signed, or a copy of its neighbour
// leaves exactly one of these surviving and the rest still passing.
void test_every_plane_culls_its_own_side() {
    const glm::vec3 eye{0.0f, 0.0f, 0.0f};
    gfx::Frustum f;
    f.from_view_proj(view_proj_from(eye, {0.0f, 0.0f, -1.0f}, 100.0f));

    auto box_at = [](glm::vec3 c, float r) {
        return gfx::AABB{c - glm::vec3(r), c + glm::vec3(r)};
    };
    // 70 degrees vertical at 16:9, so at z = -50 the volume is ~70 units
    // tall and ~124 wide: these sit far past one edge and inside the rest.
    EXPECT(f.intersects_aabb(box_at({0.0f, 0.0f, -50.0f}, 1.0f)),
           "a box straight ahead is kept");
    EXPECT(!f.intersects_aabb(box_at({-200.0f, 0.0f, -50.0f}, 1.0f)),
           "left plane culls a box off the left edge");
    EXPECT(!f.intersects_aabb(box_at({200.0f, 0.0f, -50.0f}, 1.0f)),
           "right plane culls a box off the right edge");
    EXPECT(!f.intersects_aabb(box_at({0.0f, -200.0f, -50.0f}, 1.0f)),
           "bottom plane culls a box below the view");
    EXPECT(!f.intersects_aabb(box_at({0.0f, 200.0f, -50.0f}, 1.0f)),
           "top plane culls a box above the view");
    EXPECT(!f.intersects_aabb(box_at({0.0f, 0.0f, -500.0f}, 1.0f)),
           "far plane culls a box past the draw distance");
    // Behind the camera is NOT an isolating case for the near plane: back
    // there w < 0, so the side planes reject the box whatever the near
    // plane says, and a mis-signed near plane passes this happily. It was
    // the only case here at first and fault injection caught it. This box
    // is in front of the camera and nearer than znear (0.1), which only
    // the near plane can object to.
    EXPECT(!f.intersects_aabb(box_at({0.0f, 0.0f, -0.05f}, 0.02f)),
           "near plane culls a box in front of it but nearer than znear");
    EXPECT(f.intersects_aabb(box_at({0.0f, 0.0f, -0.05f}, 0.5f)),
           "a box straddling the near plane is kept");
    EXPECT(!f.intersects_aabb(box_at({0.0f, 0.0f, 50.0f}, 1.0f)),
           "a box behind the camera is culled");
}

// Growing a box can only add points to it, so it can only add reasons to
// keep it. A culler that answers "outside" for a box containing one that
// answered "inside" is testing something other than containment - the
// p-vertex selection reading the near corner instead of the far one, say.
void test_growing_a_box_never_makes_it_invisible() {
    std::mt19937 rng(778);
    std::uniform_real_distribution<float> pos(-150.0f, 150.0f);
    std::uniform_real_distribution<float> grow(0.1f, 30.0f);
    gfx::Frustum f;
    f.from_view_proj(view_proj_from({5.0f, 20.0f, 5.0f},
                                    {0.3f, -0.2f, -1.0f}, 250.0f));

    int shrank_into_view = 0;
    for (int trial = 0; trial < 3000; ++trial) {
        const glm::vec3 lo(pos(rng), pos(rng), pos(rng));
        const gfx::AABB inner{lo,
                              lo + glm::vec3(grow(rng), grow(rng), grow(rng))};
        const float g = grow(rng);
        const gfx::AABB outer{inner.min - glm::vec3(g), inner.max + glm::vec3(g)};
        if (f.intersects_aabb(inner) && !f.intersects_aabb(outer)) {
            ++shrank_into_view;
        }
    }
    EXPECT(shrank_into_view == 0, "a box containing a visible box is visible");
}

// Not tested here: that from_view_proj normalizes its planes. It was, and
// the test came out after fault injection - stubbing normalize_plane down
// to `return p;` failed nothing, its own test included. Scaling a plane by
// a positive constant cannot change the sign of dot(n, p) + d, and that
// sign is all intersects_aabb looks at, so through the only thing Frustum
// exposes a normalized plane and an unnormalized one are the same object.
// The normalization is real work for a distance query nothing asks for
// yet. A test that cannot fail is worse than no test: it reports coverage
// it does not have.

// ----- occlusion BFS --------------------------------------------------------

gfx::Frustum frustum_from(const glm::vec3& eye, const glm::vec3& forward,
                          float zfar) {
    gfx::Frustum f;
    f.from_view_proj(view_proj_from(eye, forward, zfar));
    return f;
}

void test_bfs_solid_chunk_blocks_sightline() {
    // Three chunks along +X: air, fully solid, air. The camera in chunk 0
    // looking +X must reach the wall chunk's sections (their near faces are
    // visible) but nothing in the far chunk behind it.
    world::Chunk air, wall;
    for (int y = 0; y < world::kChunkSizeY; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x)
                wall.set(x, y, z, world::BlockId::Stone);

    auto vis_air  = world::compute_section_visibility(air);
    auto vis_wall = world::compute_section_visibility(wall);

    auto visibility_of = [&](world::ChunkCoord c) -> const world::SectionVisArray* {
        if (c.z != 0) return nullptr;
        if (c.x == 0) return &vis_air;
        if (c.x == 1) return &vis_wall;
        if (c.x == 2) return &vis_air;
        return nullptr;
    };

    const glm::vec3 eye{8.0f, 40.0f, 8.0f};
    world::SectionReachableMap reachable;
    const bool ok = world::occlusion_bfs(
        eye, frustum_from(eye, {1.0f, 0.0f, 0.0f}, 500.0f),
        visibility_of, reachable);
    EXPECT(ok, "BFS runs when the camera chunk is loaded");
    EXPECT(reachable.count({1, 0}) == 1, "wall chunk is reached (visible faces)");
    EXPECT(reachable.count({2, 0}) == 0, "chunk behind the solid wall is not");

    // Unloaded camera chunk refuses to run.
    world::SectionReachableMap r2;
    EXPECT(!world::occlusion_bfs({500.0f, 40.0f, 500.0f},
                                 frustum_from(eye, {1.0f, 0.0f, 0.0f}, 500.0f),
                                 visibility_of, r2),
           "BFS reports fallback when camera chunk is unloaded");
}

// Line-of-sight property: for real terrain, every air cell a straight
// unobstructed ray passes through (well inside the frustum) must land in a
// BFS-reachable section. Over-culling here is what would show up as holes
// in the rendered world.
void test_bfs_never_culls_line_of_sight() {
    constexpr int kR = 4;  // 9x9 chunk grid
    const int side = 2 * kR + 1;
    world::TerrainGen terrain(1337);

    std::vector<world::Chunk> chunks(static_cast<std::size_t>(side) * side);
    std::vector<world::SectionVisArray> vis(chunks.size());
    for (int cz = -kR; cz <= kR; ++cz) {
        for (int cx = -kR; cx <= kR; ++cx) {
            const std::size_t i =
                static_cast<std::size_t>(cz + kR) * side + (cx + kR);
            terrain.fill_chunk(cx, cz, chunks[i]);
            vis[i] = world::compute_section_visibility(chunks[i]);
        }
    }

    auto chunk_of = [](int w) { return w >= 0 ? w / 16 : (w - 15) / 16; };
    auto index_of = [&](int cx, int cz) -> int {
        if (cx < -kR || cx > kR || cz < -kR || cz > kR) return -1;
        return (cz + kR) * side + (cx + kR);
    };
    auto visibility_of = [&](world::ChunkCoord c) -> const world::SectionVisArray* {
        const int i = index_of(c.x, c.z);
        return i < 0 ? nullptr : &vis[static_cast<std::size_t>(i)];
    };
    auto block_at = [&](int wx, int wy, int wz) -> world::BlockId {
        if (wy < 0 || wy >= world::kChunkSizeY) return world::BlockId::Air;
        const int cx = chunk_of(wx), cz = chunk_of(wz);
        const int i = index_of(cx, cz);
        if (i < 0) return world::BlockId::Air;
        return chunks[static_cast<std::size_t>(i)].get(
            wx - cx * 16, wy, wz - cz * 16);
    };
    auto reachable_has = [&](const world::SectionReachableMap& r,
                             int wx, int wy, int wz) -> bool {
        auto it = r.find({chunk_of(wx), chunk_of(wz)});
        if (it == r.end()) return false;
        return ((it->second >> (wy / world::kSectionHeight)) & 1) != 0;
    };

    const float zfar = static_cast<float>(kR * 16) * 0.95f + 16.0f;

    // Surface pose mirrors --bench; the second pose sits low (cave-ish).
    // The ray fan stays well inside the 70-degree frustum.
    struct Pose { glm::vec3 eye; float yaw_deg; float pitch_deg; };
    const Pose poses[] = {
        {{0.0f, 80.0f, 0.0f}, -90.0f, -15.0f},
        {{8.5f, 30.5f, 8.5f}, -90.0f,   0.0f},
    };

    int rays_checked = 0;
    for (const auto& pose : poses) {
        const float cy = glm::radians(pose.yaw_deg);
        const float cp = glm::radians(pose.pitch_deg);
        const glm::vec3 fwd{std::cos(cy) * std::cos(cp), std::sin(cp),
                            std::sin(cy) * std::cos(cp)};
        const gfx::Frustum f = frustum_from(pose.eye, fwd, zfar);

        world::SectionReachableMap reachable;
        if (!world::occlusion_bfs(pose.eye, f, visibility_of, reachable)) {
            EXPECT(false, "BFS must run for an in-grid pose");
            continue;
        }

        bool all_visible = true;
        for (int dh = -25; dh <= 25 && all_visible; dh += 5) {
            for (int dv = -15; dv <= 15 && all_visible; dv += 5) {
                const float ry = glm::radians(pose.yaw_deg + dh);
                const float rp = glm::radians(pose.pitch_deg + dv);
                const glm::vec3 dir{std::cos(ry) * std::cos(rp), std::sin(rp),
                                    std::sin(ry) * std::cos(rp)};
                for (float t = 0.0f; t < zfar - 16.0f; t += 0.25f) {
                    const glm::vec3 p = pose.eye + dir * t;
                    const int wx = static_cast<int>(std::floor(p.x));
                    const int wy = static_cast<int>(std::floor(p.y));
                    const int wz = static_cast<int>(std::floor(p.z));
                    if (wy < 0 || wy >= world::kChunkSizeY) break;
                    if (index_of(chunk_of(wx), chunk_of(wz)) < 0) break;
                    if (world::is_solid(block_at(wx, wy, wz))) break;
                    if (!reachable_has(reachable, wx, wy, wz)) {
                        std::printf("  LOS miss at (%d,%d,%d) t=%.1f pose y=%.0f\n",
                                    wx, wy, wz, t, pose.eye.y);
                        all_visible = false;
                        break;
                    }
                }
                ++rays_checked;
            }
        }
        EXPECT(all_visible, "every air cell on a clear sightline is reachable");
    }
    EXPECT(rays_checked > 100, "ray fan actually ran");
}

// ----- write_bytes_atomic --------------------------------------------------

namespace fs = std::filesystem;

fs::path make_scratch_dir() {
    fs::path dir = fs::temp_directory_path() / "voxel_atomic_write_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::vector<std::uint8_t> read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

void test_atomic_write_creates_then_replaces() {
    const fs::path dir = make_scratch_dir();
    const fs::path target = dir / "chunk_0_0.vchk";

    const std::vector<std::uint8_t> first = {1, 2, 3, 4};
    EXPECT(world::write_bytes_atomic(target, first), "fresh write succeeds");
    EXPECT(read_all(target) == first, "fresh write lands the exact bytes");
    EXPECT(!fs::exists(dir / "chunk_0_0.vchk.tmp"),
           "no .tmp left after a successful write");

    const std::vector<std::uint8_t> second = {9, 8, 7};
    EXPECT(world::write_bytes_atomic(target, second), "overwrite succeeds");
    EXPECT(read_all(target) == second, "overwrite replaces the content");
    EXPECT(!fs::exists(dir / "chunk_0_0.vchk.tmp"),
           "no .tmp left after an overwrite");
    fs::remove_all(dir);
}

void test_atomic_write_consumes_a_stale_tmp() {
    // A crash between the .tmp write and the rename leaves a stray .tmp.
    // The next save of the same chunk must not be confused by it.
    const fs::path dir = make_scratch_dir();
    const fs::path target = dir / "chunk_1_-2.vchk";
    {
        std::ofstream stale(target.string() + ".tmp", std::ios::binary);
        stale << "torn leftover from a crashed save";
    }
    const std::vector<std::uint8_t> bytes = {42, 42};
    EXPECT(world::write_bytes_atomic(target, bytes),
           "write succeeds over a stale .tmp");
    EXPECT(read_all(target) == bytes, "target holds the new bytes");
    EXPECT(!fs::exists(target.string() + ".tmp"), "stale .tmp is consumed");
    fs::remove_all(dir);
}

void test_atomic_write_missing_dir_fails_cleanly() {
    const fs::path dir = make_scratch_dir();
    const fs::path target = dir / "no_such_subdir" / "chunk_0_0.vchk";
    const std::vector<std::uint8_t> bytes = {1};
    EXPECT(!world::write_bytes_atomic(target, bytes),
           "write into a missing directory reports failure");
    EXPECT(!fs::exists(target), "no target file appears on failure");
    EXPECT(!fs::exists(target.string() + ".tmp"), "no .tmp survives failure");
    fs::remove_all(dir);
}

}  // namespace


// --- frame statistics ------------------------------------------------------
//
// The descheduled-frame attribution decides whether a missed deadline is
// reported as the engine stuttering or the machine stealing the CPU, which
// is a claim the README makes, so it is pinned here rather than trusted.

void test_frame_stats_basic_percentiles() {
    std::vector<double> wall;
    for (int i = 1; i <= 100; ++i) wall.push_back(static_cast<double>(i));
    const auto s = core::compute_frame_stats(wall, {});
    EXPECT(s.frames == 100, "frame stats sees every sample");
    EXPECT(std::fabs(s.avg_ms - 50.5) < 1e-9, "mean of 1..100 is 50.5");
    EXPECT(std::fabs(s.min_ms - 1.0) < 1e-9, "min is the fastest frame");
    EXPECT(std::fabs(s.max_ms - 100.0) < 1e-9, "max is the slowest frame");
    EXPECT(s.p50_ms >= 50.0 && s.p50_ms <= 52.0, "p50 near the middle");
    EXPECT(s.p99_ms >= 99.0, "p99 near the slow tail");
}

// With no stall, the engine's worst-1% must equal the raw worst-1%. This is
// the property that makes engine_low1_fps an attribution rather than a
// flattering recomputation: when nothing was stolen, nothing is subtracted.
void test_frame_stats_quiet_run_leaves_low1_untouched() {
    std::vector<double> wall(300, 4.5);
    std::vector<double> cpu(300, 4.4);   // thread on-core the whole time
    const auto s = core::compute_frame_stats(wall, cpu);
    EXPECT(s.descheduled_frames == 0, "no stalls on a quiet run");
    EXPECT(s.stolen_ms == 0.0, "nothing stolen on a quiet run");
    EXPECT(s.over_budget == 0, "4.5 ms frames are inside a 60 Hz budget");
    EXPECT(std::fabs(s.engine_low1_fps - s.low1_fps) < 1e-9,
           "quiet run: engine 1% low equals raw 1% low");
}

// A frame that blew the deadline while its thread sat off-core is the
// machine's fault, and is charged only the CPU it used.
void test_frame_stats_attributes_a_descheduled_frame() {
    std::vector<double> wall(100, 4.0);
    std::vector<double> cpu(100, 3.9);
    wall[7] = 88.0;   // long frame...
    cpu[7]  = 4.0;    // ...but the thread only ran 4 ms of it
    const auto s = core::compute_frame_stats(wall, cpu);
    EXPECT(s.descheduled_frames == 1, "the stalled frame is identified");
    EXPECT(std::fabs(s.stolen_ms - 84.0) < 1e-9, "stolen time is wall minus cpu");
    EXPECT(s.over_budget == 1, "it still counts as a missed deadline");
    EXPECT(s.engine_low1_fps > s.low1_fps,
           "excusing the stall improves the engine's own 1% low");
}

// A slow frame the engine genuinely spent working must NOT be excused.
void test_frame_stats_does_not_excuse_real_engine_work() {
    std::vector<double> wall(100, 4.0);
    std::vector<double> cpu(100, 3.9);
    wall[3] = 40.0;
    cpu[3]  = 39.0;   // the thread was on-core for essentially all of it
    const auto s = core::compute_frame_stats(wall, cpu);
    EXPECT(s.descheduled_frames == 0, "genuine slow work is not descheduled");
    EXPECT(std::fabs(s.engine_low1_fps - s.low1_fps) < 1e-9,
           "a real slow frame still counts against the engine");
}

// Frames with no paired CPU sample must keep their wall time rather than
// being silently excused.
void test_frame_stats_unpaired_samples_are_not_excused() {
    std::vector<double> wall(100, 4.0);
    wall[9] = 90.0;
    const auto s = core::compute_frame_stats(wall, {});  // no cpu samples
    EXPECT(s.descheduled_frames == 0, "nothing attributed without cpu data");
    EXPECT(std::fabs(s.engine_low1_fps - s.low1_fps) < 1e-9,
           "unmeasured frames keep their wall time");
}

void test_frame_stats_empty_is_safe() {
    const auto s = core::compute_frame_stats({}, {});
    EXPECT(s.frames == 0, "no samples reports no frames");
    EXPECT(s.low1_fps == 0.0, "no samples reports no rate");
}


// ---- Cross-chunk face culling -------------------------------------------
//
// A chunk meshed on its own treats everything outside it as air, so every
// face on its four vertical boundaries is emitted even where the
// neighbouring chunk has solid rock pressed against it. Those faces can
// never be seen by any camera, and they were being uploaded, drawn, and
// counted as merged geometry.
//
// The direction of the error matters and is asserted separately below: a
// missing neighbour must still emit the face. Culling one that might be
// visible is a hole in the world; emitting one that is hidden costs a
// wasted quad.

static world::Chunk solid_chunk() {
    world::Chunk c;
    for (int y = 0; y < 8; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x)
                c.set(x, y, z, world::BlockId::Stone);
    return c;
}

static void test_sampler_reads_across_the_boundary() {
    world::Chunk self = solid_chunk();
    world::Chunk east;                       // all air
    auto n = world::NeighborPlanes::from({.pos_x = &east});

    // x == world::kChunkSizeX is the first column of the +X neighbour.
    EXPECT(world::sample_with_neighbors(self, n, world::kChunkSizeX, 0, 0) == world::BlockId::Air,
           "sampler reads the neighbour, which is air here");

    east.set(0, 0, 0, world::BlockId::Stone);
    // The planes are a snapshot, not a view. This is the property the
    // whole threading story rests on: a worker meshing against them cannot
    // be affected by the main thread mutating or evicting the chunk they
    // came from, so it must NOT see this write.
    EXPECT(world::sample_with_neighbors(self, n, world::kChunkSizeX, 0, 0) == world::BlockId::Air,
           "a snapshot does not follow later writes to the chunk");

    n = world::NeighborPlanes::from({.pos_x = &east});
    EXPECT(world::sample_with_neighbors(self, n, world::kChunkSizeX, 0, 0) == world::BlockId::Stone,
           "re-snapshotting sees the neighbour's block");

    // Without the link the same lookup is air: that is the old behaviour,
    // and it is what makes the boundary faces appear.
    EXPECT(world::sample_with_neighbors(self, {}, world::kChunkSizeX, 0, 0) == world::BlockId::Air,
           "no neighbour link reads as air");

    // Out of the world vertically is always air, neighbours or not.
    EXPECT(world::sample_with_neighbors(self, n, 0, -1, 0) == world::BlockId::Air,
           "below the world is air");
    EXPECT(world::sample_with_neighbors(self, n, 0, world::kChunkSizeY, 0) == world::BlockId::Air,
           "above the world is air");
}

static void test_neighbor_culls_the_shared_boundary() {
    world::Chunk self = solid_chunk();
    world::Chunk east = solid_chunk();

    const auto alone = world::build_chunk_mesh_greedy(self);
    const auto with_east =
        world::build_chunk_mesh_greedy(self, world::NeighborPlanes::from({.pos_x = &east}));

    // The +X wall is 16 wide by 8 tall and merges into one quad, so
    // exactly one quad should disappear.
    EXPECT(with_east.quad_count == alone.quad_count - 1,
           "the hidden +X wall is no longer emitted");
    EXPECT(with_east.quad_count > 0, "the rest of the mesh survives");

    // Same for the naive mesher, where the wall is 128 separate faces.
    const auto n_alone = world::build_chunk_mesh_naive(self);
    const auto n_east =
        world::build_chunk_mesh_naive(self, world::NeighborPlanes::from({.pos_x = &east}));
    EXPECT(n_east.quad_count == n_alone.quad_count - world::kChunkSizeZ * 8,
           "naive mesher drops every face of the hidden wall");
}

static void test_a_gap_in_the_neighbor_keeps_the_face() {
    world::Chunk self = solid_chunk();
    world::Chunk east = solid_chunk();
    // Dig one block out of the neighbour's touching column. That block is
    // now visible through the gap, so its face must survive.
    east.set(0, 4, 5, world::BlockId::Air);

    const auto n = world::NeighborPlanes::from({.pos_x = &east});
    const auto meshed = world::build_chunk_mesh_naive(self, n);
    const auto sealed = world::build_chunk_mesh_naive(self, world::NeighborPlanes{});

    // One face on the wall is still visible, so exactly one fewer face is
    // culled than in the fully sealed case.
    EXPECT(meshed.quad_count == sealed.quad_count - (world::kChunkSizeZ * 8 - 1),
           "the face facing the gap is still emitted");
}

static void test_missing_neighbor_never_culls() {
    world::Chunk self = solid_chunk();
    // No links at all: identical to the pre-neighbour behaviour, which is
    // the conservative direction. A regression that culled here would open
    // holes at the edge of the loaded world.
    const auto a = world::build_chunk_mesh_greedy(self);
    const auto b = world::build_chunk_mesh_greedy(self, world::NeighborPlanes{});
    EXPECT(a.quad_count == b.quad_count,
           "an empty neighbour set meshes exactly as before");
    EXPECT(!world::NeighborPlanes{}.any(), "an empty neighbour set reports empty");
}



// The invariant that makes cross-chunk culling safe, checked against real
// terrain rather than a hand-built fixture: a boundary face is emitted if
// and only if the block across it is not solid.
//
// One direction is the optimisation (a face hidden by the neighbour must
// be gone) and the other is the correctness (a face with air across it
// must survive). Getting the second wrong is a hole in the world, which is
// the failure this whole feature risks and the one a screenshot test on a
// single pose would probably miss.
static void test_boundary_faces_match_the_neighbour_exactly() {
    world::TerrainGen terrain(1337);
    world::Chunk self, west, east, north, south;
    terrain.fill_chunk(0, 0, self);
    terrain.fill_chunk(-1, 0, west);
    terrain.fill_chunk( 1, 0, east);
    terrain.fill_chunk(0, -1, north);
    terrain.fill_chunk(0,  1, south);

    const auto planes = world::NeighborPlanes::from(
        {.neg_x = &west, .pos_x = &east, .neg_z = &north, .pos_z = &south});
    // Naive: one quad per face, no merging, so quads can be counted
    // against block pairs directly.
    const auto mesh = world::build_chunk_mesh_naive(self, planes);

    // Count quads sitting on a given boundary plane with a given normal.
    auto plane_quads = [&](int normal_idx, int axis, float coord) {
        int n = 0;
        for (std::size_t q = 0; q + 3 < mesh.vertices.size(); q += 4) {
            const auto& v = mesh.vertices[q];
            if (v.normal != normal_idx) continue;
            const glm::vec3 p = v.pos();
            if (p[axis] == coord) ++n;
        }
        return n;
    };

    struct Side {
        const char* label;
        int normal_idx;      // index into gfx::kPackedNormals
        int axis;            // which coordinate the boundary plane fixes
        float plane;         // its value at the outer face
        const world::Chunk* other;
        bool along_z;        // the boundary runs along z (X-facing sides)
        int self_fixed;      // the self column touching the boundary
        int other_fixed;     // the neighbour column touching it
    };
    const Side sides[] = {
        {"+X", 0, 0, static_cast<float>(world::kChunkSizeX), &east,  true,
         world::kChunkSizeX - 1, 0},
        {"-X", 1, 0, 0.0f, &west, true, 0, world::kChunkSizeX - 1},
        {"+Z", 4, 2, static_cast<float>(world::kChunkSizeZ), &south, false,
         world::kChunkSizeZ - 1, 0},
        {"-Z", 5, 2, 0.0f, &north, false, 0, world::kChunkSizeZ - 1},
    };

    int total_hidden = 0;
    for (const auto& s : sides) {
        int expect_visible = 0, hidden = 0;
        for (int y = 0; y < world::kChunkSizeY; ++y) {
            for (int t = 0; t < world::kChunkSizeX; ++t) {
                const world::BlockId mine = s.along_z
                    ? self.get(s.self_fixed, y, t)
                    : self.get(t, y, s.self_fixed);
                if (!world::is_solid(mine)) continue;
                const world::BlockId theirs = s.along_z
                    ? s.other->get(s.other_fixed, y, t)
                    : s.other->get(t, y, s.other_fixed);
                if (world::is_solid(theirs)) ++hidden;
                else                         ++expect_visible;
            }
        }
        total_hidden += hidden;
        EXPECT(plane_quads(s.normal_idx, s.axis, s.plane) == expect_visible,
               "boundary faces emitted exactly where the neighbour is not solid");
    }
    // The test is only meaningful if the terrain actually presses solid
    // against solid somewhere along all four sides.
    EXPECT(total_hidden > 1000, "real terrain hides a large number of boundary faces");
}



// ---- Block light propagation --------------------------------------------

static void test_light_falls_off_by_one_per_step() {
    world::Chunk c;                       // all air
    c.set(8, 40, 8, world::BlockId::Glow);
    world::LightGrid g;
    const auto st = world::propagate_light(c, {}, g);

    EXPECT(st.sources == 1, "one emitter seeded");
    EXPECT(g.get(8, 40, 8) == world::kMaxLight, "the source cell is full bright");
    EXPECT(g.get(9, 40, 8) == world::kMaxLight - 1, "one step costs one level");
    EXPECT(g.get(8 + 5, 40, 8) == world::kMaxLight - 5, "five steps cost five");
    // 15 levels means light dies exactly 15 steps out. Measured up the Y
    // axis: a chunk is only 16 wide, so 15 steps in x or z would leave the
    // chunk and read out of bounds.
    EXPECT(g.get(8, 40 + 15, 8) == 0, "light runs out at its range");
    EXPECT(g.get(8, 40 + 14, 8) == 1, "and is still 1 the step before");
}

static void test_light_does_not_pass_through_solids() {
    world::Chunk c;
    // A sealed 1-block pocket: emitter inside a stone shell.
    for (int y = 39; y <= 41; ++y)
        for (int z = 7; z <= 9; ++z)
            for (int x = 7; x <= 9; ++x)
                c.set(x, y, z, world::BlockId::Stone);
    c.set(8, 40, 8, world::BlockId::Glow);

    world::LightGrid g;
    world::propagate_light(c, {}, g);
    EXPECT(g.get(8, 40, 8) == world::kMaxLight, "the emitter still lights itself");
    // Everything outside the shell stays dark: stone does not transmit.
    EXPECT(g.get(8, 40, 6) == 0, "light does not leak through a solid wall");
    EXPECT(g.get(6, 40, 8) == 0, "nor sideways through one");
    EXPECT(g.get(8, 42, 8) == 0, "nor upward through one");
}

// Without this a torch near a chunk edge would light its own chunk and
// stop dead at the boundary - a seam every 16 blocks, which is exactly the
// kind of thing that makes a voxel world look unfinished.
static void test_light_crosses_a_chunk_boundary() {
    world::Chunk c;                       // all air
    world::NeighborLight in;
    in.neg_x.present = true;
    in.neg_x.set(4, 40, 10);              // the neighbour's edge column is at 10

    world::LightGrid g;
    world::propagate_light(c, in, g);
    // Crossing the boundary costs one step, then one per cell after that.
    EXPECT(g.get(0, 40, 4) == 9, "light entering from -X arrives one level down");
    EXPECT(g.get(1, 40, 4) == 8, "and keeps falling off inside this chunk");
    EXPECT(g.get(0, 40, 6) == 7, "it also spreads along the face");
}

static void test_light_grid_is_nibble_packed() {
    // 32 KB per chunk rather than 64: the same refusal the 12-byte vertex
    // makes, applied to the second grid.
    world::LightGrid g;
    EXPECT(g.bytes() == static_cast<std::size_t>(world::kChunkVolume) / 2,
           "two light levels share a byte");
    // Adjacent cells share a byte, so writing one must not disturb the other.
    g.set(0, 0, 0, 15);
    g.set(1, 0, 0, 3);
    EXPECT(g.get(0, 0, 0) == 15, "low nibble survives its neighbour's write");
    EXPECT(g.get(1, 0, 0) == 3,  "high nibble reads back");
}



// The whole pipeline in one check: a light source changes the vertices the
// mesher emits. Without this the propagation could be perfect and the
// renderer would still draw a uniformly bright world, which is exactly the
// state this repo shipped in for one round.
static void test_light_reaches_the_vertices() {
    world::Chunk c;
    for (int z = 0; z < world::kChunkSizeZ; ++z)
        for (int x = 0; x < world::kChunkSizeX; ++x)
            for (int y = 0; y < 40; ++y)
                c.set(x, y, z, world::BlockId::Stone);
    // Carve a pocket and put a source in it.
    for (int y = 20; y <= 24; ++y)
        for (int z = 6; z <= 10; ++z)
            for (int x = 6; x <= 10; ++x)
                c.set(x, y, z, world::BlockId::Air);
    c.set(8, 22, 8, world::BlockId::Glow);

    world::LightGrid g;
    world::propagate_light(c, {}, g);

    const auto unlit = world::build_chunk_mesh_greedy(c);
    const auto lit   = world::build_chunk_mesh_greedy(c, {}, {&g, nullptr});
    EXPECT(unlit.quad_count == lit.quad_count,
           "light changes brightness, never geometry");

    auto max_light = [](const world::ChunkMeshData& m) {
        std::uint8_t hi = 0;
        for (const auto& v : m.vertices) hi = std::max(hi, v.light);
        return hi;
    };
    auto min_light = [](const world::ChunkMeshData& m) {
        std::uint8_t lo = 255;
        for (const auto& v : m.vertices) lo = std::min(lo, v.light);
        return lo;
    };
    // With no grid at all every vertex is full bright: that is the
    // pre-block-light look, and it is what keeps the mesher's default
    // harmless until a caller supplies light.
    EXPECT(min_light(unlit) == world::kMaxLight, "no grid means full bright");
    // With a grid, the pocket walls are lit and the sealed rock is not.
    EXPECT(max_light(lit) > 0, "faces around the source carry light");
    EXPECT(min_light(lit) == 0, "faces sealed in rock stay dark");
    EXPECT(max_light(lit) < world::kMaxLight + 1, "light stays in range");
}



// ----- day/night lighting -------------------------------------------------
//
// compute_lighting is 84 lines of pure math behind one entry point and had
// no coverage. What is worth pinning is not the colour constants - those
// are taste and will be tweaked - but the RELATIONSHIPS between outputs,
// which are load-bearing and which a refactor can break silently.

void test_sun_peaks_at_noon_and_bottoms_at_midnight() {
    const float noon = render::compute_lighting(0.5f).sun_height;
    const float midnight = render::compute_lighting(0.0f).sun_height;
    const float sunrise = render::compute_lighting(0.25f).sun_height;
    const float sunset = render::compute_lighting(0.75f).sun_height;
    EXPECT(noon > 0.9f, "sun is near its zenith at noon");
    EXPECT(midnight < -0.9f, "sun is near its nadir at midnight");
    EXPECT(std::fabs(sunrise) < 0.01f, "sun sits on the horizon at sunrise");
    EXPECT(std::fabs(sunset) < 0.01f, "sun sits on the horizon at sunset");
}

// The moon rides the sun's own arc half a turn behind, which is what lets
// it rise exactly as the sun sets with no second schedule to keep in sync.
// If someone gives the moon its own curve, this is the test that notices.
void test_moon_rides_the_suns_arc_half_a_turn_behind() {
    for (float t = 0.0f; t < 1.0f; t += 0.05f) {
        const render::LightingFrame f = render::compute_lighting(t);
        // Half a turn behind on a shared arc means near-antipodal, but not
        // exactly: celestial_dir carries a constant +0.05 x offset that
        // does not flip with the angle. Tolerate that, reject a moon that
        // has wandered onto a different path.
        const float d = glm::dot(f.sun_dir, f.moon_dir);
        EXPECT(d < -0.95f, "moon is opposite the sun on the shared arc");
    }
    // Sunset: the sun is going down, the moon is coming up.
    const float before = render::compute_lighting(0.74f).moon_dir.y;
    const float after = render::compute_lighting(0.76f).moon_dir.y;
    EXPECT(after > before, "the moon is rising as the sun sets");
    EXPECT(render::compute_lighting(0.74f).sun_dir.y >
           render::compute_lighting(0.76f).sun_dir.y,
           "and the sun is setting while it does");
}

// A day is a loop. Midnight reached by counting up must equal midnight
// reached by wrapping around, or the sky pops once per cycle - which is
// exactly the artefact a long capture would show and a short one would not.
void test_the_day_wraps_without_a_seam() {
    const render::LightingFrame a = render::compute_lighting(0.0f);
    const render::LightingFrame b = render::compute_lighting(0.9999f);
    EXPECT(glm::distance(a.sun_dir, b.sun_dir) < 0.01f,
           "sun direction is continuous across midnight");
    EXPECT(glm::distance(a.moon_dir, b.moon_dir) < 0.01f,
           "moon direction is continuous across midnight");
    EXPECT(glm::distance(a.sky_horizon, b.sky_horizon) < 0.01f,
           "horizon colour is continuous across midnight");
    EXPECT(std::fabs(a.star_fade - b.star_fade) < 0.01f,
           "star fade is continuous across midnight");
}

void test_stars_and_shadows_track_the_sun() {
    EXPECT(render::compute_lighting(0.5f).star_fade == 0.0f,
           "no stars at noon");
    EXPECT(render::compute_lighting(0.0f).star_fade > 0.99f,
           "full stars at midnight");
    EXPECT(render::compute_lighting(0.5f).shadow_strength > 0.99f,
           "shadows at full strength at noon");
    EXPECT(render::compute_lighting(0.0f).shadow_strength == 0.0f,
           "no shadows with the sun below the horizon");
    // Below the horizon the scene is still lit from slightly above, or the
    // ground would be lit from underneath.
    EXPECT(render::compute_lighting(0.0f).light_dir.y > 0.0f,
           "light still comes from above at night");
}

void test_every_direction_is_normalized() {
    for (float t = 0.0f; t < 1.0f; t += 0.037f) {
        const render::LightingFrame f = render::compute_lighting(t);
        EXPECT(std::fabs(glm::length(f.sun_dir) - 1.0f) < 1e-4f,
               "sun_dir is unit length");
        EXPECT(std::fabs(glm::length(f.moon_dir) - 1.0f) < 1e-4f,
               "moon_dir is unit length");
        EXPECT(std::fabs(glm::length(f.light_dir) - 1.0f) < 1e-4f,
               "light_dir is unit length");
    }
}

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
    test_greedy_equals_naive_area_on_perlin_cave_terrain();
    test_greedy_checkerboard_degrades_to_naive();
    test_greedy_never_merges_across_block_types();
    test_rle_empty_roundtrip();
    test_rle_solid_roundtrip();
    test_rle_decode_garbage_fails_gracefully();
    test_rle_decode_rejects_unknown_block_ids();
    test_crc_known_answer();
    test_crc_catches_a_valid_looking_bit_flip();
    test_edited_flag_roundtrip_and_integrity();
    test_world_manifest_roundtrip();
    test_rle_fuzz_roundtrip();
    test_rle_full_chunk_boundary();
    test_rle_decoder_fuzz_no_crash();
    test_face_pair_bits_unique();
    test_visibility_empty_and_solid();
    test_frame_stats_basic_percentiles();
    test_frame_stats_quiet_run_leaves_low1_untouched();
    test_frame_stats_attributes_a_descheduled_frame();
    test_frame_stats_does_not_excuse_real_engine_work();
    test_frame_stats_unpaired_samples_are_not_excused();
    test_frame_stats_empty_is_safe();
    test_visibility_slab_blocks_vertical_only();
    test_visibility_wall_blocks_x_only();
    test_section_visibility_matches_an_independent_flood_fill();
    test_carving_air_only_ever_adds_sightlines();
    test_height_at_is_deterministic_and_in_range();
    test_the_seed_actually_reaches_the_noise();
    test_neighbouring_chunks_agree_across_the_seam();
    test_fill_chunk_puts_the_surface_where_height_at_says();
    test_a_column_is_solid_all_the_way_down_without_caves();
    test_caves_only_ever_remove();
    test_surface_material_follows_altitude();
    test_lakes_are_carved_below_the_waterline();
    test_the_heightfield_has_no_cliffs();
    test_trees_stand_on_the_ground_they_were_planted_on();
    test_fill_chunk_is_reproducible_from_one_generator();
    test_frustum_never_rejects_a_box_it_can_see();
    test_every_plane_culls_its_own_side();
    test_growing_a_box_never_makes_it_invisible();
    test_bfs_solid_chunk_blocks_sightline();
    test_bfs_never_culls_line_of_sight();
    test_atomic_write_creates_then_replaces();
    test_atomic_write_consumes_a_stale_tmp();
    test_atomic_write_missing_dir_fails_cleanly();
    test_sampler_reads_across_the_boundary();
    test_neighbor_culls_the_shared_boundary();
    test_a_gap_in_the_neighbor_keeps_the_face();
    test_missing_neighbor_never_culls();
    test_boundary_faces_match_the_neighbour_exactly();
    test_light_falls_off_by_one_per_step();
    test_light_does_not_pass_through_solids();
    test_light_crosses_a_chunk_boundary();
    test_light_grid_is_nibble_packed();
    test_light_reaches_the_vertices();
    test_sun_peaks_at_noon_and_bottoms_at_midnight();
    test_moon_rides_the_suns_arc_half_a_turn_behind();
    test_the_day_wraps_without_a_seam();
    test_stars_and_shadows_track_the_sun();
    test_every_direction_is_normalized();

    std::printf("\nvoxel_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
