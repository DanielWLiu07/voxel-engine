# Block textures

The engine generates a procedural texture atlas at boot. To override any
block with a higher-quality (AI-generated or hand-drawn) image, drop a
PNG into `./textures/` with the right filename - the loader picks it up
automatically and falls back to the procedural version for any missing
files.

## File layout

```
voxel-engine/
  textures/
    stone.png
    dirt.png
    grass.png
    sand.png
    wood.png
    leaves.png
    snow.png
```

Each file is one block face. Recommended size: **16x16 pixels** (the
classic voxel aesthetic). Larger sizes work; the loader nearest-neighbor
downsamples to 16x16 to match the atlas tile size. RGBA or RGB both fine.

The loader prints `[atlas] loaded N/7 block textures from textures/` at
startup so you can confirm which files were picked up.

## Generating textures with AI

These prompts are tuned for the engine's voxel aesthetic. They produce
seamless tileable square textures that read well at 16x16.

### Stable Diffusion / DALL-E 3 / Midjourney

For each block, use:

**Stone**:
> A seamless tileable game texture of gray rough stone with subtle cracks
> and small embedded darker pebbles, top-down view, pixel art style, 16x16
> pixels, flat lighting, no shading, square crop

**Dirt**:
> A seamless tileable game texture of rich brown soil with small pebbles
> and root fragments, top-down view, pixel art style, 16x16 pixels, flat
> lighting, no shading, square crop

**Grass**:
> A seamless tileable game texture of short green grass with a few darker
> tufts and tiny yellow flowers, top-down view, pixel art style, 16x16
> pixels, flat lighting, no shading, square crop

**Sand**:
> A seamless tileable game texture of pale yellow desert sand with fine
> granular detail, top-down view, pixel art style, 16x16 pixels, flat
> lighting, no shading, square crop

**Wood**:
> A seamless tileable game texture of brown tree bark with vertical grain
> lines and slight ridges, side view, pixel art style, 16x16 pixels, flat
> lighting, no shading, square crop

**Leaves**:
> A seamless tileable game texture of dense dark green oak leaves with
> small gaps and a few brighter highlights, pixel art style, 16x16 pixels,
> flat lighting, no shading, square crop

**Snow**:
> A seamless tileable game texture of fresh white snow with a faint blue
> cast and a few sparkle highlights, top-down view, pixel art style, 16x16
> pixels, flat lighting, no shading, square crop

### Tips that improve results

- Always include `seamless tileable` so edges match when the texture
  repeats across a merged greedy quad.
- `flat lighting, no shading` matters because the engine does its own
  Phong-style shading + vertex AO. A pre-shaded texture will look
  doubly-shaded in game.
- `pixel art style, 16x16 pixels` keeps the AI from producing
  high-detail textures that lose all their character when downsampled.
- `square crop` reminds the generator not to add letterboxing.

### Post-processing

After generating, if the texture is larger than 16x16:

- Open in any image editor that supports nearest-neighbor downsampling
  (Aseprite, GIMP "scale image" with "none" interpolation, Photoshop with
  "nearest neighbor")
- Resize to 16x16
- Export as PNG

The loader will accept any size and downsample, but doing the resample
yourself with proper pixel-art tooling gives noticeably better results
than the nearest-neighbor downsample in the engine.

## Going back to procedural

To revert any block to the engine's procedural texture, delete the PNG
(or rename it to `stone.png.bak`, etc.). The atlas regenerates at every
boot.

## Why this is the right abstraction

The procedural fallback means the engine has zero hard texture
dependencies - anyone can clone the repo and run it without needing
the textures. The PNG override path means an artist (or AI) can ship
better-looking content without touching any C++ code.
