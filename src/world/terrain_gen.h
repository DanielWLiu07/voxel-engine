#pragma once

#include "world/chunk.h"

#include <FastNoiseLite.h>

#include <cstdint>

namespace world {

inline constexpr int kSeaLevel  = 24;
inline constexpr int kSandBand  = 2;
inline constexpr int kStoneBand = 28;
inline constexpr int kSnowBand  = 40;  // grass above this altitude turns to snow

class TerrainGen {
public:
    explicit TerrainGen(std::uint32_t seed = 1337);

    int  height_at(int wx, int wz) const;
    void fill_chunk(int chunk_x, int chunk_z, Chunk& out) const;

private:
    FastNoiseLite continents_;  // low-freq landmass shape
    FastNoiseLite hills_;       // mid-freq rolling hills
    FastNoiseLite detail_;      // high-freq surface detail
    FastNoiseLite warp_;        // domain warping for organic ridges
    FastNoiseLite biome_;       // selects tree type / snow probability
};

}  // namespace world
