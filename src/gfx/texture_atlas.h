#pragma once

#include <glad/gl.h>

namespace gfx {

// Block textures live in a GL_TEXTURE_2D_ARRAY: one 64x64 RGBA8 layer per
// tile id, GL_REPEAT wrap (face-unit UVs from the greedy mesher tile
// natively) and per-layer mipmaps (a packed 2D atlas can't mip without
// bleeding across tile borders). Layer index == tile id == BlockId for the
// base tiles; 8..10 are the per-face top variants.
inline constexpr int kAtlasTilePx = 64;
inline constexpr int kAtlasLayers = 16;
// Procedural paint patterns were authored on a 16px grid; painting them at
// this scale factor keeps their apparent feature size after the res bump.
inline constexpr int kAtlasLegacyTilePx = 16;

// Generate the atlas and return its GL texture handle. Uploaded as
// GL_RGBA8 with GL_NEAREST filtering and clamp-to-edge so the voxel
// aesthetic stays crisp at any zoom. Tiles default to procedural paint;
// textures/<block>.png overrides a tile when present (the AI-generated
// set - see TEXTURES.md). png_tiles_out, if non-null, receives how many
// tiles came from PNGs so the caller can show the AI-art credit.
GLuint generate_block_atlas(int* png_tiles_out = nullptr);

}  // namespace gfx
