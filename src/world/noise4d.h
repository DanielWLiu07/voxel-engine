#pragma once

#include <cstdint>

namespace world {

// 4D gradient (Perlin) noise.
//
// This exists because FastNoiseLite, which every 3D field in terrain_gen
// uses, stops at three dimensions - it exposes GetNoise(x,y) and
// GetNoise(x,y,z) and nothing else. A world with a fourth spatial axis
// needs a fourth noise argument, so this is the one piece of the terrain
// generator that cannot be reused and has to be written.
//
// Perlin rather than simplex, deliberately. The existing height fields are
// already Perlin, so the character of the terrain carries over; 4D simplex
// needs coordinate ranking across five simplex vertices and is markedly
// harder to convince yourself is correct. Perlin in 4D is the same
// algorithm as in 3D with 16 hypercube corners instead of 8, which is
// tedious but verifiable - and it has an exact property worth testing
// against (see kLatticeZero below).
class Noise4D {
public:
    explicit Noise4D(std::uint32_t seed = 1337) : seed_(seed) {}

    // One octave, in roughly [-1, 1]. See kNormalize in the .cpp for why
    // "roughly" is the honest word and what the measured bound actually is.
    float sample(float x, float y, float z, float w) const;

    // Fractal Brownian motion: `octaves` samples at doubling frequency and
    // halving amplitude, normalized by the amplitude sum so the result
    // keeps the single-octave range. Matches the FractalType_FBm setup the
    // 3D fields use, so a 4D field and its 3D counterpart are comparable.
    float fbm(float x, float y, float z, float w, int octaves,
              float frequency = 1.0f, float lacunarity = 2.0f,
              float gain = 0.5f) const;

    std::uint32_t seed() const { return seed_; }

private:
    std::uint32_t seed_;
};

// Gradient noise is exactly zero at every integer lattice point: the
// distance vector to each of the 16 corners has a zero component along the
// axis it shares with the sample, and at a lattice point every distance
// vector is the zero vector, so every corner dot product is zero.
//
// That is an exact identity rather than an approximation, which makes it
// the one property here a test can assert on the nose. A hash that
// collides, a gradient table with a zero row, or a fade curve that does
// not pin its endpoints all break it.
inline constexpr float kLatticeZero = 0.0f;

}  // namespace world
