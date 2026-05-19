#include "world/terrain_gen.h"

#include <FastNoiseLite.h>

#include <algorithm>

namespace world {

namespace {

// Sea level. Anything below sand line is mostly sand, hills above are
// grass, exposed peaks turn to stone.
constexpr int kSeaLevel  = 24;
constexpr int kSandBand  = 2;   // sand extends this many blocks above sea
constexpr int kStoneBand = 28;  // grass below this height, stone above

// Combine two octaves of Perlin: a low-frequency continent shape and a
// higher-frequency hill detail. Returns normalized in [-1, 1].
float sample_height_noise(const FastNoiseLite& continents,
                          const FastNoiseLite& hills,
                          float wx, float wz) {
    float c = continents.GetNoise(wx, wz);              // [-1, 1]
    float h = hills.GetNoise(wx, wz);                   // [-1, 1]
    return c * 0.75f + h * 0.25f;
}

}  // namespace

TerrainGen::TerrainGen(std::uint32_t seed) : seed_(seed) {}

int TerrainGen::height_at(int wx, int wz) const {
    FastNoiseLite continents(static_cast<int>(seed_));
    continents.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    continents.SetFrequency(0.005f);
    continents.SetFractalType(FastNoiseLite::FractalType_FBm);
    continents.SetFractalOctaves(3);

    FastNoiseLite hills(static_cast<int>(seed_) + 1);
    hills.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    hills.SetFrequency(0.025f);
    hills.SetFractalType(FastNoiseLite::FractalType_FBm);
    hills.SetFractalOctaves(4);

    float n = sample_height_noise(continents, hills,
                                  static_cast<float>(wx),
                                  static_cast<float>(wz));
    // Map [-1, 1] -> [kSeaLevel - 12, kSeaLevel + 36]
    float h = kSeaLevel + n * 24.0f + 12.0f;
    int height = static_cast<int>(h);
    return std::clamp(height, 1, kChunkSizeY - 1);
}

void TerrainGen::fill_chunk(int chunk_x, int chunk_z, Chunk& out) const {
    // Build noise samplers once per chunk (cheap; not per column).
    FastNoiseLite continents(static_cast<int>(seed_));
    continents.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    continents.SetFrequency(0.005f);
    continents.SetFractalType(FastNoiseLite::FractalType_FBm);
    continents.SetFractalOctaves(3);

    FastNoiseLite hills(static_cast<int>(seed_) + 1);
    hills.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    hills.SetFrequency(0.025f);
    hills.SetFractalType(FastNoiseLite::FractalType_FBm);
    hills.SetFractalOctaves(4);

    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            int wx = origin_x + x;
            int wz = origin_z + z;

            float n = sample_height_noise(continents, hills,
                                          static_cast<float>(wx),
                                          static_cast<float>(wz));
            float h = kSeaLevel + n * 24.0f + 12.0f;
            int height = std::clamp(static_cast<int>(h), 1, kChunkSizeY - 1);

            for (int y = 0; y <= height; ++y) {
                BlockId b;
                if (y == 0) {
                    b = BlockId::Stone;             // bedrock layer
                } else if (height <= kSeaLevel + kSandBand && y >= height - 1) {
                    b = BlockId::Sand;              // beaches near sea level
                } else if (height >= kStoneBand && y == height) {
                    b = BlockId::Stone;             // exposed mountain tops
                } else if (y == height) {
                    b = BlockId::Grass;             // grass surface
                } else if (y >= height - 3) {
                    b = BlockId::Dirt;              // dirt subsurface
                } else {
                    b = BlockId::Stone;             // stone interior
                }
                out.set(x, y, z, b);
            }
        }
    }
}

}  // namespace world
