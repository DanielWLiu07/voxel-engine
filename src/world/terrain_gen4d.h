#pragma once

#include "world/chunk.h"
#include "world/noise4d.h"

#include <array>
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
        // The second rotation plane, and the second shift that pivots it.
        //
        // 4D Miner turns the cut in TWO planes: the wheel (and vertical
        // mouse) turns it in ZW, horizontal mouse turns it in XW. One
        // plane alone leaves a whole axis of 4D orientation unreachable -
        // you can tilt the world away from you but never sideways.
        float phi = 0.0f;
        float x_shift = 0.0f;
    };

    // Surface height for a world column at (wx, wz) on the given slice.
    int height_at(int wx, int wz, Slice s) const;

    // The same, addressed by where the column already sits in 4D rather
    // than by a slice point that has to be carried there. height_at is
    // this composed with to_4d.
    int height_at_4d(float x4, float z4, float w4) const;

    // Everything one column of the world holds, plus the four facts the
    // chunk-level tree pass needs about it.
    //
    // A column is the natural unit here and not an implementation
    // convenience: y is the one axis a slice rotation leaves alone, so a
    // point of the 4D lattice picks out a full 256-tall stack of blocks
    // however the cut is turned. That is what lets the same generator
    // serve a chunk of voxels and a tiling of 4D cells - only the
    // question "which columns" differs between them.
    struct Column4D {
        std::array<std::uint8_t, kChunkSizeY> blocks{};
        // The guide height. Not the surface: the density band can put
        // ground above it or carve it away.
        int   guide_height = 0;
        // The topmost solid block the band produced.
        int   top          = 0;
        bool  is_desert    = false;
        float biome        = 0.0f;
    };

    // Generates the column standing at 4D point (x4, z4, w4). Terrain,
    // block types and caves; trees are not here because a tree spills
    // into its neighbours and so belongs to whatever owns the set of
    // columns.
    void fill_column(float x4, float z4, float w4, Column4D& out) const;

    // The column of the 4D lattice cell (i, k, l).
    //
    // Two decisions here, and both are load-bearing.
    //
    // It takes no Slice. That is what makes a block's material
    // independent of how the world is being looked at: a cell keeps its
    // contents as the cut turns through it, and only the shape it
    // presents changes. Sample a point of the slice instead and the same
    // block quietly becomes a different one as the player scrolls, which
    // is a world being re-rolled rather than a world being cut.
    //
    // It samples the cell's LOW CORNER, not its centre, and that is not
    // arbitrary either. The cube path samples the voxel column at integer
    // (wx, wz) with w4 = w * kWScale, so corner sampling is what makes an
    // untilted 4D cut reproduce the cube world EXACTLY - same seed, same
    // terrain, block for block. Half a block of offset would give a world
    // that looks right and matches nothing, and every comparison between
    // the two paths would be measuring two different worlds.
    void fill_cell_column(int i, int k, int l, Column4D& out) const {
        fill_column(static_cast<float>(i), static_cast<float>(k),
                    static_cast<float>(l), out);
    }

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
    // Where a point of the slice lands in the noise's 4D space.
    //
    // Two rotations, composed: XW first, then ZW. The slice's x axis
    // leans into w by phi, and what comes out of that leans into w again
    // by theta along z. At phi = 0 this reduces exactly to the ZW-only
    // form every earlier figure was measured with, and at theta = phi = 0
    // to the axis-aligned hyperplane the whole thing started as.
    static void to_4d(float sx, float sz, Slice s,
                      float* out_x4, float* out_z4, float* out_w4) {
        const float w = s.w * kWScale;
        const float dx = sx + s.x_shift;
        const float dz = sz + s.z_shift;
        const float cp = std::cos(s.phi), sp = std::sin(s.phi);
        // XW: x leans into w.
        const float x4 = dx * cp - w * sp;
        const float w1 = dx * sp + w * cp;
        // ZW: z leans into whatever w has become.
        const float ct = std::cos(s.theta), st = std::sin(s.theta);
        *out_x4 = x4;
        *out_z4 = dz * ct - w1 * st;
        *out_w4 = dz * st + w1 * ct;
    }

    // Structures: rock formations that are genuinely four-dimensional.
    //
    // A boulder here is a 4-BALL, not a sphere, so the slice cuts a
    // sphere out of it whose radius is sqrt(r^2 - d^2) in the fourth
    // axis: travel along w and a boulder swells, peaks and vanishes. A
    // monolith is a 4-box, so rotating the cut turns its footprint from a
    // rectangle into a hexagon exactly as a single block's does, only
    // eight times the size and impossible to miss.
    //
    // They are defined in 4D and evaluated inside fill_column, which is
    // what makes them work on BOTH paths for free - the cube mesher and
    // the cross-section mesher ask the same generator the same question,
    // and neither has to know structures exist.
    void set_structures_enabled(bool e) { structures_enabled_ = e; }
    bool structures_enabled() const { return structures_enabled_; }

    void set_caves_enabled(bool e) { caves_enabled_ = e; }
    bool caves_enabled() const { return caves_enabled_; }

private:
    // One formation, resolved from its grid site. Shared by the stamping
    // pass and the footprint predicate so the two cannot describe
    // different worlds - the placement rule lives in exactly one place.
    struct StructureSite {
        float cx, cz, cw;      // centre in 4D
        bool  is_ball;
        float r;               // ball radius
        float hx, hz, hw;      // box half-extents
        int   base;            // terrain height under the centre, -1 if unused
    };

    // Visits every grid site whose formation could reach this 4D column.
    // The callback is only invoked for sites that exist and pass the
    // shoreline rule.
    template <typename Fn>
    void for_each_site(float x4, float z4, float w4, Fn&& fn) const;

    Noise4D continents_, hills_, detail_, warp_;
    // biome_ drives tree density, and it is 4D like everything else - so
    // forests thicken and thin as you travel along w rather than being
    // painted on a static map.
    Noise4D biome_, temp_;
    Noise4D cave_a_, cave_b_;
    // The density field: what makes this a world SLICED from four
    // dimensions rather than one PARAMETERISED by a fourth.
    //
    // height_at still gives the landscape its shape, but it is a GUIDE
    // now, not the surface. Solidity near it is decided per voxel by this
    // field, so a column can enter and leave the ground more than once -
    // which is where arches, roofs over air and floating ground come
    // from, and which a heightfield cannot produce at any tilt.
    Noise4D density_;
    bool caves_enabled_ = true;
    bool structures_enabled_ = true;
    std::uint32_t seed_ = 1337;

    // Writes any structure covering this column into `out`. Called from
    // fill_column with the column's own 4D address.
    void stamp_structures(float x4, float z4, float w4, int column_height,
                          Column4D& out) const;

public:
    // Whether any structure's footprint covers this 4D column, ignoring
    // terrain entirely.
    //
    // A testability seam, and it earns its place: the natural way to
    // observe a structure is to difference the world against one
    // generated without them, and that measurement is contaminated by
    // terrain - a block written where stone already stands shows no
    // difference, and the terrain under a fixed footprint varies with w
    // on its own. Four successive versions of the boulder test were
    // fooled by exactly that, each passing with an extruded-sphere fault
    // injected.
    //
    // This answers the geometric question directly: is (x4, z4, w4)
    // inside a formation. A 4-ball's footprint shrinks as the slice moves
    // off its centre; an extruded sphere's does not. Nothing about the
    // landscape can blur that.
    bool structure_footprint(float x4, float z4, float w4) const;

private:
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
