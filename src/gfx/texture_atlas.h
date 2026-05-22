#pragma once

#include <glad/gl.h>

namespace gfx {

// Block atlas layout: 4x4 grid of 16x16 tiles in a 64x64 RGBA8 image.
// Block id n lives at column (n & 3), row (n >> 2). Tile UV range per
// block: [n_col, n_col + 1) / 4  by  [n_row, n_row + 1) / 4.
inline constexpr int kAtlasTilePx   = 16;
inline constexpr int kAtlasTilesDim = 4;
inline constexpr int kAtlasSizePx   = kAtlasTilePx * kAtlasTilesDim;

// Generate a procedural atlas and return its GL texture handle.
// Uploaded as GL_RGBA8 with GL_NEAREST filtering and clamp-to-edge so
// the voxel aesthetic stays crisp at any zoom.
GLuint generate_block_atlas();

}  // namespace gfx
