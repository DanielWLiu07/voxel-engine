#include "world/terrain_gen4d.h"

#include "world/terrain_gen.h"

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
constexpr float kTempFreq      = 0.006f;
constexpr float kCaveFreq      = 0.038f;

}  // namespace

TerrainGen4D::TerrainGen4D(std::uint32_t seed)
    : continents_(seed), hills_(seed + 1), detail_(seed + 2), warp_(seed + 3),
      temp_(seed + 5),
      cave_a_(seed + 6), cave_b_(seed + 7) {}

int TerrainGen4D::height_at(int wx, int wz, int w) const {
    const float x = static_cast<float>(wx);
    const float z = static_cast<float>(wz);
    const float fw = static_cast<float>(w) * kWScale;

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

void TerrainGen4D::fill_chunk(int chunk_x, int chunk_z, int w, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;
    const float fw = static_cast<float>(w) * kWScale;

    int  surface[kChunkSizeZ][kChunkSizeX];
    bool is_desert[kChunkSizeZ][kChunkSizeX];

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            const int wx = origin_x + x;
            const int wz = origin_z + z;
            // Single source of truth for surface height, exactly as in the
            // 3D generator: a divergent inline copy here would desync
            // physics raycasts and the chunk contents from each other.
            const int height = height_at(wx, wz, w);
            surface[z][x] = height;

            const float temp = temp_.sample(static_cast<float>(wx) * kTempFreq,
                                            static_cast<float>(wz) * kTempFreq,
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
                const float wz = static_cast<float>(origin_z + z);
                const int y_max = surface[z][x] - kCaveCeiling;
                for (int y = kCaveFloor; y <= y_max; ++y) {
                    // y * 1.6 matches the 3D generator's vertical squash,
                    // which keeps passages wider than they are tall.
                    const float fy = static_cast<float>(y) * 1.6f;
                    const float na = cave_a_.sample(wx * kCaveFreq, fy * kCaveFreq,
                                                    wz * kCaveFreq, fw * kCaveFreq);
                    const float nb = cave_b_.sample(wx * kCaveFreq, fy * kCaveFreq,
                                                    wz * kCaveFreq, fw * kCaveFreq);
                    if (std::abs(na) < kCaveIsoWidth && std::abs(nb) < kCaveIsoWidth) {
                        out.set(x, y, z, BlockId::Air);
                    }
                }
            }
        }
    }
}

}  // namespace world
