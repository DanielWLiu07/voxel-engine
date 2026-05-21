#pragma once

#include "world/chunk.h"

#include <FastNoiseLite.h>

#include <cstdint>

namespace world {

// Multi-octave Perlin terrain. Noise samplers are constructed once at
// TerrainGen build time and shared across threads (FastNoiseLite::GetNoise
// is a const read, so concurrent sampling is safe).
class TerrainGen {
public:
    explicit TerrainGen(std::uint32_t seed = 1337);

    // Height at world-space (wx, wz) in blocks. Result is in [0, kChunkSizeY).
    int height_at(int wx, int wz) const;

    // Fill a whole chunk for a given chunk coord (in chunk-space).
    void fill_chunk(int chunk_x, int chunk_z, Chunk& out) const;

private:
    FastNoiseLite continents_;
    FastNoiseLite hills_;
};

}  // namespace world
