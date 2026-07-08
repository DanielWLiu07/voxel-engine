#!/usr/bin/env bash
# Regenerates a README clip from a live engine run.
#
#   ./scripts/capture_clip.sh orbit [frames] [out.gif]
#   ./scripts/capture_clip.sh cycle [frames] [out.gif]
#
# orbit flies one deterministic camera circle; cycle holds the camera and
# runs one full day of time-of-day. Frames land in ./capture, then ffmpeg
# assembles a palette-optimized looping GIF sized for GitHub's README
# renderer (it must stay under roughly 10 MB to display inline). Both
# modes cover one full period with frozen extras, so the last frame meets
# the first and the loop is seamless.

set -euo pipefail

MODE=${1:-orbit}
FRAMES=${2:-360}
case "$MODE" in
  orbit) OUT=${3:-docs/media/orbit.gif} ;;
  cycle) OUT=${3:-docs/media/daycycle.gif} ;;
  *) echo "usage: $0 orbit|cycle [frames] [out.gif]" >&2; exit 1 ;;
esac
WIDTH=${CLIP_GIF_WIDTH:-560}
FPS=${CLIP_GIF_FPS:-12}

rm -rf capture
./build/voxel_engine "--capture-$MODE" "$FRAMES"

# Two-pass palette assembly: a shared palette across the whole clip avoids
# per-frame palette flicker, and lanczos keeps block edges crisp at README
# width. The capture is 30 fps worth of orbit steps; the fps filter drops
# to the target rate evenly.
ffmpeg -y -framerate 30 -i capture/frame_%04d.png \
    -vf "fps=$FPS,scale=$WIDTH:-1:flags=lanczos,palettegen=max_colors=128" \
    /tmp/clip_palette.png
ffmpeg -y -framerate 30 -i capture/frame_%04d.png -i /tmp/clip_palette.png \
    -lavfi "fps=$FPS,scale=$WIDTH:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
    "$OUT"

ls -lh "$OUT"
