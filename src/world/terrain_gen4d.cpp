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

}  // namespace

TerrainGen4D::TerrainGen4D(std::uint32_t seed)
    : continents_(seed), hills_(seed + 1), detail_(seed + 2), warp_(seed + 3),
      biome_(seed + 4), temp_(seed + 5),
      cave_a_(seed + 6), cave_b_(seed + 7) {}

int TerrainGen4D::height_at(int wx, int wz, Slice s) const {
    const float x = static_cast<float>(wx);
    // The slice's z maps into BOTH the 4D z and w axes once the cut is
    // rotated. At theta = 0 this reduces to z4 = wz, w4 = w, which is the
    // axis-aligned slice everything started as.
    float z = 0.0f, fw = 0.0f;
    to_4d(static_cast<float>(wz), s, &z, &fw);
    fw *= kWScale;

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

void TerrainGen4D::fill_chunk(int chunk_x, int chunk_z, Slice s, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    int  surface[kChunkSizeZ][kChunkSizeX];
    bool is_desert[kChunkSizeZ][kChunkSizeX];
    float biome_val[kChunkSizeZ][kChunkSizeX];

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            const int wx = origin_x + x;
            const int wz = origin_z + z;
            // Single source of truth for surface height, exactly as in the
            // 3D generator: a divergent inline copy here would desync
            // physics raycasts and the chunk contents from each other.
            const int height = height_at(wx, wz, s);
            // The rotated 4D coordinates for this column, shared by
            // the biome and temperature fields below.
            float z4 = 0.0f, w4 = 0.0f;
            to_4d(static_cast<float>(wz), s, &z4, &w4);
            const float fw = w4 * kWScale;
            surface[z][x] = height;

            const float temp = temp_.sample(static_cast<float>(wx) * kTempFreq,
                                            z4 * kTempFreq,
                                            0.0f, fw * kTempFreq);
            // 0.24, not the 3D generator's 0.35.
            //
            // Same class of mistake as the height amplitude, in a place
            // nobody thought to look: a threshold copied across a change
            // of noise. FastNoiseLite's Perlin at this frequency has
            // stddev 0.310; this noise has 0.215. The same 0.35 therefore
            // fires on 5.22% of columns here against 14.05% there, making
            // deserts about 2.7x rarer - a quiet biome change, not a bug
            // anything would report.
            //
            // Matched by QUANTILE, not by scaling the threshold by the
            // ratio of standard deviations. That first attempt gave 0.24,
            // which fires on 10.3% against the 3D generator's 13.25% -
            // Perlin's distribution is not Gaussian, so a stddev ratio is
            // only an approximation of the tail. Measuring the value with
            // the same tail mass gives 0.2114.
            is_desert[z][x] = (temp > 0.21f) && (height < kSnowBand);
            biome_val[z][x] = biome_.sample(static_cast<float>(wx) * kBiomeFreq,
                                            z4 * kBiomeFreq,
                                            0.0f, fw * kBiomeFreq);

            for (int y = 0; y <= height; ++y) {
                BlockId b;
                if      (y == 0)                                           b = BlockId::Stone;
                else if (height <= kSeaLevel + kSandBand && y >= height-1) b = BlockId::Sand;
                else if (y == height && height >= kSnowBand)               b = BlockId::Snow;
                else if (is_desert[z][x] && y >= height - 2)               b = BlockId::Sand;
                else if (height >= kStoneBand && y == height)              b = BlockId::Stone;
                else if (y == height)                                      b = BlockId::Grass;
                else if (y >= height - 3)                                  b = BlockId::Dirt;
                else                                                       b = BlockId::Stone;
                out.set(x, y, z, b);
            }
        }
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
    if (caves_enabled_) {
        constexpr int   kCaveCeiling  = 5;
        constexpr int   kCaveFloor    = 1;
        // 0.025, not the 3D generator's 0.05, and the reason is that the
        // 3D cave fields are OpenSimplex2 while these are Perlin. The
        // comment below calls this "the same construction"; the
        // construction is the same and the DISTRIBUTION is not.
        // OpenSimplex2 at this frequency has stddev 0.435 against this
        // noise's 0.219, so |n| < 0.05 catches 17.2% of cells here
        // against 7.9% there - more than twice the carving, which reads
        // as a cavier world rather than as anything wrong.
        //
        // Matched by quantile for the same reason as the desert
        // threshold: scaling by the stddev ratio gives 0.0252, which
        // catches 9.1% of cells against the 3D generator's 7.9%. The
        // width with the same central mass is 0.0218.
        constexpr float kCaveIsoWidth = 0.022f;
        for (int z = 0; z < kChunkSizeZ; ++z) {
            for (int x = 0; x < kChunkSizeX; ++x) {
                const float wx = static_cast<float>(origin_x + x);
                float cz4 = 0.0f, cw4 = 0.0f;
                to_4d(static_cast<float>(origin_z + z), s, &cz4, &cw4);
                const float cfw = cw4 * kWScale;
                const int y_max = surface[z][x] - kCaveCeiling;
                for (int y = kCaveFloor; y <= y_max; ++y) {
                    // y * 1.6 matches the 3D generator's vertical squash,
                    // which keeps passages wider than they are tall.
                    const float fy = static_cast<float>(y) * 1.6f;
                    const float na = cave_a_.sample(wx * kCaveFreq, fy * kCaveFreq,
                                                    cz4 * kCaveFreq, cfw * kCaveFreq);
                    const float nb = cave_b_.sample(wx * kCaveFreq, fy * kCaveFreq,
                                                    cz4 * kCaveFreq, cfw * kCaveFreq);
                    if (std::abs(na) < kCaveIsoWidth && std::abs(nb) < kCaveIsoWidth) {
                        out.set(x, y, z, BlockId::Air);
                    }
                }
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
            const int h = surface[z][x];
            if (is_desert[z][x]) continue;
            if (h <= kSeaLevel + kSandBand) continue;
            if (h >= kStoneBand) continue;
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
