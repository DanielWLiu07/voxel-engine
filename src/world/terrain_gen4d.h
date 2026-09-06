#pragma once

#include "world/chunk.h"
#include "world/noise4d.h"

#include <cstdint>

namespace world {

// Terrain generation with a fourth spatial axis.
//
// The 3D generator's structure carries over exactly - domain-warped fBm
// for height, two intersecting iso-surfaces for caves, altitude bands for
// surface material - with w threaded through every noise sample. What does
// not carry over is the constants, which is the finding docs/4d.md
// records: this noise normalizes by its amplitude sum and FastNoiseLite's
// does not, so every amplitude had to be refitted to a measured
// distribution rather than copied.
//
// A 4D world reaches a 3D renderer by slicing: fill_chunk takes a w and
// produces an ordinary Chunk, which the existing mesher, culler and
// renderer handle without knowing a fourth dimension exists. That is what
// keeps the change to storage and generation rather than to everything.
class TerrainGen4D {
public:
    explicit TerrainGen4D(std::uint32_t seed = 1337);

    // Surface height for a world column at (wx, wz) on slice w.
    int height_at(int wx, int wz, int w) const;

    // Fills `out` with the chunk at (chunk_x, chunk_z) on slice w.
    void fill_chunk(int chunk_x, int chunk_z, int w, Chunk& out) const;

    void set_caves_enabled(bool e) { caves_enabled_ = e; }
    bool caves_enabled() const { return caves_enabled_; }

private:
    Noise4D continents_, hills_, detail_, warp_;
    // No biome field and no trees yet: the 3D generator uses biome noise
    // only to pick tree density and variant, and trees are a stamp pass
    // that has nothing 4D about it. Left out rather than carried along
    // unused, so nothing here is dead weight pretending to be a feature.
    Noise4D temp_;
    Noise4D cave_a_, cave_b_;
    bool caves_enabled_ = true;
};

// How much noise-space one unit of movement along w covers.
//
// Not 1, and the reason is measured rather than assumed. With w treated
// like x and z, a step along it changed 4.6% of columns by at most one
// block: the world was frozen along the fourth axis. That is not a bug -
// the continent field's noise cell is 250 units wide, and a player who can
// walk 250 units along x cannot walk 250 units along w, because there is
// nothing there to walk through.
//
// src/bench/slice4d.cpp swept this. At scale 1, 5, 25 and 60 the share of
// columns that change per step is 4.6%, 51.2%, 77.3% and 85.0%, while the
// largest single-column jump stays in single digits throughout - so the
// field is continuous along w at every scale and this is a choice about
// feel, not about correctness.
inline constexpr float kWScale = 6.0f;

// Height amplitude and offset, fitted to this noise rather than inherited.
// The 3D generator's `kSeaLevel + n * 28 + 14` put every column between
// y=31 and y=46 here - no water anywhere, the whole world inside the
// grass/stone/snow bands - because the composite measures p1..p99 of
// -0.17..0.21 instead of filling [-1, 1]. 115 spreads that across roughly
// y=12..56, and +8 leaves about a fifth of the world under sea level.
inline constexpr float kHeightAmplitude = 115.0f;
inline constexpr float kHeightOffset    = 8.0f;

}  // namespace world
