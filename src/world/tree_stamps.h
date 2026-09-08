#pragma once

#include "world/block.h"
#include "world/chunk.h"

#include <cmath>
#include <cstdint>

namespace world {

// The three tree stamps, shared by the 3D and 4D generators.
//
// They lived in terrain_gen.cpp's anonymous namespace, and the 4D
// generator went without trees rather than copy them. That was the right
// call at the time: this project has already been bitten twice by
// constants duplicated across the two generators and then changed in one
// - the w-scale table measured a copy that had drifted, and the cave iso
// width and desert threshold were carried over unrefitted. A third copy
// of the trunk heights and canopy shapes would have been a third
// opportunity for the same bug.
//
// Sharing them removes the opportunity instead. Both generators now stamp
// identical trees by construction, and the heights the tests pin (bush 1,
// oak 5, conifer 7) are pinned for both.

// Deterministic per-column hash, shared so tree placement is reproducible
// and identical between the generators.
inline std::uint32_t hash2d(int x, int z, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u
                    + static_cast<std::uint32_t>(z) * 0x85EBCA77u
                    + seed * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

inline float hash2d_f(int x, int z, std::uint32_t seed) {
    return (hash2d(x, z, seed) & 0x00FFFFFFu) / 16777216.0f;
}

// The same, over a site of the 4D lattice rather than a column.
//
// Structures are placed on a coarse grid in (x4, z4, w4), so they need a
// draw that depends on all three. Folding w in through its own multiplier
// before the avalanche keeps neighbouring w slabs uncorrelated - without
// that, a structure would repeat identically down the fourth axis, which
// is the one place a player would notice it immediately.
inline std::uint32_t hash4d(int x, int z, int w, std::uint32_t seed) {
    return hash2d(x, z, seed ^ (static_cast<std::uint32_t>(w) * 0x27D4EB2Du));
}

inline float hash4d_f(int x, int z, int w, std::uint32_t seed) {
    return (hash4d(x, z, w, seed) & 0x00FFFFFFu) / 16777216.0f;
}

// The three stamps are templates on what they write into, and the two
// sinks are not interchangeable geometry: the cube generator stamps into
// a Chunk addressed by voxel, the prism generator into a set of 4D
// lattice cells addressed by (i, k) at a fixed w. A tree is the one
// feature that spills sideways, so it is the one that has to know which
// space it is spilling in.
//
// A sink provides get, set, and in_bounds - Chunk already has all three.
// Nothing else about the stamps changes, which is the point: both worlds
// keep growing identical trees, and the heights the tests pin (bush 1,
// oak 5, conifer 7) stay pinned for both.

// Small oak: 5-tall trunk under a 5-wide canopy layer with its four
// corners knocked off, a 3x3 layer above it missing a random half of its
// corners, and one leaf on top.
template <class Sink>
inline void stamp_oak(Sink& c, int lx, int base_y, int lz) {
    constexpr int kTrunkH = 5;
    const int top = base_y + kTrunkH;

    for (int dy = 0; dy < kTrunkH; ++dy) {
        int y = base_y + dy;
        if (y >= 0 && y < kChunkSizeY) c.set(lx, y, lz, BlockId::Wood);
    }

    auto put_leaf = [&](int x, int y, int z) {
        if (!c.in_bounds(x, y, z)) return;
        if (is_solid(c.get(x, y, z))) return;
        c.set(x, y, z, BlockId::Leaves);
    };

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (std::abs(dx) == 2 && std::abs(dz) == 2) continue;
            put_leaf(lx + dx, top - 1, lz + dz);
        }
    }
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (std::abs(dx) == 1 && std::abs(dz) == 1
                && ((hash2d(lx + dx, lz + dz, 0xA1B2C3) & 1) == 0)) continue;
            put_leaf(lx + dx, top, lz + dz);
        }
    }
    put_leaf(lx, top + 1, lz);
}

// Tall conifer: 7-tall trunk, pointy stepped canopy.
template <class Sink>
inline void stamp_conifer(Sink& c, int lx, int base_y, int lz) {
    constexpr int kTrunkH = 7;
    const int top = base_y + kTrunkH;

    for (int dy = 0; dy < kTrunkH; ++dy) {
        int y = base_y + dy;
        if (y >= 0 && y < kChunkSizeY) c.set(lx, y, lz, BlockId::Wood);
    }

    auto put_leaf = [&](int x, int y, int z) {
        if (!c.in_bounds(x, y, z)) return;
        if (is_solid(c.get(x, y, z))) return;
        c.set(x, y, z, BlockId::Leaves);
    };

    // Stepped triangular silhouette: wider near the base.
    for (int layer = 0; layer < 4; ++layer) {
        int y = base_y + 2 + layer * 2;
        int r = 2 - layer / 2;
        for (int dz = -r; dz <= r; ++dz) {
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) + std::abs(dz) > r + 1) continue;
                put_leaf(lx + dx, y, lz + dz);
            }
        }
    }
    put_leaf(lx, top + 1, lz);
}

// Small bush: 1-tall stem, one 3x3 leaf layer, one leaf above its centre.
template <class Sink>
inline void stamp_bush(Sink& c, int lx, int base_y, int lz) {
    if (c.in_bounds(lx, base_y, lz)) c.set(lx, base_y, lz, BlockId::Wood);

    auto put_leaf = [&](int x, int y, int z) {
        if (!c.in_bounds(x, y, z)) return;
        if (is_solid(c.get(x, y, z))) return;
        c.set(x, y, z, BlockId::Leaves);
    };

    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            put_leaf(lx + dx, base_y + 1, lz + dz);
            if (dx == 0 && dz == 0) put_leaf(lx + dx, base_y + 2, lz + dz);
        }
    }
}

}  // namespace world
