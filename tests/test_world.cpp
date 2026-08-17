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
#include "world/chunk_mesh.h"
#include "world/chunk_serialize.h"
#include "world/section_visibility.h"
#include "world/world_io.h"
#include "world/world.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// ----- occlusion BFS --------------------------------------------------------

gfx::Frustum frustum_from(const glm::vec3& eye, const glm::vec3& forward,
                          float zfar) {
    gfx::Frustum f;
    const glm::mat4 proj = glm::perspective(glm::radians(70.0f),
                                            16.0f / 9.0f, 0.1f, zfar);
    const glm::mat4 view = glm::lookAt(eye, eye + forward,
                                       glm::vec3(0.0f, 1.0f, 0.0f));
    f.from_view_proj(proj * view);
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
    world::NeighborChunks n{.pos_x = &east};

    // x == world::kChunkSizeX is the first column of the +X neighbour.
    EXPECT(world::sample_with_neighbors(self, n, world::kChunkSizeX, 0, 0) == world::BlockId::Air,
           "sampler reads the neighbour, which is air here");

    east.set(0, 0, 0, world::BlockId::Stone);
    EXPECT(world::sample_with_neighbors(self, n, world::kChunkSizeX, 0, 0) == world::BlockId::Stone,
           "sampler sees the neighbour's block");

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
        world::build_chunk_mesh_greedy(self, world::NeighborChunks{.pos_x = &east});

    // The +X wall is 16 wide by 8 tall and merges into one quad, so
    // exactly one quad should disappear.
    EXPECT(with_east.quad_count == alone.quad_count - 1,
           "the hidden +X wall is no longer emitted");
    EXPECT(with_east.quad_count > 0, "the rest of the mesh survives");

    // Same for the naive mesher, where the wall is 128 separate faces.
    const auto n_alone = world::build_chunk_mesh_naive(self);
    const auto n_east =
        world::build_chunk_mesh_naive(self, world::NeighborChunks{.pos_x = &east});
    EXPECT(n_east.quad_count == n_alone.quad_count - world::kChunkSizeZ * 8,
           "naive mesher drops every face of the hidden wall");
}

static void test_a_gap_in_the_neighbor_keeps_the_face() {
    world::Chunk self = solid_chunk();
    world::Chunk east = solid_chunk();
    // Dig one block out of the neighbour's touching column. That block is
    // now visible through the gap, so its face must survive.
    east.set(0, 4, 5, world::BlockId::Air);

    const world::NeighborChunks n{.pos_x = &east};
    const auto meshed = world::build_chunk_mesh_naive(self, n);
    const auto sealed = world::build_chunk_mesh_naive(self, world::NeighborChunks{});

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
    const auto b = world::build_chunk_mesh_greedy(self, world::NeighborChunks{});
    EXPECT(a.quad_count == b.quad_count,
           "an empty neighbour set meshes exactly as before");
    EXPECT(!world::NeighborChunks{}.any(), "an empty neighbour set reports empty");
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
    test_bfs_solid_chunk_blocks_sightline();
    test_bfs_never_culls_line_of_sight();
    test_atomic_write_creates_then_replaces();
    test_atomic_write_consumes_a_stale_tmp();
    test_atomic_write_missing_dir_fails_cleanly();
    test_sampler_reads_across_the_boundary();
    test_neighbor_culls_the_shared_boundary();
    test_a_gap_in_the_neighbor_keeps_the_face();
    test_missing_neighbor_never_culls();

    std::printf("\nvoxel_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
