#pragma once

#include "world/chunk.h"

#include <FastNoiseLite.h>

#include <cstdint>

namespace world {

inline constexpr int kSeaLevel  = 24;
inline constexpr int kSandBand  = 2;
// Stone band at 28 left only a 4-block grass strip above sea level -
// virtually all mid-altitude terrain (heights ~30-40) rendered bare stone
// or snow and the world read as monochrome white-grey. 36 gives grassland
// and forest at mid-altitudes, a rock band at 36-39, snow caps above 40.
// Heights are unchanged, so cull/cave bench geometry is identical; only
// surface block types (and the greedy merge runs across them) shift.
inline constexpr int kStoneBand = 36;
inline constexpr int kSnowBand  = 40;  // grass above this altitude turns to snow

class TerrainGen {
public:
    explicit TerrainGen(std::uint32_t seed = 1337);

    int  height_at(int wx, int wz) const;
    void fill_chunk(int chunk_x, int chunk_z, Chunk& out) const;

    // Caves are part of normal world gen but expensive for the mesher
    // benchmark, which wants to isolate the greedy algorithm's gains on
    // contiguous terrain. Bench turns them off.
    void set_caves_enabled(bool e) { caves_enabled_ = e; }

private:
    FastNoiseLite continents_;  // low-freq landmass shape
    FastNoiseLite hills_;       // mid-freq rolling hills
    FastNoiseLite detail_;      // high-freq surface detail
    FastNoiseLite warp_;        // domain warping for organic ridges
    FastNoiseLite biome_;       // selects tree density / variant
    FastNoiseLite temp_;        // hot/cold axis - deserts vs forests
    FastNoiseLite cave_a_;      // 3D field A for cave carving
    FastNoiseLite cave_b_;      // 3D field B; intersection forms tubes
    FastNoiseLite lakes_;       // low-freq basins carved below sea level
    bool caves_enabled_ = true;
};

}  // namespace world
