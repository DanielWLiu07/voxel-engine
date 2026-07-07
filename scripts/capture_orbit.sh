#!/usr/bin/env bash
# Regenerates the README orbit clip from a live engine run.
#
#   ./scripts/capture_orbit.sh [frames] [out.gif]
#
# Captures one full deterministic camera orbit (frames land in ./capture),
# then assembles a palette-optimized looping GIF sized for GitHub's README
# renderer (it must stay under roughly 10 MB to display inline). The orbit
# is a fixed-step circle with time-of-day paused, so the last frame meets
# the first and the loop is seamless.

set -euo pipefail

FRAMES=${1:-360}
OUT=${2:-docs/media/orbit.gif}
WIDTH=${ORBIT_GIF_WIDTH:-560}
FPS=${ORBIT_GIF_FPS:-12}

rm -rf capture
./build/voxel_engine --capture-orbit "$FRAMES"

# Two-pass palette assembly: a shared palette across the whole clip avoids
# per-frame palette flicker, and lanczos keeps block edges crisp at README
# width. The capture is 30 fps worth of orbit steps; the fps filter drops
# to the target rate evenly.
ffmpeg -y -framerate 30 -i capture/frame_%04d.png \
    -vf "fps=$FPS,scale=$WIDTH:-1:flags=lanczos,palettegen=max_colors=128" \
    /tmp/orbit_palette.png
ffmpeg -y -framerate 30 -i capture/frame_%04d.png -i /tmp/orbit_palette.png \
    -lavfi "fps=$FPS,scale=$WIDTH:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
    "$OUT"

ls -lh "$OUT"
