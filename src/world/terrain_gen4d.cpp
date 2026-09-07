#include "world/terrain_gen4d.h"

#include "world/terrain_gen.h"
#include "world/tree_stamps.h"

#include <algorithm>
#include <cmath>

namespace world {

namespace {

// The 3D generator's frequencies, unchanged. The frequencies are about
// feature size in world units and that does not depend on how many axes
// there are, which is why these port and the amplitudes did not.
constexpr float kContinentFreq = 0.004f;
constexpr float kHillsFreq     = 0.020f;
constexpr float kDetailFreq    = 0.080f;
constexpr float kWarpFreq      = 0.012f;
constexpr float kBiomeFreq     = 0.008f;
constexpr float kTempFreq      = 0.006f;
constexpr float kCaveFreq      = 0.038f;

// Surface density. Within kDensityBand blocks of the guide height, a
// voxel is solid when the field beats a ramp running from solid at the
// bottom of the band to air at the top.
//
// The y term is deliberately the fastest axis. An earlier attempt used a
// single octave whose y period was wider than the band, so the field
// crossed the ramp exactly once per column and the result was still a
// heightfield - it produced no overhangs at all and cost 1.6x for the
// privilege. Several crossings need the field to vary faster in y than
// the ramp does, which is what kDensityYSquash and the octave count buy.
constexpr int   kDensityBand     = 20;
constexpr float kDensityFreq     = 0.030f;
constexpr float kDensityYSquash  = 5.5f;
constexpr int   kDensityOctaves  = 4;
// How hard the field can argue with the ramp. At 1.0 the band detaches
// into floating gravel; below about 0.7 it cannot cross twice.
constexpr float kDensityStrength = 1.10f;

}  // namespace

TerrainGen4D::TerrainGen4D(std::uint32_t seed)
    : continents_(seed), hills_(seed + 1), detail_(seed + 2), warp_(seed + 3),
      biome_(seed + 4), temp_(seed + 5),
      cave_a_(seed + 6), cave_b_(seed + 7), density_(seed + 8) {
    seed_ = seed;
}

// Structures, and the reason they live in fill_column rather than in a
// chunk pass like the trees.
//
// A tree is a 3D object: it spreads sideways across cells at one w, so it
// needs a set of columns to stamp into and belongs to whatever owns that
// set. A structure here is a 4D object - a ball or a box with an extent
// along w - so every column can decide on its own whether it is inside
// one, from its own 4D address and nothing else. That is what makes it
// work identically for the cube mesher and the cross-section mesher
// without either of them knowing structures exist.
//
// Sites sit on a coarse 4D grid. A column only ever examines the eight
// grid corners around it, rejects almost all of them on a distance test
// that costs three subtractions, and evaluates terrain height only for a
// site it is actually inside - so the whole pass is free on the ~99% of
// columns that are nowhere near one.
template <typename Fn>
void TerrainGen4D::for_each_site(float x4, float z4, float w4, Fn&& fn) const {
    // 18 units between sites, 45% of them occupied.
    //
    // Tuned by measuring, because reasoning about it went wrong twice.
    // The first attempt used 40 and 30% and thought of it as a 3D grid,
    // which is off by an axis: sites are spaced in x4, z4 AND w4, and a
    // slice only sees the ones whose w falls inside a structure's extent.
    // That measured one formation per roughly 150x150 blocks - invisible.
    // The second derived the in-slice density as
    //
    //     (1 / grid^2) * exists * (2 * extent / grid)
    //
    // and predicted one per 55x55 at 24 units; measured, it was one per
    // ~120x120, because the shoreline rule rejects a large share of sites
    // and the formula does not know about it.
    //
    // So: measured. Structure blocks cover 0.74% of the ground at w = 0
    // and 1.4% a few slabs along - a formation every 60-70 blocks of
    // walking, enough that a walk passes several and not so many that the
    // world reads as rubble.
    constexpr float kGrid      = 18.0f;
    constexpr float kExists    = 0.45f;
    constexpr int   kMaxExtent = 7;

    const int gx = static_cast<int>(std::floor(x4 / kGrid));
    const int gz = static_cast<int>(std::floor(z4 / kGrid));
    const int gw = static_cast<int>(std::floor(w4 / kGrid));

    for (int dx = 0; dx <= 1; ++dx)
    for (int dz = 0; dz <= 1; ++dz)
    for (int dw = 0; dw <= 1; ++dw) {
        const int sx = gx + dx, sz = gz + dz, sw = gw + dw;
        if (hash4d_f(sx, sz, sw, 0x5C0FFEE1u ^ seed_) > kExists) continue;

        StructureSite s{};
        // Jitter off the grid, or every formation sits on a lattice and
        // the world looks surveyed.
        s.cx = (static_cast<float>(sx) +
                hash4d_f(sx, sz, sw, 0x11u ^ seed_)) * kGrid;
        s.cz = (static_cast<float>(sz) +
                hash4d_f(sx, sz, sw, 0x22u ^ seed_)) * kGrid;
        s.cw = (static_cast<float>(sw) +
                hash4d_f(sx, sz, sw, 0x33u ^ seed_)) * kGrid;

        if (std::fabs(x4 - s.cx) > kMaxExtent ||
            std::fabs(z4 - s.cz) > kMaxExtent ||
            std::fabs(w4 - s.cw) > kMaxExtent) continue;

        // Only now is a terrain sample worth paying for.
        s.base = height_at_4d(s.cx, s.cz, s.cw);
        // Nothing rooted in the surf: a boulder standing in the water
        // looks wrong even where it breaks no invariant.
        if (s.base <= kSeaLevel + kSandBand + 1) continue;

        const float pick = hash4d_f(sx, sz, sw, 0x44u ^ seed_);
        s.is_ball = (pick < 0.62f);
        if (s.is_ball) {
            s.r = 3.0f + hash4d_f(sx, sz, sw, 0x55u ^ seed_) * 3.0f;
        } else {
            s.hx = 2.0f + hash4d_f(sx, sz, sw, 0x66u ^ seed_) * 2.5f;
            s.hz = 2.0f + hash4d_f(sx, sz, sw, 0x77u ^ seed_) * 2.5f;
            s.hw = 2.0f + hash4d_f(sx, sz, sw, 0x88u ^ seed_) * 2.5f;
            s.r  = 7.0f + hash4d_f(sx, sz, sw, 0x99u ^ seed_) * 8.0f;  // height
        }
        fn(s);
    }
}

bool TerrainGen4D::structure_footprint(float x4, float z4, float w4) const {
    if (!structures_enabled_) return false;
    bool inside = false;
    for_each_site(x4, z4, w4, [&](const StructureSite& s) {
        if (inside) return;
        const float ddx = x4 - s.cx, ddz = z4 - s.cz, ddw = w4 - s.cw;
        if (s.is_ball) {
            if (ddx * ddx + ddz * ddz + ddw * ddw < s.r * s.r) inside = true;
        } else {
            if (std::fabs(ddx) < s.hx && std::fabs(ddz) < s.hz &&
                std::fabs(ddw) < s.hw) inside = true;
        }
    });
    return inside;
}

void TerrainGen4D::stamp_structures(float x4, float z4, float w4,
                                    int column_height, Column4D& out) const {
    if (!structures_enabled_) return;
    // Nothing on the shoreline, tested on THIS column rather than on the
    // structure's centre.
    //
    // The centre test came first and was not enough: a formation is up to
    // seven blocks across, so a site on high ground can still overhang a
    // column whose own surface is at the waterline. The generator
    // guarantees every waterline column is sand-topped, and a stone
    // overhang breaks it - which it did, twice, the second time only
    // after the density went up enough to make the overlap likely.
    if (column_height <= kSeaLevel + kSandBand + 1) return;

    for_each_site(x4, z4, w4, [&](const StructureSite& s) {
        const float ddx = x4 - s.cx, ddz = z4 - s.cz, ddw = w4 - s.cw;
        if (s.is_ball) {
            // A 4-BALL. Its intersection with the slice is a sphere of
            // radius sqrt(r^2 - ddw^2), so travelling along w makes a
            // boulder swell, peak and vanish - the clearest demonstration
            // in the world that the fourth axis is real, and it costs one
            // extra term in a distance check.
            const float rr = s.r * s.r;
            const float flat = ddx * ddx + ddz * ddz + ddw * ddw;
            if (flat >= rr) return;
            const float cy = static_cast<float>(s.base) + s.r * 0.45f;
            const int y_lo = std::max(1, static_cast<int>(cy - s.r));
            const int y_hi = std::min(kChunkSizeY - 1, static_cast<int>(cy + s.r));
            for (int y = y_lo; y <= y_hi; ++y) {
                const float ddy = static_cast<float>(y) - cy;
                if (flat + ddy * ddy < rr) {
                    out.blocks[static_cast<std::size_t>(y)] =
                        static_cast<std::uint8_t>(BlockId::Stone);
                }
            }
        } else {
            // A 4-BOX. Rotating the cut turns its footprint from a
            // rectangle into a hexagon exactly as one block's does, at
            // eight times the size - the same geometry the hyperslice
            // kernel proves, standing in the world where it can be walked
            // around.
            if (std::fabs(ddx) >= s.hx || std::fabs(ddz) >= s.hz ||
                std::fabs(ddw) >= s.hw) return;
            const int y_lo = std::max(1, s.base - 2);
            const int y_hi = std::min(kChunkSizeY - 1,
                                      s.base + static_cast<int>(s.r));
            for (int y = y_lo; y <= y_hi; ++y) {
                out.blocks[static_cast<std::size_t>(y)] =
                    static_cast<std::uint8_t>(BlockId::Stone);
            }
        }
    });
}

int TerrainGen4D::height_at(int wx, int wz, Slice s) const {
    // Both slice axes map into the 4D axes once the cut is rotated: z
    // into (z, w) by theta, x into (x, w) by phi. At theta = phi = 0 this
    // reduces to x4 = wx, z4 = wz, w4 = w - the axis-aligned slice
    // everything started as.
    //
    // to_4d returns coordinates ready to sample: kWScale is inside it,
    // because scaling after the rotation shears rather than rotates.
    float x = 0.0f, z = 0.0f, fw = 0.0f;
    to_4d(static_cast<float>(wx), static_cast<float>(wz), s, &x, &z, &fw);
    return height_at_4d(x, z, fw);
}

int TerrainGen4D::height_at_4d(float x, float z, float fw) const {
    // Domain warp, then three octave stacks over a 3D slice of the 4D
    // field: world x, world z and w, with the noise's remaining axis
    // pinned at 0.
    //
    // Which axis is pinned is worth stating precisely, because the
    // argument order hides it: world z goes into the noise's SECOND
    // argument (its y) and the constant 0 into its third (its z). So it
    // is the noise's z that is held fixed, not its y - an earlier comment
    // here said the opposite. Nothing depends on which axis carries what,
    // since the field is isotropic, but a reader tracing the heightfield
    // should not have to discover that the labels are shuffled.
    const float ox = warp_.sample(x * kWarpFreq, z * kWarpFreq, 0.0f,
                                  fw * kWarpFreq) * 60.0f;
    const float oz = warp_.sample((x + 113.0f) * kWarpFreq,
                                  (z + 271.0f) * kWarpFreq, 0.0f,
                                  fw * kWarpFreq) * 60.0f;
    const float c = continents_.fbm(x + ox, z + oz, 0.0f, fw, 4, kContinentFreq);
    const float h = hills_.fbm(x, z, 0.0f, fw, 4, kHillsFreq);
    const float d = detail_.fbm(x, z, 0.0f, fw, 2, kDetailFreq);
    const float n = c * 0.65f + h * 0.25f + d * 0.10f;

    return std::clamp(
        static_cast<int>(static_cast<float>(kSeaLevel)
                         + n * kHeightAmplitude + kHeightOffset),
        1, kChunkSizeY - 1);
}

// One column of the world, everything except its trees.
//
// This is fill_chunk's inner loop, lifted out unchanged and addressed by
// a 4D point instead of by a slice coordinate that then gets carried to
// one. Two callers want it: fill_chunk, which walks a 16x16 grid of slice
// columns, and the prism mesher, which walks the lattice cells a tilted
// cut passes through. Neither is a special case of the other, and both
// have to produce the same world or the thing the player collides with
// stops matching the thing they can see.
void TerrainGen4D::fill_column(float x4, float z4, float fw,
                               Column4D& out) const {
    out.blocks.fill(static_cast<std::uint8_t>(BlockId::Air));

    const int height = height_at_4d(x4, z4, fw);
    out.guide_height = height;

    const float temp = temp_.sample(x4 * kTempFreq, z4 * kTempFreq,
                                    0.0f, fw * kTempFreq);
    // 0.24, not the 3D generator's 0.35.
    //
    // Same class of mistake as the height amplitude, in a place nobody
    // thought to look: a threshold copied across a change of noise.
    // FastNoiseLite's Perlin at this frequency has stddev 0.310; this
    // noise has 0.215. The same 0.35 therefore fires on 5.22% of columns
    // here against 14.05% there, making deserts about 2.7x rarer - a
    // quiet biome change, not a bug anything would report.
    //
    // Matched by QUANTILE, not by scaling the threshold by the ratio of
    // standard deviations. That first attempt gave 0.24, which fires on
    // 10.3% against the 3D generator's 13.25% - Perlin's distribution is
    // not Gaussian, so a stddev ratio is only an approximation of the
    // tail. Measuring the value with the same tail mass gives 0.2114.
    out.is_desert = (temp > 0.21f) && (height < kSnowBand);
    out.biome = biome_.sample(x4 * kBiomeFreq, z4 * kBiomeFreq,
                              0.0f, fw * kBiomeFreq);

    auto put = [&out](int y, BlockId b) {
        out.blocks[static_cast<std::size_t>(y)] = static_cast<std::uint8_t>(b);
    };

    // Solidity first, block type second: with a density band a column can
    // hold several solid runs, so "depth below the surface" is no longer
    // "height minus y".
    const int lo = std::max(1, height - kDensityBand);
    const int hi = std::min(kChunkSizeY - 1, height + kDensityBand);
    bool band[2 * kDensityBand + 2];
    for (int y = lo; y <= hi; ++y) {
        const float ramp = static_cast<float>(y - height)
                         / static_cast<float>(kDensityBand);
        const float d = density_.fbm(
            x4, static_cast<float>(y) * kDensityYSquash,
            z4, fw, kDensityOctaves, kDensityFreq);
        band[y - lo] = (d * kDensityStrength - ramp) > 0.0f;
    }
    // Everything under the band is stone: deeper than any surface rule
    // cares about, and a tight fill rather than a scan.
    for (int y = 0; y < lo; ++y) put(y, BlockId::Stone);

    int t_top = lo - 1;
    bool band_has_ground = false;
    for (int y = hi; y >= lo; --y)
        if (band[y - lo]) { t_top = y; band_has_ground = true; break; }
    out.top = t_top;

    // Where the density carved the whole band away, the topmost solid
    // block is the unconditional stone below it - and it would keep the
    // stone it was filled with, so a shallow column came out stone-topped
    // where every rule says sand. Give the exposed block the surface
    // material it would have had. Only reachable when the band is
    // entirely air, so it costs one write on a small minority of columns.
    if (!band_has_ground && t_top >= 1) {
        BlockId b;
        if      (height <= kSeaLevel + kSandBand) b = BlockId::Sand;
        else if (height >= kSnowBand)             b = BlockId::Snow;
        else if (out.is_desert)                   b = BlockId::Sand;
        else if (height >= kStoneBand)            b = BlockId::Stone;
        else                                      b = BlockId::Grass;
        put(t_top, b);
    }

    int depth = 0;
    for (int y = hi; y >= lo; --y) {
        if (!band[y - lo]) { depth = 0; continue; }
        BlockId b;
        if      (height <= kSeaLevel + kSandBand && depth <= 1) b = BlockId::Sand;
        else if (depth == 0 && height >= kSnowBand)             b = BlockId::Snow;
        else if (out.is_desert && depth <= 2)                   b = BlockId::Sand;
        else if (height >= kStoneBand && depth == 0)            b = BlockId::Stone;
        else if (depth == 0)                                    b = BlockId::Grass;
        else if (depth <= 3)                                    b = BlockId::Dirt;
        else                                                    b = BlockId::Stone;
        put(y, b);
        ++depth;
    }

    // Caves: the intersection of two 4D iso-surfaces. In 3D this traces
    // tubes; in 4D the same construction traces a 2-surface, so a 3D slice
    // of it is again tube-shaped - which is why the slices look like caves
    // rather than like something with no 3D analogue. Passages open and
    // close as w moves, which is the fourth dimension being visible
    // underground as well as on the surface.
    //
    // The construction carries over from the 3D generator; the iso width
    // does not, because those fields are OpenSimplex2 and these are
    // Perlin. See kCaveIsoWidth.
    // Structures before the caves-disabled early return, not after it.
    // Putting them at the bottom of this function made them a silent
    // casualty of --no-caves, which is a flag that has nothing to do with
    // them.
    //
    // A formation is a solid object, so it is stamped AFTER the cave pass
    // when there is one - a cave should not be carved through a boulder.
    if (!caves_enabled_) {
        stamp_structures(x4, z4, fw, height, out);
        return;
    }
    constexpr int   kCaveCeiling  = 5;
    constexpr int   kCaveFloor    = 1;
    // 0.025, not the 3D generator's 0.05, and the reason is that the 3D
    // cave fields are OpenSimplex2 while these are Perlin. The comment
    // above calls this "the same construction"; the construction is the
    // same and the DISTRIBUTION is not. OpenSimplex2 at this frequency
    // has stddev 0.435 against this noise's 0.219, so |n| < 0.05 catches
    // 17.2% of cells here against 7.9% there - more than twice the
    // carving, which reads as a cavier world rather than as anything
    // wrong.
    //
    // Matched by quantile for the same reason as the desert threshold:
    // scaling by the stddev ratio gives 0.0252, which catches 9.1% of
    // cells against the 3D generator's 7.9%. The width with the same
    // central mass is 0.0218.
    constexpr float kCaveIsoWidth = 0.022f;
    const int y_max = height - kCaveCeiling;
    for (int y = kCaveFloor; y <= y_max; ++y) {
        // y * 1.6 matches the 3D generator's vertical squash, which keeps
        // passages wider than they are tall.
        const float fy = static_cast<float>(y) * 1.6f;
        const float na = cave_a_.sample(x4 * kCaveFreq, fy * kCaveFreq,
                                        z4 * kCaveFreq, fw * kCaveFreq);
        const float nb = cave_b_.sample(x4 * kCaveFreq, fy * kCaveFreq,
                                        z4 * kCaveFreq, fw * kCaveFreq);
        if (std::abs(na) < kCaveIsoWidth && std::abs(nb) < kCaveIsoWidth) {
            put(y, BlockId::Air);
        }
    }

    stamp_structures(x4, z4, fw, height, out);
}

void TerrainGen4D::fill_chunk(int chunk_x, int chunk_z, Slice s, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    int  surface[kChunkSizeZ][kChunkSizeX];
    // The topmost solid block, which is not the guide height once the
    // density band can put ground above it or carve it away.
    int  top[kChunkSizeZ][kChunkSizeX];
    bool is_desert[kChunkSizeZ][kChunkSizeX];
    float biome_val[kChunkSizeZ][kChunkSizeX];

    Column4D col;
    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            const int wx = origin_x + x;
            const int wz = origin_z + z;
            // Carry the slice point into 4D once, then let fill_column do
            // the rest. Everything this loop used to inline - height,
            // biome, the density band, block typing, caves - lives there
            // now, so a chunk of voxels and a tiling of 4D cells cannot
            // drift apart.
            float x4 = 0.0f, z4 = 0.0f, w4 = 0.0f;
            to_4d(static_cast<float>(wx), static_cast<float>(wz), s,
                  &x4, &z4, &w4);
            fill_column(x4, z4, w4, col);

            surface[z][x]   = col.guide_height;
            top[z][x]       = col.top;
            is_desert[z][x] = col.is_desert;
            biome_val[z][x] = col.biome;
            for (int y = 0; y < kChunkSizeY; ++y) {
                out.set(x, y, z, static_cast<BlockId>(col.blocks[
                    static_cast<std::size_t>(y)]));
            }
        }
    }

    // Trees, and the way they respond to w is the point.
    //
    // The per-column random draw is a 2D hash, so a given column always
    // rolls the same number - but the DENSITY it is compared against comes
    // from the 4D biome field. Travel along w and the density surface
    // moves under a fixed set of draws, so forests thicken and thin in
    // spatially coherent patches rather than flickering column by column.
    // A tree that disappears takes its neighbours with it, which is what a
    // forest edge moving through the fourth dimension should look like.
    //
    // The stamps themselves come from tree_stamps.h, shared with the 3D
    // generator, so both worlds grow identical trees.
    constexpr int kMargin = 2;
    for (int z = kMargin; z < kChunkSizeZ - kMargin; ++z) {
        for (int x = kMargin; x < kChunkSizeX - kMargin; ++x) {
            // The topmost solid block, not the guide height: planting at
            // the guide buried trees inside overhangs and left others
            // floating where the density carved the ground away.
            const int h = top[z][x];
            if (is_desert[z][x]) continue;
            if (surface[z][x] <= kSeaLevel + kSandBand) continue;
            if (surface[z][x] >= kStoneBand) continue;
            if (out.get(x, h, z) != BlockId::Grass) continue;
            if (h + 8 >= kChunkSizeY) continue;

            const int wx = origin_x + x;
            const int wz = origin_z + z;
            const float r = hash2d_f(wx, wz, 0x7B1E5A2D);
            const float density = 0.012f + std::max(0.0f, biome_val[z][x]) * 0.025f;
            if (r > density) continue;

            const float pick = hash2d_f(wx + 17, wz + 41, 0x55AA00FF);
            if (h > kStoneBand - 4 || biome_val[z][x] > 0.25f) {
                if (pick < 0.6f) stamp_conifer(out, x, h + 1, z);
                else             stamp_oak(out, x, h + 1, z);
            } else {
                if (pick < 0.15f)      stamp_conifer(out, x, h + 1, z);
                else if (pick < 0.85f) stamp_oak(out, x, h + 1, z);
                else                   stamp_bush(out, x, h + 1, z);
            }
        }
    }
}

}  // namespace world
