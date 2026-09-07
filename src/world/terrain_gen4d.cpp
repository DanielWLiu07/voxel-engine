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
      cave_a_(seed + 6), cave_b_(seed + 7), density_(seed + 8) {}

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
    if (!caves_enabled_) return;
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
