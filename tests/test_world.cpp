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
#include "world/section_visibility.h"
#include "world/world.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
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

void test_greedy_equals_naive_area_on_perlin_cave_terrain() {
    // Real generator output (caves on) exercises overhangs and interior
    // surfaces the synthetic stepped terrain can't. Any area mismatch means
    // the greedy mesher emitted stray or missing faces — this is the
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
    test_greedy_equals_naive_area_on_perlin_cave_terrain();
    test_rle_empty_roundtrip();
    test_rle_solid_roundtrip();
    test_rle_decode_garbage_fails_gracefully();
    test_face_pair_bits_unique();
    test_visibility_empty_and_solid();
    test_visibility_slab_blocks_vertical_only();
    test_visibility_wall_blocks_x_only();
    test_bfs_solid_chunk_blocks_sightline();
    test_bfs_never_culls_line_of_sight();

    std::printf("\nvoxel_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
