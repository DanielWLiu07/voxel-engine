#pragma once

#include "world/chunk.h"

#include <FastNoiseLite.h>

#include <cstdint>

namespace world {

inline constexpr int kSeaLevel  = 24;
inline constexpr int kSandBand  = 2;
inline constexpr int kStoneBand = 28;

// Multi-octave Perlin terrain with trees. fill_chunk and height_at are
// const + thread-safe: FastNoiseLite::GetNoise is a const read.
class TerrainGen {
public:
    explicit TerrainGen(std::uint32_t seed = 1337);

    int  height_at(int wx, int wz) const;
    void fill_chunk(int chunk_x, int chunk_z, Chunk& out) const;

private:
    FastNoiseLite continents_;
    FastNoiseLite hills_;
};

}  // namespace world
