#!/usr/bin/env python3
"""Generate the AI block textures + provenance manifest.

Reproducible pipeline: SDXL-Turbo (fp16) with every Conv2d patched to
circular padding so outputs tile seamlessly. Each tile has a fixed prompt +
seed recorded in textures/MANIFEST.toml; re-running this script regenerates
byte-comparable art (same model revision + same seeds).

Run from the repo root:  python3 scripts/gen_textures.py [--tiles stone,dirt]

Outputs:
  textures/<name>.png        64x64 final tile (engine drop-in loader path)
  textures/preview/<name>_512.png   full-res output for inspection
  textures/preview/<name>_seam.png  50%-offset wrap for seam checking
  textures/MANIFEST.toml     per-file provenance (model, prompt, seed, ...)
"""

import argparse
import datetime
import hashlib
import sys
from pathlib import Path

import torch
from PIL import Image

MODEL_ID = "stabilityai/sdxl-turbo"
GEN_RES = 512
TILE_RES = 64
STEPS = 4
GUIDANCE = 0.0  # turbo is distilled for guidance-free sampling

STYLE = ("seamless tileable game texture, top-down flat view, "
         "stylized pixel art, even diffuse lighting, no shadows, "
         "no border, high detail")

TILES = {
    # name: (seed, subject prompt)
    "stone":  (101, f"rough grey granite stone surface, {STYLE}"),
    "dirt":   (102, f"brown soil dirt ground with small pebbles, {STYLE}"),
    "grass":  (103, f"dense green grass lawn seen from directly above, {STYLE}"),
    "sand":   (104, f"fine pale desert sand with gentle ripples, {STYLE}"),
    "wood":   (105, f"vertical oak wood bark planks, {STYLE}"),
    "leaves": (106, f"dense dark green leafy foliage canopy, {STYLE}"),
    "snow":   (107, f"clean white snow surface with subtle sparkle, {STYLE}"),
}


def patch_circular(module: torch.nn.Module) -> int:
    n = 0
    for m in module.modules():
        if isinstance(m, torch.nn.Conv2d):
            m.padding_mode = "circular"
            n += 1
    return n


def seam_score(img: Image.Image) -> float:
    """Mean abs diff across the wrap seam relative to interior texture.
    < ~1.5 means the seam is indistinguishable from normal texture detail."""
    import numpy as np
    a = np.asarray(img.convert("L"), dtype=np.float32)
    h, w = a.shape
    seam_v = np.abs(a[:, 0] - a[:, -1]).mean()
    seam_h = np.abs(a[0, :] - a[-1, :]).mean()
    interior = (np.abs(np.diff(a, axis=1)).mean() +
                np.abs(np.diff(a, axis=0)).mean()) / 2.0
    return max(seam_v, seam_h) / max(interior, 1e-6)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiles", default=",".join(TILES),
                    help="comma-separated subset of tile names")
    args = ap.parse_args()
    wanted = [t.strip() for t in args.tiles.split(",") if t.strip()]

    out_dir = Path("textures")
    prev_dir = out_dir / "preview"
    prev_dir.mkdir(parents=True, exist_ok=True)

    from diffusers import AutoPipelineForText2Image
    pipe = AutoPipelineForText2Image.from_pretrained(
        MODEL_ID, torch_dtype=torch.float16, variant="fp16")
    pipe = pipe.to("mps")
    patched = patch_circular(pipe.unet) + patch_circular(pipe.vae)
    print(f"[gen] {MODEL_ID} on mps, {patched} convs -> circular padding")

    manifest_rows = []
    for name in wanted:
        seed, prompt = TILES[name]
        gen = torch.Generator("cpu").manual_seed(seed)
        img = pipe(prompt=prompt, num_inference_steps=STEPS,
                   guidance_scale=GUIDANCE, width=GEN_RES, height=GEN_RES,
                   generator=gen).images[0]

        img.save(prev_dir / f"{name}_512.png")
        # 50% roll in both axes: any seam lands dead-center for inspection.
        import numpy as np
        rolled = Image.fromarray(
            np.roll(np.roll(np.asarray(img), GEN_RES // 2, 0), GEN_RES // 2, 1))
        rolled.save(prev_dir / f"{name}_seam.png")
        score = seam_score(img)

        tile = img.resize((TILE_RES, TILE_RES), Image.LANCZOS)
        tile_path = out_dir / f"{name}.png"
        tile.save(tile_path)
        sha = hashlib.sha256(tile_path.read_bytes()).hexdigest()[:16]

        status = "ok" if score < 1.5 else "CHECK SEAM"
        print(f"[gen] {name:<7} seed={seed} seam_ratio={score:.2f} ({status})")
        manifest_rows.append((name, seed, prompt, sha, score))

    today = datetime.date.today().isoformat()
    with open(out_dir / "MANIFEST.toml", "w") as f:
        f.write("# AI-generated texture provenance. Regenerate with\n"
                "#   python3 scripts/gen_textures.py\n"
                "# These assets are AI-generated and labeled as such on\n"
                "# purpose - see TEXTURES.md.\n\n")
        for name, seed, prompt, sha, score in manifest_rows:
            f.write(f"[{name}]\n")
            f.write(f'file = "textures/{name}.png"\n')
            f.write(f'model = "{MODEL_ID}"\n')
            f.write(f'prompt = "{prompt}"\n')
            f.write(f"seed = {seed}\n")
            f.write(f"steps = {STEPS}\n")
            f.write(f"guidance = {GUIDANCE}\n")
            f.write(f'generated = "{today}"\n')
            f.write(f'pipeline = "circular-conv padding, {GEN_RES}px -> '
                    f'LANCZOS {TILE_RES}px"\n')
            f.write(f'sha256_16 = "{sha}"\n')
            f.write(f"seam_ratio = {score:.3f}\n\n")
    print(f"[gen] wrote textures/MANIFEST.toml ({len(manifest_rows)} tiles)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
