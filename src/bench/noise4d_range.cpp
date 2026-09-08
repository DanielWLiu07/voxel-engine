// Measures the raw output range of Noise4D, which is where the
// normalization constant in noise4d.cpp comes from.
//
// This exists as a committed program rather than as a number in a comment
// because of a rule this repo learned the hard way: never quote a figure
// whose input is not in the repo. A normalization constant that someone
// once measured on their machine and typed into a comment is exactly that
// kind of figure - it works for the author and nobody can check it.
//
//     cmake --build build --target noise4d_range && ./build/noise4d_range
//
// Eight seeds, including the two edge cases (0 and UINT32_MAX) that a hash
// mixing the seed by multiplication is most likely to handle badly.

#include "world/noise4d.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>

int main() {
    // 200 seeds and 200M samples, not the 8 and 20M this started with.
    // The narrower sweep found a raw peak of 1.2244; this one finds
    // 1.2572, and the constant fitted to the first was not a bound.
    // Two seed families, because they find different maxima and the
    // committed tool has to report the worse one. A scratch sweep using
    // si*K+1 found a raw peak of 1.2572 while this tool's si*K family
    // found 1.2109, and a comment in noise4d.cpp quoted the 1.2572 -
    // a figure the committed program did not print. That is the rule this
    // repo has about never quoting a figure whose input is not in the
    // repo, broken inside the fix for an instance of it. Both families
    // are swept now.
    constexpr int kPerSeed = 500'000;
    constexpr int kSeedCount = 400;

    float peak = 0.0f;
    double sum = 0.0, sumsq = 0.0;
    long n = 0;

    for (int si = 0; si < kSeedCount; ++si) {
        // Spread over the whole 32-bit space rather than clustered low,
        // and including the two edges a multiply-mixed seed handles worst.
        const std::uint32_t seed =
            (si == 0) ? 0u
          : (si == 1) ? 4294967295u
          : (si % 2 == 0) ? static_cast<std::uint32_t>(si / 2) * 2654435761u
                          : static_cast<std::uint32_t>(si / 2) * 2654435761u + 1u;
        const world::Noise4D noise(seed);
        std::mt19937 rng(seed);
        std::uniform_real_distribution<float> pos(-500.0f, 500.0f);
        float seed_peak = 0.0f;
        for (int i = 0; i < kPerSeed; ++i) {
            const float v = noise.sample(pos(rng), pos(rng), pos(rng), pos(rng));
            seed_peak = std::max(seed_peak, std::fabs(v));
            sum += v;
            sumsq += static_cast<double>(v) * v;
            ++n;
        }
        if (seed_peak >= peak) {
            std::printf("  new max at seed %-10u peak|v|=%.5f\n", seed, seed_peak);
        }
        peak = std::max(peak, seed_peak);
    }

    const double mean = sum / static_cast<double>(n);
    const double stddev = std::sqrt(sumsq / static_cast<double>(n) - mean * mean);
    // Sampling on a lattice-aligned grid would land on the exact zeros and
    // flatter the mean, so the sweep above is at random positions.
    std::printf("\nNOISE4D_RANGE samples=%ld peak=%.4f mean=%.5f stddev=%.4f\n",
                n, peak, mean, stddev);
    // sample() already applies the normalizer, so `peak` above is the
    // normalized figure and the raw one is recovered by dividing. Printing
    // both, because the constant in noise4d.cpp is derived from the raw
    // peak and the bound the unit test enforces is the normalized one.
    constexpr double kNormalizeInUse = 0.75;
    std::printf("normalized peak %.4f (must stay <= 1.0), "
                "implying a raw peak of %.4f\n",
                peak, peak / kNormalizeInUse);
    return 0;
}
