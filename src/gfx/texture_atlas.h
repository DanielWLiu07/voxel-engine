#pragma once

#include <glad/gl.h>

namespace gfx {

// Block atlas layout: 4x4 grid of 16x16 tiles in a 64x64 RGBA8 image.
// Block id n lives at column (n & 3), row (n >> 2). Tile UV range per
// block: [n_col, n_col + 1) / 4  by  [n_row, n_row + 1) / 4.
inline constexpr int kAtlasTilePx   = 16;
inline constexpr int kAtlasTilesDim = 4;
inline constexpr int kAtlasSizePx   = kAtlasTilePx * kAtlasTilesDim;

// Generate the atlas and return its GL texture handle. Uploaded as
// GL_RGBA8 with GL_NEAREST filtering and clamp-to-edge so the voxel
// aesthetic stays crisp at any zoom. Tiles default to procedural paint;
// textures/<block>.png overrides a tile when present (the AI-generated
// set — see TEXTURES.md). png_tiles_out, if non-null, receives how many
// tiles came from PNGs so the caller can show the AI-art credit.
GLuint generate_block_atlas(int* png_tiles_out = nullptr);

}  // namespace gfx
