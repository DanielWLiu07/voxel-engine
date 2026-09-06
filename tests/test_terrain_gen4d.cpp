// Unit tests for four-dimensional terrain generation.
//
// The 3D generator's tests pin relationships rather than outputs, and the
// same relationships have to hold here - a surface where height_at says it
// is, caves that only remove, material bands that follow altitude. What is
// new is the fourth axis, and the properties worth pinning about it are
// the two that decide whether it is a dimension at all: a slice has to be
// an ordinary usable world, and adjacent slices have to be related.
//
// Both failures are silent. A w that does nothing gives a world that looks
// completely correct and simply has no fourth dimension in it. A w that
// does too much gives a world that teleports, which reads as a rendering
// glitch rather than as a generator bug.

#include "world/terrain_gen4d.h"

#include "world/block.h"
#include "world/chunk.h"
#include "world/terrain_gen.h"  // altitude band constants

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

// Solid/air difference between two chunks, split by direction so "the cave
// pass turned stone into air" and "into dirt" are distinguishable.
struct Diff {
    int only_in_a = 0;
    int only_in_b = 0;
    int different = 0;
};

Diff diff_chunks(const world::Chunk& a, const world::Chunk& b) {
    Diff d;
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

// ----- the fourth axis ------------------------------------------------------

void test_w_actually_changes_the_world() {
    // The silent failure: a generator that ignores w produces a perfectly
    // good world with no fourth dimension in it, and nothing else in these
    // tests would notice.
    const world::TerrainGen4D t(1337);
    int differing = 0, sampled = 0;
    for (int wz = -150; wz <= 150; wz += 11) {
        for (int wx = -150; wx <= 150; wx += 11) {
            ++sampled;
            if (t.height_at(wx, wz, 0) != t.height_at(wx, wz, 3)) ++differing;
        }
    }
    EXPECT(differing > sampled / 2,
           "three steps along w changes most of the heightfield");
}

void test_adjacent_slices_are_related_not_unrelated() {
    // The other half. If w and w+1 were unrelated worlds, the fourth
    // dimension would be a seed change with extra steps - you would not be
    // moving through anything. Adjacent slices must move the surface by a
    // bounded amount, while distant slices are free to differ.
    const world::TerrainGen4D t(1337);
    int worst_adjacent = 0;
    double sum_adjacent = 0.0;
    int worst_distant = 0;
    int n = 0;
    for (int wz = -120; wz <= 120; wz += 7) {
        for (int wx = -120; wx <= 120; wx += 7) {
            const int h0 = t.height_at(wx, wz, 0);
            const int adjacent = std::abs(t.height_at(wx, wz, 1) - h0);
            const int distant  = std::abs(t.height_at(wx, wz, 40) - h0);
            worst_adjacent = std::max(worst_adjacent, adjacent);
            worst_distant  = std::max(worst_distant, distant);
            sum_adjacent += adjacent;
            ++n;
        }
    }
    const double mean_adjacent = sum_adjacent / n;
    // Measured: one step moves a column by about 2 blocks on average and
    // never by more than the low twenties, against a height range of
    // roughly 45 blocks. The bound is loose because this separates
    // "morphs" from "teleports", it does not pin a constant.
    EXPECT(mean_adjacent < 6.0, "one step along w moves a column a few blocks");
    EXPECT(worst_adjacent < 40, "and never resurfaces the world at once");
    EXPECT(worst_distant > worst_adjacent,
           "while a distant slice is free to differ more");
}

void test_a_slice_is_a_deterministic_pure_function() {
    const world::TerrainGen4D a(1337), b(1337);
    bool same_twice = true, same_instance = true;
    for (int wz = -80; wz <= 80; wz += 9)
        for (int wx = -80; wx <= 80; wx += 9)
            for (int w = 0; w < 3; ++w) {
                const int h = a.height_at(wx, wz, w);
                if (h != a.height_at(wx, wz, w)) same_twice = false;
                if (h != b.height_at(wx, wz, w)) same_instance = false;
            }
    EXPECT(same_twice, "height_at is pure across all four axes");
    EXPECT(same_instance, "two generators on one seed agree everywhere");
}

void test_the_seed_reaches_every_slice() {
    // A seed that only reached w=0 would be a plausible bug: the fields
    // are constructed once and sampled per slice.
    const world::TerrainGen4D a(1337), b(1338);
    for (int w = 0; w < 3; ++w) {
        int differing = 0, sampled = 0;
        for (int wz = -100; wz <= 100; wz += 13)
            for (int wx = -100; wx <= 100; wx += 13) {
                ++sampled;
                if (a.height_at(wx, wz, w) != b.height_at(wx, wz, w)) ++differing;
            }
        EXPECT(differing > sampled / 2, "two seeds disagree on this slice");
    }
}

// ----- each slice is an ordinary world --------------------------------------

void test_a_slice_has_no_cliffs() {
    // The same continuity the 3D generator has, checked per slice. Without
    // it the terrain would be unwalkable and the mesher's merge ratio
    // would collapse.
    int worst = 0;
    for (int w = 0; w < 3; ++w) {
        const world::TerrainGen4D t(1337);
        for (int wz = -150; wz <= 150; wz += 3)
            for (int wx = -150; wx <= 150; wx += 3) {
                const int h = t.height_at(wx, wz, w);
                worst = std::max(worst, std::abs(t.height_at(wx + 1, wz, w) - h));
                worst = std::max(worst, std::abs(t.height_at(wx, wz + 1, w) - h));
            }
    }
    EXPECT(worst <= 8, "no adjacent columns differ by more than eight blocks");
    EXPECT(worst >= 2, "and the terrain has relief");
}

void test_fill_chunk_puts_the_surface_where_height_at_says() {
    // The single-source-of-truth claim, per slice. A divergent inline copy
    // would desync physics from the chunk contents, and only on some w.
    world::TerrainGen4D t(1337);
    t.set_caves_enabled(false);
    int wrong_surface = 0, wrong_above = 0, checked = 0;
    for (int w = 0; w < 3; ++w) {
        for (int cz = -1; cz <= 1; ++cz) {
            for (int cx = -1; cx <= 1; ++cx) {
                world::Chunk c;
                t.fill_chunk(cx, cz, w, c);
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x) {
                        ++checked;
                        const int h = t.height_at(cx * world::kChunkSizeX + x,
                                                  cz * world::kChunkSizeZ + z, w);
                        if (!world::is_solid(c.get(x, h, z))) ++wrong_surface;
                        if (h + 1 < world::kChunkSizeY &&
                            c.get(x, h + 1, z) != world::BlockId::Air) {
                            ++wrong_above;
                        }
                    }
            }
        }
    }
    EXPECT(checked == 3 * 9 * 16 * 16, "the sweep covered nine chunks on three slices");
    EXPECT(wrong_surface == 0, "height_at names a solid block in every column");
    EXPECT(wrong_above == 0, "and nothing sits above the surface");
}

void test_a_column_is_solid_all_the_way_down_without_caves() {
    world::TerrainGen4D t(1337);
    t.set_caves_enabled(false);
    int gaps = 0;
    for (int w = 0; w < 2; ++w) {
        world::Chunk c;
        t.fill_chunk(3, -2, w, c);
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                const int h = t.height_at(3 * world::kChunkSizeX + x,
                                          -2 * world::kChunkSizeZ + z, w);
                for (int y = 0; y <= h; ++y)
                    if (!world::is_solid(c.get(x, y, z))) ++gaps;
            }
    }
    EXPECT(gaps == 0, "with caves off every column is solid from 0 to height");
}

void test_caves_only_ever_remove() {
    // The 4D cave pass writes Air and nothing else, exactly like the 3D
    // one. If it ever added, the caves-off baseline would stop being an
    // upper bound and the two worlds would not be comparable.
    world::TerrainGen4D with(1337), without(1337);
    without.set_caves_enabled(false);
    int carved = 0;
    bool subtractive = true;
    for (int w = 0; w < 2; ++w) {
        for (int cz = -1; cz <= 1; ++cz) {
            for (int cx = -1; cx <= 1; ++cx) {
                world::Chunk a, b;
                without.fill_chunk(cx, cz, w, a);
                with.fill_chunk(cx, cz, w, b);
                const Diff d = diff_chunks(a, b);
                carved += d.only_in_a;
                if (d.only_in_b != 0 || d.different != 0) subtractive = false;
            }
        }
    }
    EXPECT(subtractive, "the cave pass removes blocks and never adds or changes one");
    EXPECT(carved > 0, "and actually carved something");
}

void test_caves_move_with_w() {
    // Caves are carved from 4D fields, so passages must open and close as w
    // moves. A cave pass that dropped w would leave the underground
    // identical across slices while the surface morphed - a strange,
    // entirely silent inconsistency.
    const world::TerrainGen4D t(1337);
    world::Chunk a, b;
    t.fill_chunk(0, 0, 0, a);
    t.fill_chunk(0, 0, 2, b);
    int air_a = 0, air_b = 0, differing = 0;
    // Below the shallowest terrain, so surface morphing cannot account
    // for the difference - anything here is the cave field moving.
    for (int y = 1; y < 12; ++y)
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                const bool aa = !world::is_solid(a.get(x, y, z));
                const bool bb = !world::is_solid(b.get(x, y, z));
                if (aa) ++air_a;
                if (bb) ++air_b;
                if (aa != bb) ++differing;
            }
    EXPECT(air_a > 0 && air_b > 0, "there are caves down here on both slices");
    EXPECT(differing > 0, "and they are not in the same places");
}

void test_surface_material_follows_altitude() {
    world::TerrainGen4D t(1337);
    t.set_caves_enabled(false);
    int snow_below_line = 0, grass_above_line = 0, dry_shore = 0;
    int snow_seen = 0, grass_seen = 0, sand_seen = 0;
    int desert_seen = 0, land_seen = 0;
    // Radius 6, not 4. The temperature field runs at 0.006, so its
    // features are ~166 units across and a radius-4 window can sit
    // entirely inside one temperature regime - at an earlier threshold
    // that window contained exactly zero desert columns, so the check
    // below would have been asserting against a sample that could not
    // contain the thing it looks for.
    for (int cz = -6; cz <= 6; ++cz) {
        for (int cx = -6; cx <= 6; ++cx) {
            world::Chunk c;
            t.fill_chunk(cx, cz, 0, c);
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    const int h = t.height_at(cx * world::kChunkSizeX + x,
                                              cz * world::kChunkSizeZ + z, 0);
                    const world::BlockId top = c.get(x, h, z);
                    if (top == world::BlockId::Snow) {
                        ++snow_seen;
                        if (h < world::kSnowBand) ++snow_below_line;
                    }
                    if (top == world::BlockId::Grass) {
                        ++grass_seen;
                        if (h >= world::kSnowBand) ++grass_above_line;
                    }
                    if (top == world::BlockId::Sand) {
                        ++sand_seen;
                        if (h > world::kSeaLevel + world::kSandBand) ++desert_seen;
                    }
                    if (h > world::kSeaLevel + world::kSandBand) ++land_seen;
                    if (h <= world::kSeaLevel + world::kSandBand &&
                        top != world::BlockId::Sand) {
                        ++dry_shore;
                    }
                }
        }
    }
    EXPECT(snow_seen > 0 && grass_seen > 0 && sand_seen > 0,
           "the sweep saw all three surface materials");
    // Sand alone proves nothing about deserts: every shoreline is sand,
    // and about a fifth of the world is below sea level. Deleting the
    // desert branch entirely passed all 29 checks in this file because
    // `sand_seen > 0` was satisfied by beaches. A desert is sand well
    // ABOVE the beach band, which shoreline sand can never be.
    EXPECT(desert_seen > 0, "and saw sand above the beach band, i.e. desert");
    // Bounded from above too, because a threshold that fires everywhere
    // would turn the whole world to sand and still satisfy the check
    // above. Deserts are a minority biome by design.
    EXPECT(desert_seen < land_seen / 2, "but the world is not all desert");
    EXPECT(snow_below_line == 0, "no snow below the snow line");
    EXPECT(grass_above_line == 0, "no grass above the snow line");
    EXPECT(dry_shore == 0, "everything at the waterline is sand");
}

void test_the_world_reaches_below_sea_level() {
    // The amplitude refit exists so a 4D world has coasts. If the height
    // formula were copied from the 3D generator unchanged, every column
    // would sit between y=31 and y=46 - no water, and the whole world in
    // the grass/stone/snow bands. That was the first render, and this is
    // the check that it does not come back.
    const world::TerrainGen4D t(1337);
    int below = 0, above_snow = 0, sampled = 0;
    for (int wz = -200; wz <= 200; wz += 5)
        for (int wx = -200; wx <= 200; wx += 5) {
            const int h = t.height_at(wx, wz, 0);
            ++sampled;
            if (h < world::kSeaLevel) ++below;
            if (h >= world::kSnowBand) ++above_snow;
        }
    EXPECT(below > sampled / 20, "a real share of the world is under water");
    EXPECT(below < sampled * 3 / 4, "but it is not a flooded world");
    EXPECT(above_snow > 0, "and the world reaches the snow line");
}

void test_fill_chunk_is_reproducible() {
    // The generator will be shared by reference across worker threads and
    // called const, exactly as the 3D one is.
    const world::TerrainGen4D t(1337);
    world::Chunk first, second, elsewhere;
    t.fill_chunk(2, -3, 1, first);
    t.fill_chunk(9, 9, 4, elsewhere);   // interleave an unrelated fill
    t.fill_chunk(2, -3, 1, second);
    const Diff d = diff_chunks(first, second);
    EXPECT(d.only_in_a == 0 && d.only_in_b == 0 && d.different == 0,
           "one coordinate and slice fills the same way twice");
    EXPECT(first.solid_count() == second.solid_count(),
           "and holds the same number of solid blocks");
}

void test_neighbouring_chunks_agree_across_the_seam() {
    // Nothing in fill_chunk knows about its neighbours, so the only reason
    // there are no cliffs at chunk borders is that height_at is a function
    // of world position alone. Checked per slice, because a w-dependent
    // seam would only appear on some slices.
    const world::TerrainGen4D t(1337);
    int mismatches = 0;
    for (int w = 0; w < 3; ++w) {
        world::Chunk left, right;
        t.fill_chunk(0, 0, w, left);
        t.fill_chunk(1, 0, w, right);
        for (int z = 0; z < world::kChunkSizeZ; ++z) {
            const int h_left  = t.height_at(world::kChunkSizeX - 1, z, w);
            const int h_right = t.height_at(world::kChunkSizeX, z, w);
            if (!world::is_solid(left.get(world::kChunkSizeX - 1, h_left, z))) ++mismatches;
            if (!world::is_solid(right.get(0, h_right, z))) ++mismatches;
            if (std::abs(h_left - h_right) > 8) ++mismatches;
        }
    }
    EXPECT(mismatches == 0, "the heightfield is continuous across a chunk seam");
}

// ----- what is deliberately NOT tested here ---------------------------------
//
// Two faults were injected and could not be caught by any test here that
// would not be brittle. Both are recorded rather than papered over,
// because a test tuned tightly enough to catch them would fail on ordinary
// noise retuning, and a test that fails for unrelated reasons is worse
// than a gap that is written down.
//
// They share a cause: this generator composes several independent noise
// fields, so breaking one is masked by the others. Isolating a single
// field would need a seam in the class that exists only for a test.
//
// 1. One noise field losing its seed offset. Setting hills_ to a fixed
//    seed changes nothing detectable, because continents_ carries 0.65 of
//    the height and still differs everywhere. test_the_seed_reaches_every_
//    slice catches the seed being dropped entirely, which is the failure
//    that actually matters - --seed silently doing nothing.
//
// 2. One of the two cave fields losing its w dependence. Caves are the
//    intersection of cave_a_ and cave_b_, so freezing one still leaves
//    passages that open and close along w, just less freely. Measured over
//    five seeds, the ratio of cave cells that change per slice is
//    1.63-2.10 correct and 1.26-1.75 with cave_b_ frozen. The ranges
//    overlap, so no bound separates them. test_caves_move_with_w catches
//    the case that matters - both fields losing w, which would freeze the
//    underground while the surface morphed.

}  // namespace

int main() {
    std::printf("terrain4d_tests: running...\n\n");
    test_w_actually_changes_the_world();
    test_adjacent_slices_are_related_not_unrelated();
    test_a_slice_is_a_deterministic_pure_function();
    test_the_seed_reaches_every_slice();
    test_a_slice_has_no_cliffs();
    test_fill_chunk_puts_the_surface_where_height_at_says();
    test_a_column_is_solid_all_the_way_down_without_caves();
    test_caves_only_ever_remove();
    test_caves_move_with_w();
    test_surface_material_follows_altitude();
    test_the_world_reaches_below_sea_level();
    test_fill_chunk_is_reproducible();
    test_neighbouring_chunks_agree_across_the_seam();

    std::printf("\nterrain4d_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
