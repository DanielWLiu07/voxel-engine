#pragma once

#include "world/chunk.h"
#include "world/noise4d.h"

#include <cmath>    // to_4d is inline and calls std::cos/std::sin
#include <cstdint>

namespace world {

// How much noise-space one unit of movement along w covers.
//
// Six. This constant has now been 6, then 1, then 6 again, and the round
// trip is worth recording because both changes were right about what they
// measured and the first two were measuring the wrong thing.
//
// It was 6, justified by a sweep showing that at scale 1 a step along w
// changed 4.6% of columns by at most one block. That sweep was taken
// against a height amplitude of 28, and when the amplitude was refitted
// to 126 nobody re-measured, so the justification described a generator
// that no longer existed. Re-measured, scale 1 changes 51.2% of columns
// per world unit of w - not frozen at all - so it went to 1.
//
// That was correct about the FIELD and wrong about the ENGINE, because
// the field scale and the player's travel speed multiply, and only one
// of them is free.
//
// A rebuild re-requests every chunk in the window, so how often the world
// is rebuilt is set by travel speed alone: at kSliceRemeshStep the player
// crosses a rebuild every step/speed seconds, and the worker pool caps
// that at roughly three per second. Travel speed is therefore expensive
// and bounded at about 0.4 units/sec. kWScale costs nothing - it changes
// how DIFFERENT each rebuild looks, not how many there are.
//
// So the visual rate has to be bought with kWScale, and at scale 1 with a
// walk of 0.4 units/sec the result was 19% of columns moving by one block
// per second: technically continuous, visually nothing. Measured across
// the scale, per rebuild and per second of walking:
//
//     scale   per rebuild (0.12w)   per second walking (0.4w)
//       1      5.3%,  max jump 1     19.4%,  max jump 1
//       2     11.9%,  max jump 1     38.8%,  max jump 2
//       4     23.7%,  max jump 1     62.0%,  max jump 3
//       6     35.3%,  max jump 2     73.9%,  max jump 5
//      10     52.6%,  max jump 3     83.5%,  max jump 7
//
// 6 is where a second of travel visibly reshapes the landscape (74% of
// columns, five blocks at the extreme) while a single rebuild still moves
// nothing by more than two blocks, so the terrain flows rather than
// stepping. 10 makes each rebuild a visible jolt; 1 and 2 are invisible
// while walking.
//
// The continuity of the field along w does not depend on this at all -
// that is established by test_adjacent_slices_are_related_not_unrelated,
// and it holds at every scale in the table.
inline constexpr float kWScale = 6.0f;

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

    // The 3D hyperplane the player currently occupies, as an offset along
    // w and a rotation of the slice within 4D space.
    //
    // Translation alone is not a fourth dimension in the sense a player
    // experiences one. It keeps the cut axis-aligned, so every slice is a
    // different but perfectly ordinary 3D world and every block is a whole
    // cube - the world swaps rather than reveals. Rotating the cut is what
    // 4D Miner does with the scroll wheel, and it is what makes structures
    // appear to change shape: a hyperplane at an angle to the 4D lattice
    // meets a 4D block in a cross-section that is not the block.
    //
    // `theta` rotates in the (z, w) plane. At 0 the slice is the familiar
    // w = constant hyperplane and z is depth. At pi/2 the slice's own z
    // axis IS the fourth dimension: walking forward walks through w, and
    // what was depth is gone. Everything between is a genuine mixture.
    struct Slice {
        float w = 0.0f;
        float theta = 0.0f;
        // Where the slice's own z axis starts, relative to world z.
        //
        // Needed only because the cut turns about the PLAYER. w and theta
        // describe a line in the (z, w) plane, and that line is
        // parametrised from the foot of its perpendicular to the 4D
        // origin - a point that MOVES when the line turns. Without a
        // third number the player's own position slides along the
        // parametrisation every time the wheel moves, which made rotation
        // irreversible: a notch out and a notch back returned the offset
        // to o*cos^2(delta) instead of to o, so scrolling back and forth
        // walked the player along the fourth axis without them touching a
        // travel key.
        //
        // Zero for every axis-aligned slice and for the whole 3D engine,
        // so it costs nothing where it is not needed.
        float z_shift = 0.0f;
    };

    // Surface height for a world column at (wx, wz) on the given slice.
    int height_at(int wx, int wz, Slice s) const;

    // Fills `out` with the chunk at (chunk_x, chunk_z) on the given slice.
    void fill_chunk(int chunk_x, int chunk_z, Slice s, Chunk& out) const;

    // Where a point of the slice lands in the noise's 4D space. The whole
    // rotation lives here so nothing else has to know the convention, and
    // the returned coordinates are ready to sample - kWScale is already
    // in them.
    //
    // The scale is applied to w BEFORE the rotation, and that ordering is
    // the difference between rotating the world and shearing it.
    //
    // It used to be applied afterwards, by each caller, which composed to
    // the map [[c, -s], [6s, 6c]]: determinant 6, and singular values 1
    // and 6 rather than 1 and 1. to_4d was a proper rotation, but the
    // space it rotated was six times finer along w than along z, so a
    // tilt progressively compressed the world along z. Measured as the
    // ratio of mean |dh| along z to along x: 1.00 at theta=0, 1.73 at
    // 0.25, and 4.79 at pi/2, where the terrain visibly corrugates into
    // ridges running across z. It also broke the no-cliffs invariant the
    // test suite asserts - adjacent columns differ by at most 8 blocks at
    // theta=0, and by 12 at theta=1.5.
    //
    // Scaling first makes the composed map [[c, -s], [s, c]] applied to
    // (sz, kWScale * w): a true rotation of an isotropic space. theta=0
    // is untouched - it reduces to z4 = sz, w4 = kWScale * w, exactly
    // what every published 3D and untilted 4D figure was measured with.
    static void to_4d(float sz, Slice s, float* out_z4, float* out_w4) {
        const float w = s.w * kWScale;
        const float d = sz + s.z_shift;
        const float c = std::cos(s.theta), sn = std::sin(s.theta);
        *out_z4 = d * c - w * sn;
        *out_w4 = d * sn + w * c;
    }

    void set_caves_enabled(bool e) { caves_enabled_ = e; }
    bool caves_enabled() const { return caves_enabled_; }

private:
    Noise4D continents_, hills_, detail_, warp_;
    // biome_ drives tree density, and it is 4D like everything else - so
    // forests thicken and thin as you travel along w rather than being
    // painted on a static map.
    Noise4D biome_, temp_;
    Noise4D cave_a_, cave_b_;
    bool caves_enabled_ = true;
};


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
