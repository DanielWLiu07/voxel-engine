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
#include <utility>

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
            if (t.height_at(wx, wz, {0, 0.0f}) != t.height_at(wx, wz, {3, 0.0f})) ++differing;
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
            const int h0 = t.height_at(wx, wz, {0, 0.0f});
            const int adjacent = std::abs(t.height_at(wx, wz, {1, 0.0f}) - h0);
            const int distant  = std::abs(t.height_at(wx, wz, {40, 0.0f}) - h0);
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
                const int h = a.height_at(wx, wz, {static_cast<float>(w), 0.0f});
                if (h != a.height_at(wx, wz, {static_cast<float>(w), 0.0f})) same_twice = false;
                if (h != b.height_at(wx, wz, {static_cast<float>(w), 0.0f})) same_instance = false;
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
                if (a.height_at(wx, wz, {static_cast<float>(w), 0.0f}) != b.height_at(wx, wz, {static_cast<float>(w), 0.0f})) ++differing;
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
                const int h = t.height_at(wx, wz, {static_cast<float>(w), 0.0f});
                worst = std::max(worst, std::abs(t.height_at(wx + 1, wz, {static_cast<float>(w), 0.0f}) - h));
                worst = std::max(worst, std::abs(t.height_at(wx, wz + 1, {static_cast<float>(w), 0.0f}) - h));
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
                t.fill_chunk(cx, cz, {static_cast<float>(w), 0.0f}, c);
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x) {
                        ++checked;
                        // height_at is a GUIDE, not the surface.
                        //
                        // It used to be both, and these two checks
                        // asserted it: the block at h is solid, and
                        // nothing but foliage sits above it. Neither
                        // survives a world sliced from a 4D solid rather
                        // than swept from a heightfield - the density
                        // band can carve the ground away under h or leave
                        // a roof above it, which is the entire point.
                        //
                        // What is still true, and worth more: the real
                        // surface stays within the band of the guide. A
                        // guide that stopped predicting the terrain would
                        // break the biome bands, the tree rules and the
                        // waterline, all of which key off it.
                        const int h = t.height_at(cx * world::kChunkSizeX + x,
                                                  cz * world::kChunkSizeZ + z, {static_cast<float>(w), 0.0f});
                        int top = -1;
                        for (int y = world::kChunkSizeY - 1; y >= 0; --y)
                            if (world::is_solid(c.get(x, y, z))) { top = y; break; }
                        if (top < 0 || std::abs(top - h) > 24) ++wrong_surface;
                        // Below the band the world is unconditionally
                        // solid, so that half of the guide is exact.
                        const int floor_of_band = std::max(1, h - 20) - 1;
                        if (!world::is_solid(c.get(x, floor_of_band, z))) ++wrong_above;
                    }
            }
        }
    }
    EXPECT(checked == 3 * 9 * 16 * 16, "the sweep covered nine chunks on three slices");
    EXPECT(wrong_surface == 0, "the real surface stays within the density band of the guide");
    EXPECT(wrong_above == 0, "everything below the band is solid");
}

void test_a_column_is_solid_all_the_way_down_without_caves() {
    world::TerrainGen4D t(1337);
    t.set_caves_enabled(false);
    int gaps = 0;
    for (int w = 0; w < 2; ++w) {
        world::Chunk c;
        t.fill_chunk(3, -2, {static_cast<float>(w), 0.0f}, c);
        for (int z = 0; z < world::kChunkSizeZ; ++z)
            for (int x = 0; x < world::kChunkSizeX; ++x) {
                const int h = t.height_at(3 * world::kChunkSizeX + x,
                                          -2 * world::kChunkSizeZ + z, {static_cast<float>(w), 0.0f});
                // Only up to the floor of the density band. Above it a
                // column may legitimately hold air with solid over it -
                // that is an overhang, and producing them is why the
                // density field exists. Below it nothing may be hollow
                // with caves off.
                const int floor_of_band = std::max(1, h - 20);
                for (int y = 0; y < floor_of_band; ++y)
                    if (!world::is_solid(c.get(x, y, z))) ++gaps;
            }
    }
    EXPECT(gaps == 0, "with caves off nothing below the band is hollow");
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
                without.fill_chunk(cx, cz, {static_cast<float>(w), 0.0f}, a);
                with.fill_chunk(cx, cz, {static_cast<float>(w), 0.0f}, b);
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
    t.fill_chunk(0, 0, {0, 0.0f}, a);
    t.fill_chunk(0, 0, {2, 0.0f}, b);
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
            t.fill_chunk(cx, cz, {0, 0.0f}, c);
            for (int z = 0; z < world::kChunkSizeZ; ++z)
                for (int x = 0; x < world::kChunkSizeX; ++x) {
                    const int h = t.height_at(cx * world::kChunkSizeX + x,
                                              cz * world::kChunkSizeZ + z, {0, 0.0f});
                    // The real topmost solid block, not the block at the
                    // guide height. Those were the same thing while the
                    // world was a heightfield; the density band separated
                    // them, and reading the guide made every biome rule
                    // here test a voxel that is often air.
                    // Past foliage: a tree standing on higher ground
                    // spreads leaves over the column next to it, and once
                    // the density band could lift ground above the guide
                    // those leaves became the topmost solid block of a
                    // shoreline column. Six of them failed the sand rule
                    // for being a tree rather than for being wrong.
                    world::BlockId top = world::BlockId::Air;
                    for (int y = world::kChunkSizeY - 1; y >= 0; --y) {
                        const world::BlockId b = c.get(x, y, z);
                        if (b == world::BlockId::Wood ||
                            b == world::BlockId::Leaves) continue;
                        if (world::is_solid(b)) { top = b; break; }
                    }
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
            const int h = t.height_at(wx, wz, {0, 0.0f});
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
    t.fill_chunk(2, -3, {1, 0.0f}, first);
    t.fill_chunk(9, 9, {4, 0.0f}, elsewhere);   // interleave an unrelated fill
    t.fill_chunk(2, -3, {1, 0.0f}, second);
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
        t.fill_chunk(0, 0, {static_cast<float>(w), 0.0f}, left);
        t.fill_chunk(1, 0, {static_cast<float>(w), 0.0f}, right);
        for (int z = 0; z < world::kChunkSizeZ; ++z) {
            const int h_left  = t.height_at(world::kChunkSizeX - 1, z, {static_cast<float>(w), 0.0f});
            const int h_right = t.height_at(world::kChunkSizeX, z, {static_cast<float>(w), 0.0f});
            // The GUIDE is continuous across the seam; the surface it
            // guides need not be solid at exactly h, because the density
            // band decides that per voxel. Checking the guide is what
            // this test was always about - a chunk that generated its
            // neighbour's terrain would show up here as a jump.
            if (std::abs(h_left - h_right) > 8) ++mismatches;
            // And both sides must still have a floor. y=0 rather than
            // a depth below the guide: where the guide is low - the sea
            // floor - "h minus the band" clamps into the band itself,
            // where air is legal.
            if (!world::is_solid(left.get(world::kChunkSizeX - 1, 0, z))) ++mismatches;
            if (!world::is_solid(right.get(0, 0, z))) ++mismatches;
        }
    }
    EXPECT(mismatches == 0, "the guide height is continuous across a chunk seam");
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

void test_trees_grow_and_respond_to_w() {
    // The 4D world had no trees at all until the stamps were shared with
    // the 3D generator, and a barren world is the most visible difference
    // between the two. This pins that they exist, that they sit on the
    // ground, and - the part that makes them four-dimensional - that
    // travelling along w changes which columns carry them.
    world::TerrainGen4D t(1337);
    t.set_caves_enabled(false);
    auto count_trees = [&](float w) {
        int trees = 0, floating = 0;
        for (int cz = -3; cz <= 3; ++cz)
            for (int cx = -3; cx <= 3; ++cx) {
                world::Chunk c;
                t.fill_chunk(cx, cz, {static_cast<float>(w), 0.0f}, c);
                for (int z = 0; z < world::kChunkSizeZ; ++z)
                    for (int x = 0; x < world::kChunkSizeX; ++x) {
                        const int h = t.height_at(cx * world::kChunkSizeX + x,
                                                  cz * world::kChunkSizeZ + z, {static_cast<float>(w), 0.0f});
                        // Find the trunk rather than assume it starts at
                        // h+1. Looking only there was the first version,
                        // and it could not see the fault it exists to
                        // catch: a stamp planted at h+3 leaves h+1 empty,
                        // so the tree is not counted as floating - it is
                        // not counted at all. The same mistake was made in
                        // the 3D tree test and fixed there.
                        int base = -1;
                        // From the floor of the density band, not from
                        // h+1: a tree stands on the topmost SOLID block,
                        // which the band can lift above the guide or cut
                        // below it. Searching from h+1 missed every trunk
                        // the density had lowered.
                        for (int y = std::max(1, h - 24);
                             y < h + 26 && y < world::kChunkSizeY; ++y) {
                            if (c.get(x, y, z) == world::BlockId::Wood) { base = y; break; }
                        }
                        if (base < 0) continue;
                        ++trees;
                        // A trunk must sit directly on solid ground.
                        // "One above the guide" stopped being the same
                        // statement once the surface could leave the
                        // guide; this is the property that was meant.
                        if (base < 1 || !world::is_solid(c.get(x, base - 1, z)))
                            ++floating;
                    }
            }
        return std::pair<int, int>{trees, floating};
    };

    const auto [t0, floating0] = count_trees(0.0f);
    const auto [t1, floating1] = count_trees(2.0f);
    EXPECT(t0 > 20, "the world grows trees");
    EXPECT(floating0 == 0 && floating1 == 0,
           "every trunk stands on solid ground");
    // Density comes from the 4D biome field while the per-column draw is a
    // fixed 2D hash, so the forest moves with w rather than being painted
    // on a static map. Two slices apart the counts must differ.
    EXPECT(t0 != t1, "the forest changes as you travel along w");
    // Note on what that last check does NOT establish: it does not prove
    // the BIOME field is 4D. The tree count changes between slices anyway,
    // because the terrain under it does - different columns clear the
    // height and grass gates. Freezing the biome's w argument was injected
    // and this test still passed.
    //
    // Measured trees-per-eligible-column across five slices: 0.0147-0.0177
    // with the 4D biome and 0.0143-0.0162 with it frozen. Overlapping, and
    // not separable by any bound that would not also fail on ordinary
    // retuning. The cause is that the biome only shifts density from 0.012
    // to about 0.017, so the fixed per-column hash draw dominates - which
    // is inherited from the 3D generator rather than new here.
    //
    // Left uncovered rather than fixed by inflating that coefficient,
    // which would be tuning the world to suit a test.
    EXPECT(t1 > 0, "and the distant slice still has trees");
}

void test_rotating_the_slice_changes_the_cross_section() {
    // The property that separates a fourth dimension a player can see
    // from one they can only travel through.
    //
    // Translating along w gives a different but equally axis-aligned
    // world: every block is still a whole cube and the scene swaps.
    // Rotating the cut meets the 4D lattice at an angle, so the same
    // structures present different cross-sections - which is what makes
    // them appear to change shape rather than be replaced.
    //
    // Measured over a 192-block square, seed 1337:
    //
    //     rotate 0.002 rad    4.8% of columns change, max jump  1
    //     rotate 0.010       22.9%                          2
    //     rotate 0.030       50.6%                          5
    //     rotate pi/2        95.3%                         34
    //     translate w 0.12   33.8%                          2   (one rebuild)
    //     translate w 2.00   92.1%                         15
    const world::TerrainGen4D t(1337);
    auto compare = [&](world::TerrainGen4D::Slice a,
                       world::TerrainGen4D::Slice b, int z_lo, int z_hi) {
        int diff = 0, n = 0, worst = 0;
        for (int z = z_lo; z < z_hi; z += 2)
            for (int x = -96; x < 96; x += 2) {
                const int ha = t.height_at(x, z, a);
                const int hb = t.height_at(x, z, b);
                if (ha != hb) ++diff;
                worst = std::max(worst, std::abs(ha - hb));
                ++n;
            }
        return std::pair<double, int>{100.0 * diff / n, worst};
    };
    auto changed = [&](world::TerrainGen4D::Slice a,
                       world::TerrainGen4D::Slice b) {
        return compare(a, b, -96, 96);
    };

    const world::TerrainGen4D::Slice flat{0.0f, 0.0f};
    EXPECT(changed(flat, flat).first == 0.0,
           "the same slice is the same world");

    EXPECT(changed(flat, {0.0f, 0.01f}).first > 15.0,
           "a small tilt already reshapes the terrain");

    const auto quarter = changed(flat, {0.0f, 1.5708f});
    EXPECT(quarter.first > 90.0,
           "a quarter turn is a wholly different cross-section");

    // A tilt is not a translation in disguise, and this is the property
    // that says so rather than a comparison of totals.
    //
    // A tilted hyperplane diverges from the original in proportion to the
    // distance from the axis it turns about, so a rotation displaces the
    // far edge of a window far more than the near. A translation moves
    // every point by the same amount, so it is flat across the same
    // bands. That difference is structural: no choice of w reproduces it.
    //
    // Comparing totals instead would be fragile and was: at 0.01 rad the
    // largest move is 2 blocks, exactly what a rebuild-threshold
    // translation gives, so an assertion that rotation always moves
    // columns further than translation is simply false at small angles.
    const auto rot_near = compare(flat, {0.0f, 0.03f}, -32, 32);
    const auto rot_far  = compare(flat, {0.0f, 0.03f}, 64, 128);
    EXPECT(rot_far.first > rot_near.first * 1.5,
           "a rotation changes the far field much more than the near");
    EXPECT(rot_far.second > rot_near.second,
           "and moves it further");

    const auto tr_near = compare(flat, {2.0f, 0.0f}, -32, 32);
    const auto tr_far  = compare(flat, {2.0f, 0.0f}, 64, 128);
    EXPECT(tr_far.first < tr_near.first * 1.5,
           "a translation moves the whole window by the same amount");

    // And it is reversible, like every other motion through this world.
    EXPECT(changed({0.0f, 0.7f}, {0.0f, 0.7f}).first == 0.0,
           "a tilt is a place, not a mutation");
}

void test_w_is_continuous_not_a_staircase_of_worlds() {
    // The header warns that an integer w would make the fourth axis "a
    // menu: you jump between discrete worlds". Nothing checked it.
    //
    // Every w test in this file compares integer slices - {0} against
    // {1}, {3}, {40} - so quantising w inside to_4d, which is exactly the
    // failure the design exists to avoid, left all 65 checks passing.
    // The engine caught it and the generator did not, and the generator
    // is where it belongs: kSliceStepMin is 0.015 and a walk covers 0.4
    // of a w per second, so sub-unit resolution is the whole mechanism.
    //
    // The theta axis has had this test since the wheel landed; this is
    // its counterpart. Twelve steps of 0.05, measured against the
    // previous step rather than against the start, because that is what
    // distinguishes a slide from a sequence of jumps.
    const world::TerrainGen4D t(1337);
    auto step = [&](float a, float b) {
        int diff = 0, n = 0, worst = 0;
        for (int z = -96; z < 96; z += 2)
            for (int x = -96; x < 96; x += 2) {
                const int ha = t.height_at(x, z, {a, 0.0f});
                const int hb = t.height_at(x, z, {b, 0.0f});
                if (ha != hb) ++diff;
                worst = std::max(worst, std::abs(ha - hb));
                ++n;
            }
        return std::pair<double, int>{100.0 * diff / n, worst};
    };

    constexpr float kStep = 0.05f;
    double most = 0.0, least = 100.0;
    for (int i = 1; i <= 12; ++i) {
        const auto d = step((i - 1) * kStep, i * kStep);
        // Every sub-unit step must MOVE something. Under a quantised w
        // eleven of these twelve are identical worlds, which is the
        // failure stated plainly.
        EXPECT(d.first > 1.0, "a fraction of a w changes the world");
        EXPECT(d.second <= 3, "and does not lurch");
        most = std::max(most, d.first);
        least = std::min(least, d.first);
    }
    EXPECT(most < least * 3.0, "every step moves a comparable amount");
}

void test_w_runs_both_ways_from_the_origin() {
    // Negative w is a place, not an error, and nothing sampled one.
    //
    // The whole suite lives at w >= 0, so a sign or truncation mistake
    // anywhere on the axis was invisible - including the difference
    // between floor and truncate, which collapses (-1, 0) onto slice 0
    // and is what keys the edit namespace.
    const world::TerrainGen4D t(1337);
    auto differs = [&](world::TerrainGen4D::Slice a,
                       world::TerrainGen4D::Slice b) {
        int diff = 0;
        for (int z = -64; z < 64; z += 4)
            for (int x = -64; x < 64; x += 4)
                if (t.height_at(x, z, a) != t.height_at(x, z, b)) ++diff;
        return diff;
    };
    EXPECT(differs({-3.0f, 0.0f}, {3.0f, 0.0f}) > 0,
           "w = -3 is not w = +3");
    EXPECT(differs({-0.5f, 0.0f}, {0.5f, 0.0f}) > 0,
           "and the two sides of the origin are different places");
    EXPECT(differs({-2.0f, 0.0f}, {-2.0f, 0.0f}) == 0,
           "a negative slice is still a place, reached the same way twice");
    // Symmetry would be a bug: a field that mirrored about w=0 would make
    // travelling backwards retrace the world you just left.
    EXPECT(differs({-1.0f, 0.0f}, {1.0f, 0.0f}) > 0,
           "the axis is not mirrored about the origin");
}

void test_scrolling_is_a_sweep_not_a_sequence_of_jumps() {
    // The claim the whole wheel rests on: turning it SWEEPS the
    // cross-section rather than stepping between unrelated worlds.
    //
    // Measuring each notch against the START cannot tell those apart -
    // both give a number that grows. The distinguishing measurement is
    // each notch against the PREVIOUS one: a sweep moves the terrain a
    // little at a time and by a consistent amount, while a sequence of
    // jumps moves a lot every time.
    //
    // Ten consecutive notches, 192-block window, seed 1337:
    //
    //     notch   vs previous      vs start
    //       1     7.1% max 1       7.1% max 1
    //       5     7.3% max 1      32.5% max 3
    //      10     7.7% max 1      50.6% max 5
    //
    // Small constant steps, steadily accumulating. Half the world has
    // moved after a short turn of the wheel, one block at a time.
    const world::TerrainGen4D t(1337);
    auto step = [&](float a, float b) {
        int diff = 0, n = 0, worst = 0;
        for (int z = -96; z < 96; z += 2)
            for (int x = -96; x < 96; x += 2) {
                const int ha = t.height_at(x, z, {0.0f, a});
                const int hb = t.height_at(x, z, {0.0f, b});
                if (ha != hb) ++diff;
                worst = std::max(worst, std::abs(ha - hb));
                ++n;
            }
        return std::pair<double, int>{100.0 * diff / n, worst};
    };

    constexpr float kNotch = 0.003f;      // main's scroll scale
    double most = 0.0, least = 100.0;
    for (int i = 1; i <= 10; ++i) {
        const auto d = step((i - 1) * kNotch, i * kNotch);
        // No notch may lurch. One block is the whole budget: a notch that
        // moved a column by five would read as the terrain snapping.
        EXPECT(d.second <= 2, "a single notch never lurches");
        most = std::max(most, d.first);
        least = std::min(least, d.first);
    }
    // And every notch does about the same amount of work, so the sweep
    // has no dead zones and no sudden bursts. Measured spread is
    // 6.8-7.9%; a factor of two is loose enough not to be brittle and
    // tight enough to catch a rotation that stalls or accelerates.
    EXPECT(most < least * 2.0, "every notch moves the world by about the same");

    // Cumulatively it still goes somewhere: small steps, not no steps.
    const auto ten = step(0.0f, 10.0f * kNotch);
    EXPECT(ten.first > 40.0, "ten notches move half the window");
    EXPECT(ten.second > 2, "and by more than any single notch did");
}

void test_the_second_rotation_plane_reaches_what_the_first_cannot() {
    // 4D Miner turns the cut in two planes: the wheel and vertical mouse
    // in ZW, horizontal mouse in XW. One plane alone leaves a whole axis
    // of 4D orientation unreachable - you can lean the world away from
    // you but never sideways - so this pins that the second plane exists,
    // does something, and does something DIFFERENT from the first.
    const world::TerrainGen4D t(1337);
    auto changed = [&](world::TerrainGen4D::Slice a,
                       world::TerrainGen4D::Slice b) {
        int diff = 0, n = 0;
        for (int z = -96; z < 96; z += 2)
            for (int x = -96; x < 96; x += 2) {
                if (t.height_at(x, z, a) != t.height_at(x, z, b)) ++diff;
                ++n;
            }
        return 100.0 * diff / n;
    };
    const world::TerrainGen4D::Slice flat{};

    // It does something.
    world::TerrainGen4D::Slice xw{}; xw.phi = 0.05f;
    EXPECT(changed(flat, xw) > 20.0, "turning in XW changes the world");

    // And something a ZW turn cannot reproduce. Both planes move a
    // similar TOTAL, so a totals comparison would prove nothing; what
    // separates them is direction. A ZW turn leaves the x axis alone, an
    // XW turn leaves z alone, so each has a row the other cannot touch.
    world::TerrainGen4D::Slice zw{}; zw.theta = 0.05f;
    int zw_moved_on_x_axis = 0, xw_moved_on_x_axis = 0;
    for (int x = -96; x < 96; x += 2) {
        // The row z = 0: a ZW rotation about the origin cannot move it,
        // because every point on it has slice-z zero.
        if (t.height_at(x, 0, flat) != t.height_at(x, 0, zw)) ++zw_moved_on_x_axis;
        if (t.height_at(x, 0, flat) != t.height_at(x, 0, xw)) ++xw_moved_on_x_axis;
    }
    EXPECT(zw_moved_on_x_axis == 0,
           "a ZW turn cannot move the row it turns about");
    EXPECT(xw_moved_on_x_axis > 0,
           "an XW turn moves exactly that row, which is why it is needed");

    // Reversible, like every other motion through this world.
    world::TerrainGen4D::Slice back{}; back.phi = 0.05f;
    EXPECT(changed(xw, back) == 0.0, "an XW tilt is a place, not a mutation");

    // The two planes compose rather than cancelling.
    world::TerrainGen4D::Slice both{}; both.theta = 0.05f; both.phi = 0.05f;
    EXPECT(changed(zw, both) > 10.0, "the planes are independent");
}

void test_a_tilted_slice_is_still_the_same_kind_of_world() {
    // Rotating the cut must change WHICH world you see, not what kind of
    // world it is. A slice at 45 degrees should be as walkable, as smooth
    // and as isotropic as one at 0 - it is the same 4D terrain, met at a
    // different angle.
    //
    // This is the invariant that caught kWScale being applied after the
    // rotation instead of before. to_4d was a proper rotation, but it
    // rotated a space six times finer along w than along z, so the
    // composed map had singular values 1 and 6 and progressively squashed
    // the world along z as the tilt grew. Measured as the ratio of mean
    // |dh| along z to along x:
    //
    //     theta      before      after
    //     0.00        1.00       0.97
    //     0.25        1.73       0.97
    //     pi/2        4.79       1.02
    //
    // and the largest step between adjacent columns went 3 -> 12 across
    // the same range, which is terrain corrugated into ridges running
    // across z. test_a_slice_has_no_cliffs asserted a maximum of 8, and
    // only ever checked theta = 0.
    const world::TerrainGen4D t(1337);
    for (const float theta : {0.0f, 0.25f, 0.7f, 1.2f, 1.5708f}) {
        double along_x = 0.0, along_z = 0.0;
        int worst_x = 0, worst_z = 0, n = 0;
        for (int z = -120; z < 120; ++z) {
            for (int x = -120; x < 120; ++x) {
                const int h  = t.height_at(x, z, {0.0f, theta});
                const int hx = t.height_at(x + 1, z, {0.0f, theta});
                const int hz = t.height_at(x, z + 1, {0.0f, theta});
                along_x += std::abs(h - hx);
                along_z += std::abs(h - hz);
                worst_x = std::max(worst_x, std::abs(h - hx));
                worst_z = std::max(worst_z, std::abs(h - hz));
                ++n;
            }
        }
        along_x /= n;
        along_z /= n;
        // No cliffs, at EVERY tilt rather than only at zero.
        EXPECT(worst_x <= 8 && worst_z <= 8,
               "a tilted slice has no cliffs either");
        // And no axis is smoother than another. A 25% band: the fields
        // are isotropic by construction and the measured spread across
        // these angles is 0.97-1.03, so this fails long before the
        // corrugation above would be visible.
        const double ratio = along_z / along_x;
        EXPECT(ratio > 0.75 && ratio < 1.33,
               "a tilt does not squash one axis against the other");
    }
}

}  // namespace

int main() {
    std::printf("terrain4d_tests: running...\n\n");
    test_w_actually_changes_the_world();
    test_rotating_the_slice_changes_the_cross_section();
    test_w_is_continuous_not_a_staircase_of_worlds();
    test_w_runs_both_ways_from_the_origin();
    test_scrolling_is_a_sweep_not_a_sequence_of_jumps();
    test_the_second_rotation_plane_reaches_what_the_first_cannot();
    test_a_tilted_slice_is_still_the_same_kind_of_world();
    test_adjacent_slices_are_related_not_unrelated();
    test_a_slice_is_a_deterministic_pure_function();
    test_the_seed_reaches_every_slice();
    test_a_slice_has_no_cliffs();
    test_fill_chunk_puts_the_surface_where_height_at_says();
    test_a_column_is_solid_all_the_way_down_without_caves();
    test_caves_only_ever_remove();
    test_caves_move_with_w();
    test_trees_grow_and_respond_to_w();
    test_surface_material_follows_altitude();
    test_the_world_reaches_below_sea_level();
    test_fill_chunk_is_reproducible();
    test_neighbouring_chunks_agree_across_the_seam();

    std::printf("\nterrain4d_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
