#pragma once

#include "world/chunk.h"

#include <cstdint>

namespace world {

// Multi-octave Perlin terrain. Stateless after construction: safe to call
// from multiple threads.
class TerrainGen {
public:
    explicit TerrainGen(std::uint32_t seed = 1337);

    // Height at world-space (wx, wz) in blocks. Result is in [0, kChunkSizeY).
    int height_at(int wx, int wz) const;

    // Fill a whole chunk for a given chunk coord (in chunk-space).
    void fill_chunk(int chunk_x, int chunk_z, Chunk& out) const;

private:
    std::uint32_t seed_ = 1337;
};

}  // namespace world
