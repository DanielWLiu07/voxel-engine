# Block textures — AI-generated, and labeled as such

The block textures in `textures/` are **AI-generated** with
[SDXL-Turbo](https://huggingface.co/stabilityai/sdxl-turbo) running locally
(fp16, Apple MPS). I'm stating that up front rather than passing them off as
hand-made pixel art: the engine is the work here, the tile art is not.

The engine also signals this at runtime: the boot log and the debug HUD both
show "Block textures: AI-generated (SDXL-Turbo) — see TEXTURES.md" whenever
PNG tiles from `textures/` are in use. Delete the PNGs and the engine falls
back to its built-in procedural tiles (and the credit disappears).

## Provenance

Every file's exact generation parameters live in
[`textures/MANIFEST.toml`](textures/MANIFEST.toml): model, full prompt, seed,
step count, post-processing chain, generation date, and a content hash.

## Reproducing the set

```
pip install torch diffusers transformers accelerate
python3 scripts/gen_textures.py
```

The pipeline patches every Conv2d in the UNet/VAE to circular padding so the
outputs tile seamlessly, generates at 512 px with fixed per-tile seeds,
verifies the seam (each tile is rolled by 50% in both axes; the wrap edge
must be statistically indistinguishable from interior texture), and
downsamples to the shipped 64 px tiles. `textures/preview/` holds the
full-resolution outputs and the rolled seam-check images.

## Scope

AI tiles cover the seven base blocks (stone, dirt, grass, sand, wood,
leaves, snow). The per-face variant tiles (grass top, wood end-grain, snow
top) are still the engine's procedural paint until the texture-array
migration raises tile resolution.
