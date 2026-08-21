#pragma once

#include <array>
#include <cstdint>

#include "world/block.h"
#include "world/chunk.h"

namespace world {

// Per-block light level, 0..15, packed two levels to a byte.
//
// A byte per block would be 64 KB on top of every chunk's 64 KB of block
// ids - 40 MB at radius 12, for a value that only needs four bits. This
// project already refuses that trade for the mesh vertex, so it refuses it
// here: 32 KB per chunk, and the packing is the same idea as the 12-byte
// vertex applied to a second grid.
class LightGrid {
public:
    void clear() { data_.fill(0); }

    std::uint8_t get(int x, int y, int z) const {
        const std::size_t i = chunk_index(x, y, z);
        const std::uint8_t byte = data_[i >> 1];
        return (i & 1) ? static_cast<std::uint8_t>(byte >> 4)
                       : static_cast<std::uint8_t>(byte & 0x0F);
    }

    void set(int x, int y, int z, std::uint8_t level) {
        const std::size_t i = chunk_index(x, y, z);
        std::uint8_t& byte = data_[i >> 1];
        if (i & 1) byte = static_cast<std::uint8_t>((byte & 0x0F) | (level << 4));
        else       byte = static_cast<std::uint8_t>((byte & 0xF0) | (level & 0x0F));
    }

    std::size_t bytes() const { return data_.size(); }

private:
    std::array<std::uint8_t, (kChunkVolume + 1) / 2> data_{};
};

// One face of a neighbour's light, copied for the same reason the block
// boundary planes are: a worker must not follow pointers into the chunk
// map while the main thread owns it.
struct LightPlane {
    bool present = false;
    std::array<std::uint8_t, kChunkSizeY * kChunkSizeX> level{};

    std::uint8_t at(int t, int y) const {
        return level[static_cast<std::size_t>(y) * kChunkSizeX + t];
    }
    void set(int t, int y, std::uint8_t v) {
        level[static_cast<std::size_t>(y) * kChunkSizeX + t] = v;
    }
};

struct NeighborLight {
    LightPlane neg_x, pos_x, neg_z, pos_z;
};

struct LightStats {
    std::uint64_t cells_visited = 0;   // BFS pops
    std::uint64_t sources = 0;         // emissive blocks seeded
    double build_ms = 0.0;
};

// Flood-fills light from every emissive block in the chunk, plus whatever
// spills in from the neighbour faces, decrementing one level per step and
// stopping at anything that does not transmit.
//
// Seeding from neighbour faces is what keeps light continuous across a
// chunk boundary. Without it a torch near an edge would light its own
// chunk and stop dead at x=16, which is a seam a player sees immediately.
LightStats propagate_light(const Chunk& chunk, const NeighborLight& in,
                           LightGrid& out);

}  // namespace world
