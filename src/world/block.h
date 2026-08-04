#pragma once

#include <cstdint>

namespace world {

enum class BlockId : std::uint8_t {
    Air    = 0,
    Stone  = 1,
    Dirt   = 2,
    Grass  = 3,
    Sand   = 4,
    Wood   = 5,
    Leaves = 6,
    Snow   = 7,
};

// Highest id the enum defines; bytes above this are not blocks. The RLE
// decoder rejects them so a corrupt save cannot smuggle in garbage ids.
constexpr std::uint8_t kMaxBlockId = static_cast<std::uint8_t>(BlockId::Snow);

constexpr bool is_solid(BlockId b) { return b != BlockId::Air; }

// One entry per block id, Air included: the size of every id-indexed
// lookup table (the render palette in particular).
constexpr int kBlockPaletteSize = static_cast<int>(kMaxBlockId) + 1;

constexpr bool face_visible(BlockId self, BlockId neighbor) {
    return is_solid(self) && !is_solid(neighbor);
}

}  // namespace world
