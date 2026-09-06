#pragma once

#include "world/chunk.h"
#include "world/noise4d.h"

#include <cstdint>

namespace world {

// Terrain generation with a fourth spatial axis.
//
// w is a float, not an integer slice index, and that distinction is the
// difference between a fourth dimension and a world selector. An integer
// w makes the fourth axis a menu: you jump between discrete worlds. A
// continuous w makes it an axis you travel along, where holding a key
// slides the terrain the way walking slides it - which is the only
// version that reads as four-dimensional rather than as a level swap.
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
    int height_at(int wx, int wz, float w) const;

    // Fills `out` with the chunk at (chunk_x, chunk_z) on slice w.
    void fill_chunk(int chunk_x, int chunk_z, float w, Chunk& out) const;

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
// One. The fourth axis is scaled exactly like x and z, and getting back
// to that took a mistake and an adversarial review to undo.
//
// This constant was 6, justified by a measurement showing that at scale 1
// a step along w changed 4.6% of columns by at most one block - "the
// world was frozen along the fourth axis". That measurement was real, and
// it was taken against a height amplitude of 28. The amplitude was then
// refitted to 126 (see kHeightAmplitude) and this table was never
// re-measured, so both the header and docs/4d.md carried numbers
// describing constants the generator no longer used.
//
// Re-measured against the shipped generator, one world unit per slice:
//
//     scale   columns changed per step   max column jump
//       1              51.2%                     4
//       3              80.0%                    10
//       6              88.1%                    18
//      12              92.1%                    26
//      25              94.4%                    35
//      60              96.3%                    46
//
// Scale 1 was never frozen once the amplitude was right. It moves half
// the columns by up to four blocks against a ~45-block height range,
// which is exactly the "recognisably the same place, visibly shifted"
// behaviour the scaling was introduced to produce. The scaling was
// compensating for the amplitude, not for anything about the fourth
// dimension - the amplitude is 4.5x larger now, and 4.5 * 1 block is the
// 4-block jump scale 1 produces.
//
// The old comment also claimed the max jump "stays in single digits at
// every scale, so the field is continuous along w regardless". That was
// false in its own table (scale 60 read 11) and is far more false here.
// Continuity along w is real, and it is established by
// test_adjacent_slices_are_related_not_unrelated rather than by a jump
// count that grows with the scale.
inline constexpr float kWScale = 1.0f;

// Height amplitude and offset, fitted to this noise rather than inherited.
// The 3D generator's `kSeaLevel + n * 28 + 14` put every column between
// y=31 and y=46 here - no water anywhere, the whole world inside the
// grass/stone/snow bands - because the composite does not fill [-1, 1].
//
// Refitted when noise4d's normalizer dropped from 0.81 to 0.75: the
// composite's p1..p99 span went 0.38 -> 0.3503, so 115 became 126 to keep
// the same world. That is the coupling worth noticing - the amplitude is
// a function of the noise's scale, so any change to kNormalize has to be
// followed here or the world quietly gets flatter.
//
// 126 spreads p1..p99 across roughly y=12..56, and +8 leaves about a
// fifth of the world under sea level.
inline constexpr float kHeightAmplitude = 126.0f;
inline constexpr float kHeightOffset    = 8.0f;

}  // namespace world
