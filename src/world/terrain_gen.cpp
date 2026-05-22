#include "world/terrain_gen.h"

#include <algorithm>
#include <cstdint>

namespace world {

namespace {

float sample_height_noise(const FastNoiseLite& continents,
                          const FastNoiseLite& hills,
                          const FastNoiseLite& detail,
                          const FastNoiseLite& warp,
                          float wx, float wz) {
    // Domain warp: distort the sample coordinates by a low-frequency
    // noise field. Turns straight Perlin lobes into organic ridges.
    float ox = warp.GetNoise(wx, wz) * 60.0f;
    float oz = warp.GetNoise(wx + 113.0f, wz + 271.0f) * 60.0f;
    float c = continents.GetNoise(wx + ox, wz + oz);
    float h = hills.GetNoise(wx, wz);
    float d = detail.GetNoise(wx, wz);
    return c * 0.65f + h * 0.25f + d * 0.10f;
}

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

// Small oak: 4-tall trunk, 3-wide cross canopy with corners knocked off.
void stamp_oak(Chunk& c, int lx, int base_y, int lz) {
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

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (std::abs(dx) == 2 && std::abs(dz) == 2) continue;
            put_leaf(lx + dx, top - 1, lz + dz);
        }
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (std::abs(dx) == 1 && std::abs(dz) == 1
                && ((hash2d(lx + dx, lz + dz, 0xA1B2C3) & 1) == 0)) continue;
            put_leaf(lx + dx, top, lz + dz);
        }
    }
    put_leaf(lx, top + 1, lz);
}

// Tall conifer: 7-tall trunk, pointy stepped canopy.
void stamp_conifer(Chunk& c, int lx, int base_y, int lz) {
    constexpr int kTrunkH = 7;
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

    // Stepped triangular silhouette: wider near the base.
    for (int layer = 0; layer < 4; ++layer) {
        int y = base_y + 2 + layer * 2;
        int r = 2 - layer / 2;
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) + std::abs(dz) > r + 1) continue;
                put_leaf(lx + dx, y, lz + dz);
            }
        }
    }
    put_leaf(lx, top + 1, lz);
}

// Small bush: 1-tall stem, 3x3x2 leaf clump.
void stamp_bush(Chunk& c, int lx, int base_y, int lz) {
    if (in_chunk_bounds(lx, base_y, lz)) c.set(lx, base_y, lz, BlockId::Wood);

    auto put_leaf = [&](int x, int y, int z) {
        if (!in_chunk_bounds(x, y, z)) return;
        if (is_solid(c.get(x, y, z))) return;
        c.set(x, y, z, BlockId::Leaves);
    };

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            put_leaf(lx + dx, base_y + 1, lz + dz);
            if (dx == 0 && dz == 0) put_leaf(lx + dx, base_y + 2, lz + dz);
        }
    }
}

}  // namespace

TerrainGen::TerrainGen(std::uint32_t seed) {
    continents_.SetSeed(static_cast<int>(seed));
    continents_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    continents_.SetFrequency(0.004f);
    continents_.SetFractalType(FastNoiseLite::FractalType_FBm);
    continents_.SetFractalOctaves(4);

    hills_.SetSeed(static_cast<int>(seed) + 1);
    hills_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    hills_.SetFrequency(0.020f);
    hills_.SetFractalType(FastNoiseLite::FractalType_FBm);
    hills_.SetFractalOctaves(4);

    detail_.SetSeed(static_cast<int>(seed) + 2);
    detail_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    detail_.SetFrequency(0.080f);
    detail_.SetFractalType(FastNoiseLite::FractalType_FBm);
    detail_.SetFractalOctaves(2);

    warp_.SetSeed(static_cast<int>(seed) + 3);
    warp_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    warp_.SetFrequency(0.012f);

    biome_.SetSeed(static_cast<int>(seed) + 4);
    biome_.SetNoiseType(FastNoiseLite::NoiseType_Perlin);
    biome_.SetFrequency(0.008f);
}

int TerrainGen::height_at(int wx, int wz) const {
    float n = sample_height_noise(continents_, hills_, detail_, warp_,
                                  static_cast<float>(wx), static_cast<float>(wz));
    int height = static_cast<int>(kSeaLevel + n * 28.0f + 14.0f);
    return std::clamp(height, 1, kChunkSizeY - 1);
}

void TerrainGen::fill_chunk(int chunk_x, int chunk_z, Chunk& out) const {
    const int origin_x = chunk_x * kChunkSizeX;
    const int origin_z = chunk_z * kChunkSizeZ;

    int surface[kChunkSizeZ][kChunkSizeX];
    float biome_val[kChunkSizeZ][kChunkSizeX];

    for (int z = 0; z < kChunkSizeZ; ++z) {
        for (int x = 0; x < kChunkSizeX; ++x) {
            int wx = origin_x + x;
            int wz = origin_z + z;
            float n = sample_height_noise(continents_, hills_, detail_, warp_,
                                          static_cast<float>(wx), static_cast<float>(wz));
            int height = std::clamp(static_cast<int>(kSeaLevel + n * 28.0f + 14.0f),
                                    1, kChunkSizeY - 1);
            surface[z][x] = height;
            biome_val[z][x] = biome_.GetNoise(static_cast<float>(wx),
                                              static_cast<float>(wz));

            for (int y = 0; y <= height; ++y) {
                BlockId b;
                if      (y == 0)                                            b = BlockId::Stone;
                else if (height <= kSeaLevel + kSandBand && y >= height-1)  b = BlockId::Sand;
                else if (y == height && height >= kSnowBand)                b = BlockId::Snow;
                else if (height >= kStoneBand && y == height)               b = BlockId::Stone;
                else if (y == height)                                       b = BlockId::Grass;
                else if (y >= height - 3)                                   b = BlockId::Dirt;
                else                                                        b = BlockId::Stone;
                out.set(x, y, z, b);
            }
        }
    }

    // Tree pass: variant depends on biome noise + altitude.
    constexpr int kMargin = 2;
    for (int z = kMargin; z < kChunkSizeZ - kMargin; ++z) {
        for (int x = kMargin; x < kChunkSizeX - kMargin; ++x) {
            int h = surface[z][x];
            if (h <= kSeaLevel + kSandBand) continue;
            if (h >= kStoneBand) continue;
            if (out.get(x, h, z) != BlockId::Grass) continue;
            if (h + 8 >= kChunkSizeY) continue;

            const int wx = origin_x + x;
            const int wz = origin_z + z;
            const float r = hash2d_f(wx, wz, 0x7B1E5A2D);

            // Density varies by biome. Negative biome value = sparser
            // (plains), positive = denser (forest).
            const float density = 0.012f + std::max(0.0f, biome_val[z][x]) * 0.025f;
            if (r > density) continue;

            // Variant pick by a second hash + biome.
            float pick = hash2d_f(wx + 17, wz + 41, 0x55AA00FF);
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
