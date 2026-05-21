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
};

constexpr bool is_solid(BlockId b) { return b != BlockId::Air; }

constexpr bool face_visible(BlockId self, BlockId neighbor) {
    return is_solid(self) && !is_solid(neighbor);
}

}  // namespace world
