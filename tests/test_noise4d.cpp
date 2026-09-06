// Unit tests for the 4D gradient noise.
//
// This is the one piece of a four-dimensional terrain generator that
// cannot be borrowed from the 3D engine, because FastNoiseLite stops at
// three arguments. Hand-written noise is also the easiest thing in a
// terrain pipeline to get subtly wrong: a bad hash, a gradient table with
// a zero row, or a fade curve that does not pin its endpoints all produce
// a field that still looks like noise. It just has grid artifacts, or a
// dead axis, or seams the lighting will find later.
//
// Each of those has its own guard, and they are not interchangeable - the
// lattice identity below covers the fade endpoints only, the table is
// asserted structurally at compile time, and the hash is covered by the
// seed tests. Assuming one test covered all three is how the lattice
// check spent a while claiming more than it did.
//
// So the tests here are the properties that separate correct gradient
// noise from something that merely looks random, with the exact identity
// (zero at every integer lattice point) as the anchor.

#include "world/noise4d.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>

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

// ----- the exact identity ---------------------------------------------------

void test_noise_is_exactly_zero_at_lattice_points() {
    // Exact, not a tolerance - the one thing in this file checkable on the
    // nose. But narrower than it first appears, and the header explains
    // why: fade(0) = 0 makes every lerp weight zero, so the interpolation
    // selects corner 0 alone, whose distance vector is the zero vector.
    //
    // That means this tests the fade curve's endpoints and nothing else.
    // It does NOT test the hash or the gradient table, which the first
    // version of this comment claimed: zeroing a gradient row passes it,
    // and so does a hash returning a constant. Both verified by
    // injection. Those are covered by the static_assert on the table and
    // by the seed tests respectively.
    const world::Noise4D noise(1337);
    bool all_zero = true;
    float worst = 0.0f;
    for (int w = -3; w <= 3; ++w)
        for (int z = -3; z <= 3; ++z)
            for (int y = -3; y <= 3; ++y)
                for (int x = -3; x <= 3; ++x) {
                    const float v = noise.sample(
                        static_cast<float>(x), static_cast<float>(y),
                        static_cast<float>(z), static_cast<float>(w));
                    worst = std::max(worst, std::fabs(v));
                    if (v != world::kLatticeZero) all_zero = false;
                }
    EXPECT(all_zero, "noise is exactly 0 at every integer lattice point");
    if (!all_zero) std::printf("    (worst |v| at a lattice point: %g)\n", worst);
}

void test_noise_is_not_zero_between_lattice_points() {
    // The companion to the identity above: a field that returned zero
    // everywhere would pass the lattice test perfectly. This is what says
    // the first test is measuring a property rather than a dead function.
    const world::Noise4D noise(1337);
    int nonzero = 0;
    for (int i = 0; i < 200; ++i) {
        const float t = 0.5f + static_cast<float>(i);
        if (noise.sample(t, t * 0.37f + 0.5f, t * 0.11f + 0.5f, t * 0.71f + 0.5f)
            != 0.0f) {
            ++nonzero;
        }
    }
    EXPECT(nonzero > 190, "the field is alive between lattice points");
}

// ----- determinism ----------------------------------------------------------

void test_sampling_is_pure_and_seed_stable() {
    const world::Noise4D a(1337), b(1337);
    bool same_twice = true, same_instance = true;
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> pos(-100.0f, 100.0f);
    for (int i = 0; i < 4000; ++i) {
        const float x = pos(rng), y = pos(rng), z = pos(rng), w = pos(rng);
        const float v = a.sample(x, y, z, w);
        if (v != a.sample(x, y, z, w)) same_twice = false;
        if (v != b.sample(x, y, z, w)) same_instance = false;
    }
    EXPECT(same_twice, "sampling twice gives one answer");
    EXPECT(same_instance, "two generators on one seed agree everywhere");
}

void test_the_seed_reaches_the_field() {
    const world::Noise4D a(1337), b(1338);
    int differing = 0, sampled = 0;
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> pos(-100.0f, 100.0f);
    for (int i = 0; i < 2000; ++i) {
        const float x = pos(rng), y = pos(rng), z = pos(rng), w = pos(rng);
        ++sampled;
        if (a.sample(x, y, z, w) != b.sample(x, y, z, w)) ++differing;
    }
    EXPECT(differing > sampled * 9 / 10, "two seeds disagree almost everywhere");
}

void test_edge_seeds_behave() {
    // A hash that mixes the seed by multiplication is most likely to fail
    // at 0 (the term vanishes) and at UINT32_MAX. Both have to produce a
    // live field, and a different one from each other.
    const world::Noise4D zero(0), max_seed(4294967295u);
    int zero_alive = 0, max_alive = 0, differ = 0;
    for (int i = 0; i < 500; ++i) {
        const float t = 0.5f + static_cast<float>(i) * 1.7f;
        const float a = zero.sample(t, t * 0.3f, t * 0.7f, t * 0.13f);
        const float b = max_seed.sample(t, t * 0.3f, t * 0.7f, t * 0.13f);
        if (a != 0.0f) ++zero_alive;
        if (b != 0.0f) ++max_alive;
        if (a != b) ++differ;
    }
    EXPECT(zero_alive > 490, "seed 0 produces a live field");
    EXPECT(max_alive > 490, "seed UINT32_MAX produces a live field");
    EXPECT(differ > 490, "the two edge seeds produce different fields");
}

// ----- range ----------------------------------------------------------------

void test_output_stays_in_range() {
    // The normalizer in noise4d.cpp is derived from a measured peak of
    // 1.2244 (src/bench/noise4d_range.cpp). This is that bound, enforced.
    float peak = 0.0f;
    for (std::uint32_t seed : {1337u, 7u, 99u, 0u, 4294967295u}) {
        const world::Noise4D noise(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> pos(-400.0f, 400.0f);
        for (int i = 0; i < 120000; ++i) {
            peak = std::max(peak, std::fabs(
                noise.sample(pos(rng), pos(rng), pos(rng), pos(rng))));
        }
    }
    EXPECT(peak <= 1.0f, "no sample escapes [-1, 1]");
    // Without this the range check passes for a field scaled to nothing.
    EXPECT(peak > 0.7f, "the field actually uses its range");
}

void test_fbm_keeps_the_single_octave_range() {
    // fbm divides by the amplitude sum, so stacking octaves must not push
    // the result outside the range one octave occupies. A missing
    // normalization here is invisible until terrain starts clipping at its
    // extremes, which reads as flat-topped mountains rather than as a bug.
    const world::Noise4D noise(1337);
    float peak = 0.0f;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> pos(-200.0f, 200.0f);
    for (int i = 0; i < 60000; ++i) {
        peak = std::max(peak, std::fabs(noise.fbm(
            pos(rng), pos(rng), pos(rng), pos(rng), 4, 0.03f)));
    }
    EXPECT(peak <= 1.0f, "four fbm octaves stay in [-1, 1]");
    EXPECT(peak > 0.2f, "fbm output is not collapsed to nothing");
}

void test_fbm_octaves_add_detail() {
    // More octaves must change the field, or the octave loop is not
    // running. Comparing 1 against 4 rather than asserting a magnitude,
    // because the point is that the extra octaves contribute at all.
    const world::Noise4D noise(1337);
    int differing = 0;
    for (int i = 0; i < 500; ++i) {
        const float t = static_cast<float>(i) * 0.37f;
        const float one  = noise.fbm(t, t * 0.5f, t * 0.25f, t * 0.75f, 1, 0.05f);
        const float four = noise.fbm(t, t * 0.5f, t * 0.25f, t * 0.75f, 4, 0.05f);
        if (one != four) ++differing;
    }
    EXPECT(differing > 490, "adding octaves changes the field");
}

// ----- continuity, which is what makes it terrain and not static ------------

// The largest jump the field makes over a step of `step` along one axis.
// Gradient noise is smooth, so this has to stay small; if it does not, the
// hash is leaking into the output and the "terrain" is white noise.
float worst_step_along(int axis, float step, std::uint32_t seed) {
    const world::Noise4D noise(seed);
    std::mt19937 rng(seed + 77);
    std::uniform_real_distribution<float> pos(-150.0f, 150.0f);
    float worst = 0.0f;
    for (int i = 0; i < 40000; ++i) {
        float p[4] = {pos(rng), pos(rng), pos(rng), pos(rng)};
        const float before = noise.sample(p[0], p[1], p[2], p[3]);
        p[axis] += step;
        const float after = noise.sample(p[0], p[1], p[2], p[3]);
        worst = std::max(worst, std::fabs(after - before));
    }
    return worst;
}

void test_the_field_is_continuous_on_every_axis() {
    // A 0.01 step is a hundredth of a cell. Gradient noise over that
    // distance moves by at most 0.026 (measured); white noise would move
    // by the full range. The 0.05 bound is therefore about 2x clear of
    // the real worst case - enough to separate "smooth" from "not
    // smooth", but not the wide margin the C1 and C2 checks have, and an
    // earlier version of this comment said "a few thousandths" when the
    // measured figure is 26 of them.
    for (int axis = 0; axis < 4; ++axis) {
        const float worst = worst_step_along(axis, 0.01f, 1337);
        EXPECT(worst < 0.05f, "a small step gives a small change");
        if (worst >= 0.05f) {
            std::printf("    (axis %d jumped %.4f)\n", axis, worst);
        }
    }
}

// Mean absolute change over a step of 0.25 along one axis, sampled from
// random 4D positions. The mean rather than the worst case, because the
// worst case is a tail statistic and wanders between runs; the mean is
// stable to three decimal places.
double mean_step_along(int axis, std::uint32_t seed) {
    const world::Noise4D noise(seed);
    std::mt19937 rng(seed + 7);
    std::uniform_real_distribution<float> pos(-200.0f, 200.0f);
    double sum = 0.0;
    constexpr int kN = 60000;
    for (int i = 0; i < kN; ++i) {
        float p[4] = {pos(rng), pos(rng), pos(rng), pos(rng)};
        const float before = noise.sample(p[0], p[1], p[2], p[3]);
        p[axis] += 0.25f;
        sum += std::fabs(noise.sample(p[0], p[1], p[2], p[3]) - before);
    }
    return sum / kN;
}

void test_the_fade_curve_is_the_quintic_one() {
    // One order up from the test above. Perlin's original 1985 fade,
    // 3t^2 - 2t^3, is C1 but not C2: its second derivative is 6 at t=0 and
    // -6 at t=1, so curvature jumps at every cell boundary. The 2002
    // "improved noise" quintic 6t^5 - 15t^4 + 10t^3 has both derivatives
    // vanishing at each end, which is the entire reason to prefer it.
    //
    // That distinction is invisible in the field's value and in its slope.
    // It shows up in shading, because a lit surface samples the derivative
    // of the height field, and a curvature discontinuity on a 1-unit grid
    // is exactly the artifact the quintic exists to remove.
    //
    // Measured over six seeds and 300 boundary crossings per axis: the
    // one-sided second-derivative mismatch is 1.13 to 1.28 with the
    // quintic (that residue is finite-difference error, not a real kink)
    // and 11.3 to 12.1 with the cubic. The bound below sits about 3x clear
    // of each, so it separates the two curves rather than pinning either.
    constexpr float kH = 1e-2f;
    double worst = 0.0;
    for (std::uint32_t seed : {1337u, 7u, 4294967295u}) {
        const world::Noise4D noise(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> off(-5.0f, 5.0f);
        for (int axis = 0; axis < 4; ++axis) {
            for (int trial = 0; trial < 200; ++trial) {
                float p[4] = {off(rng), off(rng), off(rng), off(rng)};
                p[axis] = std::floor(off(rng));  // exactly on a boundary
                auto at = [&](float d) {
                    float q[4] = {p[0], p[1], p[2], p[3]};
                    q[axis] += d;
                    return static_cast<double>(noise.sample(q[0], q[1], q[2], q[3]));
                };
                const double left  = (at(0) - 2 * at(-kH) + at(-2 * kH)) / (kH * kH);
                const double right = (at(2 * kH) - 2 * at(kH) + at(0)) / (kH * kH);
                worst = std::max(worst, std::fabs(left - right));
            }
        }
    }
    EXPECT(worst < 4.0, "curvature does not jump at cell boundaries (C2)");
    if (worst >= 4.0) {
        std::printf("    (worst one-sided 2nd-derivative mismatch: %.4f)\n", worst);
    }
}

void test_the_field_is_isotropic() {
    // Every gradient has the same length and the set is spread evenly, so
    // a correct field moves at the same rate along all four axes. No axis
    // is special, w included.
    //
    // This is the test that catches w being dropped from the gradient dot
    // product - a fault that survives everything else here, because the
    // hash still varies with w and the interpolation still runs along it,
    // so the field still responds to w and still looks like noise. It just
    // degrades to value noise on that one axis, which in a 4D world means
    // the fourth dimension quietly has visible grid structure the other
    // three do not.
    //
    // Measured over eight seeds: the ratio between the fastest and slowest
    // axis is 1.004 to 1.015 on correct noise, and 1.58 to 1.60 with the w
    // gradient component zeroed. A bound of 1.10 sits an order of
    // magnitude clear of both, so it is a separator rather than a tuned
    // constant.
    for (std::uint32_t seed : {1337u, 7u, 4294967295u}) {
        double lo = 1e9, hi = 0.0;
        for (int axis = 0; axis < 4; ++axis) {
            const double s = mean_step_along(axis, seed);
            lo = std::min(lo, s);
            hi = std::max(hi, s);
        }
        EXPECT(lo > 0.0, "every axis moves the field");
        EXPECT(hi / lo < 1.10, "no axis moves the field faster than another");
        if (hi / lo >= 1.10) {
            std::printf("    (seed %u: fastest/slowest axis ratio %.4f)\n",
                        seed, hi / lo);
        }
    }
}

void test_the_field_is_smooth_across_cell_boundaries() {
    // Continuity is not enough. A linear fade curve gives a field that is
    // continuous everywhere and still wrong: the slope jumps at every cell
    // boundary, and once terrain built on it is lit, that shows up as a
    // faint grid of ridges at 1-unit intervals. Perlin's quintic fade
    // exists precisely to kill that, by pinning the first and second
    // derivatives to zero at both ends of a cell.
    //
    // So this compares the one-sided derivatives at a lattice crossing.
    // They agree for a C1 field and disagree for a kinked one. Measured:
    // 0.0004 with the quintic fade, 1.4609 with a linear one, so the
    // bound below separates them by three orders of magnitude and is not
    // a tuned constant.
    const world::Noise4D noise(1337);
    constexpr float kH = 1e-3f;
    float worst_kink = 0.0f;
    for (int axis = 0; axis < 4; ++axis) {
        for (int i = -8; i <= 8; ++i) {
            float p[4] = {0.3f, 0.7f, 0.11f, 0.53f};
            p[axis] = static_cast<float>(i);  // exactly on a cell boundary
            float lo[4], hi[4];
            for (int k = 0; k < 4; ++k) { lo[k] = p[k]; hi[k] = p[k]; }
            lo[axis] -= kH;
            hi[axis] += kH;
            const float here = noise.sample(p[0], p[1], p[2], p[3]);
            const float left  = (here - noise.sample(lo[0], lo[1], lo[2], lo[3])) / kH;
            const float right = (noise.sample(hi[0], hi[1], hi[2], hi[3]) - here) / kH;
            worst_kink = std::max(worst_kink, std::fabs(left - right));
        }
    }
    EXPECT(worst_kink < 0.05f, "the slope does not jump at cell boundaries");
    if (worst_kink >= 0.05f) {
        std::printf("    (worst one-sided derivative mismatch: %.4f)\n", worst_kink);
    }
}

void test_no_axis_is_dead() {
    // Each axis has to carry variance on its own. A gradient table with a
    // zeroed column, or a hash that drops a term, leaves one axis flat -
    // in a 4D world that would mean the fourth dimension exists in the
    // storage and does nothing in the terrain.
    const world::Noise4D noise(1337);
    for (int axis = 0; axis < 4; ++axis) {
        double sum = 0.0, sumsq = 0.0;
        constexpr int kN = 4000;
        for (int i = 0; i < kN; ++i) {
            float p[4] = {0.5f, 0.5f, 0.5f, 0.5f};
            p[axis] = static_cast<float>(i) * 0.05f;
            const float v = noise.sample(p[0], p[1], p[2], p[3]);
            sum += v;
            sumsq += static_cast<double>(v) * v;
        }
        const double mean = sum / kN;
        const double var = sumsq / kN - mean * mean;
        EXPECT(var > 0.005, "sweeping this axis alone varies the field");
        if (var <= 0.005) std::printf("    (axis %d variance %.5f)\n", axis, var);
    }
}

void test_the_field_is_centred() {
    // Gradient noise should be zero-mean. A biased field would push all
    // terrain up or down, which a terrain generator's own constants would
    // then be tuned around, hiding the bug permanently.
    const world::Noise4D noise(1337);
    double sum = 0.0;
    constexpr int kN = 200000;
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> pos(-300.0f, 300.0f);
    for (int i = 0; i < kN; ++i) {
        sum += noise.sample(pos(rng), pos(rng), pos(rng), pos(rng));
    }
    const double mean = sum / kN;
    EXPECT(std::fabs(mean) < 0.01, "the field is zero-mean");
    if (std::fabs(mean) >= 0.01) std::printf("    (mean %.5f)\n", mean);
}

// ----- how far the world actually goes --------------------------------------

void test_the_field_keeps_its_detail_out_to_a_million_units() {
    // Coordinates are floats, and a float has 24 bits of mantissa, so
    // somewhere past 1e7 two nearby world positions round to the same
    // float and the noise returns the same value for both. The terrain
    // does not error when that happens - it goes flat, which is the
    // quietest possible failure for a world that advertises no bounds.
    //
    // Counted with a set, because the first version of this counted
    // `v != prev` - consecutive differences, not distinct values. A field
    // alternating between two values forever would have scored a perfect
    // 2000/2000 under that metric, and it understated the real loss: at
    // 1e7 it reported 998/2000 where the true figure is 595/2000, so the
    // doc said "half the detail gone" when 70% was gone.
    //
    //     |coord|   distinct of 2000
    //       1e2          1973
    //       1e4          1984
    //       1e6          1945
    //       1e7           595   <- collapsing
    //       1e8             1   <- flat
    //
    // Never exactly 2000 even close to the origin: float spacing means a
    // few of 2000 samples collide anywhere. The bound is 95%, which sits
    // an order of magnitude clear of the 30% at 1e7.
    const world::Noise4D noise(1337);
    for (double magnitude : {1e2, 1e4, 1e6}) {
        std::set<float> seen;
        constexpr int kN = 2000;
        for (int i = 0; i < kN; ++i) {
            const float x = static_cast<float>(magnitude)
                          + static_cast<float>(i) * 0.5f;
            seen.insert(noise.sample(x, x * 0.3f, x * 0.7f, x * 0.11f));
        }
        EXPECT(seen.size() > static_cast<std::size_t>(kN) * 95 / 100,
               "the field is still fully resolved here");
        if (seen.size() <= static_cast<std::size_t>(kN) * 95 / 100) {
            std::printf("    (at %.0e only %zu of %d samples are distinct)\n",
                        magnitude, seen.size(), kN);
        }
    }
}

void test_the_field_never_returns_nan_or_infinity() {
    // It degrades at extreme coordinates; it must not explode. A NaN would
    // propagate into a height, through the clamp (NaN compares false
    // against both bounds), and into chunk contents.
    //
    // Two honest caveats. This is close to structural: sample() has no
    // division and no unbounded growth, so with finite float inputs there
    // is little that could produce a NaN, and a review could not construct
    // a plausible mutation that trips it. And "any magnitude" means any
    // magnitude THIS TEST REACHES - the real hard limit is the
    // int32 cast of floor(x) in sample(), which is undefined above
    // 2^31 ~ 2.1e9. That is 2000x past the ~1e6 usable range the
    // resolution test pins, so it is documented rather than guarded: a
    // branch in the hot path to defend a coordinate no world reaches
    // would cost more than it protects.
    const world::Noise4D noise(1337);
    int bad = 0;
    for (double magnitude : {0.0, 1.0, 1e3, 1e6, 1e8, -1e6, -1e8}) {
        for (int i = 0; i < 200; ++i) {
            const float x = static_cast<float>(magnitude)
                          + static_cast<float>(i) * 0.37f;
            const float v = noise.sample(x, x * 0.3f, x * 0.7f, x * 0.11f);
            if (std::isnan(v) || std::isinf(v)) ++bad;
            const float f = noise.fbm(x, x * 0.3f, x * 0.7f, x * 0.11f, 4, 0.02f);
            if (std::isnan(f) || std::isinf(f)) ++bad;
        }
    }
    EXPECT(bad == 0, "no sample is NaN or infinite, at any magnitude or sign");
}

// ----- the 3D engine's assumption ------------------------------------------

void test_a_fixed_w_gives_a_usable_3d_field() {
    // Slicing is how a 4D world reaches a 3D renderer: hold w and the
    // result has to be an ordinary 3D noise field. Two different slices
    // must also be genuinely different worlds, or moving along w would be
    // a no-op with extra memory traffic.
    const world::Noise4D noise(1337);
    int alive = 0, differ = 0;
    constexpr int kN = 2000;
    std::mt19937 rng(13);
    std::uniform_real_distribution<float> pos(-100.0f, 100.0f);
    for (int i = 0; i < kN; ++i) {
        const float x = pos(rng), y = pos(rng), z = pos(rng);
        const float slice_a = noise.sample(x, y, z, 0.5f);
        const float slice_b = noise.sample(x, y, z, 40.5f);
        if (slice_a != 0.0f) ++alive;
        if (slice_a != slice_b) ++differ;
    }
    EXPECT(alive > kN * 9 / 10, "a fixed-w slice is a live 3D field");
    EXPECT(differ > kN * 9 / 10, "distant slices are different worlds");
}

}  // namespace

int main() {
    std::printf("noise4d_tests: running...\n\n");
    test_noise_is_exactly_zero_at_lattice_points();
    test_noise_is_not_zero_between_lattice_points();
    test_sampling_is_pure_and_seed_stable();
    test_the_seed_reaches_the_field();
    test_edge_seeds_behave();
    test_output_stays_in_range();
    test_fbm_keeps_the_single_octave_range();
    test_fbm_octaves_add_detail();
    test_the_field_is_continuous_on_every_axis();
    test_the_fade_curve_is_the_quintic_one();
    test_the_field_is_isotropic();
    test_the_field_is_smooth_across_cell_boundaries();
    test_no_axis_is_dead();
    test_the_field_is_centred();
    test_the_field_keeps_its_detail_out_to_a_million_units();
    test_the_field_never_returns_nan_or_infinity();
    test_a_fixed_w_gives_a_usable_3d_field();

    std::printf("\nnoise4d_tests: %d checks, %d failure%s\n",
                g_checks, g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
