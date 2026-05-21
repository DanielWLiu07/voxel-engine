#include "world/terrain_gen.h"

#include <algorithm>
#include <cstdint>

namespace world {

namespace {

float sample_height_noise(const FastNoiseLite& continents,
                          const FastNoiseLite& hills,
                          float wx, float wz) {
    float c = continents.GetNoise(wx, wz);
    float h = hills.GetNoise(wx, wz);
    return c * 0.75f + h * 0.25f;
}

// xxhash-style 2D hash; deterministic per (x, z) so chunks meshed on
// different workers agree on tree placement.
std::uint32_t hash2d(int x, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u
                    + static_cast<std::uint32_t>(z) * 0x85EBCA77u
                    + seed * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

float hash2d_f(int x, int z, std::uint32_t seed) {
    return (hash2d(x, z, seed) & 0x00FFFFFFu) / 16777216.0f;
}

void stamp_tree(Chunk& c, int lx, int base_y, int lz) {
    constexpr int kTrunkH = 5;
    const int top = base_y + kTrunkH;

    for (int dy = 0; dy < kTrunkH; ++dy) {
        int y = base_y + dy;
        if (y >= 0 && y < kChunkSizeY) c.set(lx, y, lz, BlockId::Wood);
    }

    auto put_leaf = [&](int x, int y, int z) {
        if (!in_chunk_bounds(x, y, z)) return;
        if (is_solid(c.get(x, y, z))) return;
        c.set(x, y, z, BlockId::Leaves);
    };

    int my = top - 1;
    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (std::abs(dx) == 2 && std::abs(dz) == 2) continue;
            put_leaf(lx + dx, my, lz + dz);
        }
    }
    int uy = top;
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (std::abs(dx) == 1 && std::abs(dz) == 1
                && ((hash2d(lx + dx, lz + dz, 0xA1B2C3) & 1) == 0)) continue;
            put_leaf(lx + dx, uy, lz + dz);
        }
    }
    put_leaf(lx, uy + 1, lz);
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
                                  static_cast<float>(wx), static_cast<float>(wz));
    int height = static_cast<int>(kSeaLevel + n * 24.0f + 12.0f);
    return std::clamp(height, 1, kChunkSizeY - 1);
}

void TerrainGen::fill_chunk(int chunk_x, int chunk_z, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    int surface[kChunkSizeZ][kChunkSizeX];

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            int wx = origin_x + x;
            int wz = origin_z + z;
            float n = sample_height_noise(continents_, hills_,
                                          static_cast<float>(wx), static_cast<float>(wz));
            int height = std::clamp(static_cast<int>(kSeaLevel + n * 24.0f + 12.0f),
                                    1, kChunkSizeY - 1);
            surface[z][x] = height;

            for (int y = 0; y <= height; ++y) {
                BlockId b;
                if      (y == 0)                                            b = BlockId::Stone;
                else if (height <= kSeaLevel + kSandBand && y >= height-1)  b = BlockId::Sand;
                else if (height >= kStoneBand && y == height)               b = BlockId::Stone;
                else if (y == height)                                       b = BlockId::Grass;
                else if (y >= height - 3)                                   b = BlockId::Dirt;
                else                                                        b = BlockId::Stone;
                out.set(x, y, z, b);
            }
        }
    }

    // Trees only on grass tops, away from chunk edges so a 5-wide canopy
    // fits inside one chunk.
    constexpr float kTreeChance = 0.015f;
    constexpr int   kMargin = 2;
    for (int z = kMargin; z < kChunkSizeZ - kMargin; ++z) {
        for (int x = kMargin; x < kChunkSizeX - kMargin; ++x) {
            int h = surface[z][x];
            if (h <= kSeaLevel + kSandBand) continue;
            if (h >= kStoneBand) continue;
            if (out.get(x, h, z) != BlockId::Grass) continue;
            if (h + 7 >= kChunkSizeY) continue;
            if (hash2d_f(origin_x + x, origin_z + z, 0x7B1E5A2D) > kTreeChance) continue;
            stamp_tree(out, x, h + 1, z);
        }
    }
}

}  // namespace world
