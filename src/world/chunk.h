#pragma once

#include "world/block.h"

#include <array>
#include <cstdint>

namespace world {

inline constexpr int kChunkSizeX = 16;
inline constexpr int kChunkSizeZ = 16;
inline constexpr int kChunkSizeY = 256;
inline constexpr int kChunkVolume = kChunkSizeX * kChunkSizeY * kChunkSizeZ;

// Y-major flat storage; a vertical column lives contiguously.
constexpr int chunk_index(int x, int y, int z) {
    return (y * kChunkSizeZ * kChunkSizeX) + (z * kChunkSizeX) + x;
}

// A square footprint is not a style choice, it is load-bearing, and four
// places read one of these constants where the other is meant:
// chunk_mesh's axis_size() returns kChunkSizeX for the Z axis, the greedy
// sweep's boundary planes stride by kChunkSizeX whichever face they hold,
// and chunk_light's LightPlane and its seed loop do the same. All four are
// correct while the two are equal and silently wrong the moment they are
// not - the mesher would sweep the wrong extent and emit geometry for
// blocks it never read. Cheaper to fail here than to find that later.
static_assert(kChunkSizeX == kChunkSizeZ,
              "chunk_mesh and chunk_light index X-facing and Z-facing "
              "planes with the same stride; a non-square chunk needs both "
              "fixed before this can be relaxed");

constexpr bool in_chunk_bounds(int x, int y, int z) {
    return x >= 0 && x < kChunkSizeX
        && y >= 0 && y < kChunkSizeY
        && z >= 0 && z < kChunkSizeZ;
}

// Vertical sub-chunks. The mesh is still built chunk-wide (so the greedy
// merger doesn't get split at section boundaries), then bucketed into
// kSectionsPerChunk per-section meshes. Lives here (not world.h) so the
// section-visibility flood fill can size its output without pulling in the
// world container.
inline constexpr int kSectionHeight    = 32;
inline constexpr int kSectionsPerChunk = kChunkSizeY / kSectionHeight;
static_assert(kSectionHeight * kSectionsPerChunk == kChunkSizeY,
              "kChunkSizeY must be a clean multiple of kSectionHeight");

class Chunk {
public:
    Chunk() { blocks_.fill(static_cast<std::uint8_t>(BlockId::Air)); }

    BlockId get(int x, int y, int z) const {
        return static_cast<BlockId>(blocks_[chunk_index(x, y, z)]);
    }

    BlockId get_or_air(int x, int y, int z) const {
        return in_chunk_bounds(x, y, z) ? get(x, y, z) : BlockId::Air;
    }

    void set(int x, int y, int z, BlockId b) {
        std::uint8_t prev = blocks_[chunk_index(x, y, z)];
        std::uint8_t next = static_cast<std::uint8_t>(b);
        if (prev == next) return;
        blocks_[chunk_index(x, y, z)] = next;
        if (is_solid(static_cast<BlockId>(prev))) --solid_count_;
        if (is_solid(b)) ++solid_count_;
    }

    int  solid_count() const { return solid_count_; }
    bool empty() const { return solid_count_ == 0; }

private:
    std::array<std::uint8_t, kChunkVolume> blocks_{};
    int solid_count_ = 0;
};

}  // namespace world
