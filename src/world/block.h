#pragma once

#include <cstdint>

namespace world {

// Single byte per block. Plenty of headroom for the MVP block set.
enum class BlockId : std::uint8_t {
    Air   = 0,
    Stone = 1,
    Dirt  = 2,
    Grass = 3,
    Sand  = 4,
    Wood  = 5,
    Leaves = 6,
};

constexpr bool is_solid(BlockId b) {
    return b != BlockId::Air;
}

// Whether a face between `self` and `neighbor` should be emitted from
// self's side. Air shows nothing; solid shows a face when the neighbor
// is not solid (transparent blocks like glass would need their own rule).
constexpr bool face_visible(BlockId self, BlockId neighbor) {
    return is_solid(self) && !is_solid(neighbor);
}

}  // namespace world
