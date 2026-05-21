#include "world/terrain_gen.h"

#include <algorithm>

namespace world {

namespace {

constexpr int kSeaLevel  = 24;
constexpr int kSandBand  = 2;
constexpr int kStoneBand = 28;

float sample_height_noise(const FastNoiseLite& continents,
                          const FastNoiseLite& hills,
                          float wx, float wz) {
    float c = continents.GetNoise(wx, wz);
    float h = hills.GetNoise(wx, wz);
    return c * 0.75f + h * 0.25f;
}

}  // namespace

TerrainGen::TerrainGen(std::uint32_t seed) {
    continents_.SetSeed(static_cast<int>(seed));
    continents_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    continents_.SetFrequency(0.005f);
    continents_.SetFractalType(FastNoiseLite::FractalType_FBm);
    continents_.SetFractalOctaves(3);

    hills_.SetSeed(static_cast<int>(seed) + 1);
    hills_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    hills_.SetFrequency(0.025f);
    hills_.SetFractalType(FastNoiseLite::FractalType_FBm);
    hills_.SetFractalOctaves(4);
}

int TerrainGen::height_at(int wx, int wz) const {
    float n = sample_height_noise(continents_, hills_,
                                  static_cast<float>(wx),
                                  static_cast<float>(wz));
    float h = kSeaLevel + n * 24.0f + 12.0f;
    int height = static_cast<int>(h);
    return std::clamp(height, 1, kChunkSizeY - 1);
}

void TerrainGen::fill_chunk(int chunk_x, int chunk_z, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            int wx = origin_x + x;
            int wz = origin_z + z;

            float n = sample_height_noise(continents_, hills_,
                                          static_cast<float>(wx),
                                          static_cast<float>(wz));
            float h = kSeaLevel + n * 24.0f + 12.0f;
            int height = std::clamp(static_cast<int>(h), 1, kChunkSizeY - 1);

            for (int y = 0; y <= height; ++y) {
                BlockId b;
                if (y == 0) {
                    b = BlockId::Stone;
                } else if (height <= kSeaLevel + kSandBand && y >= height - 1) {
                    b = BlockId::Sand;
                } else if (height >= kStoneBand && y == height) {
                    b = BlockId::Stone;
                } else if (y == height) {
                    b = BlockId::Grass;
                } else if (y >= height - 3) {
                    b = BlockId::Dirt;
                } else {
                    b = BlockId::Stone;
                }
                out.set(x, y, z, b);
            }
        }
    }
}

}  // namespace world
