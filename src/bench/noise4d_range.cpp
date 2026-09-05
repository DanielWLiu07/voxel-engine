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
    constexpr int kPerSeed = 2'500'000;
    constexpr std::uint32_t kSeeds[] = {1337u, 7u, 99u, 2024u,
                                        55555u, 1u, 0u, 4294967295u};

    float peak = 0.0f;
    double sum = 0.0, sumsq = 0.0;
    long n = 0;

    for (std::uint32_t seed : kSeeds) {
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
        std::printf("  seed %-10u peak|v|=%.4f\n", seed, seed_peak);
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
    constexpr double kNormalizeInUse = 0.81;
    std::printf("normalized peak %.4f (must stay <= 1.0), "
                "implying a raw peak of %.4f\n",
                peak, peak / kNormalizeInUse);
    return 0;
}
