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
# CLIP_ORBIT_CENTER="x,z[,look_y]" recenters the orbit (the lake clip uses
# 288,-400,30); unset keeps the spawn triple-point circle.
CENTER_ARGS=()
# CLIP_TIME_OF_DAY pins the sun for the clip (0.25 sunrise, 0.5 noon,
# 0.75 sunset). Without it the engine's default mid-morning sun applies,
# which is the one angle that flattens the shadows.
TOD_ARGS=()
if [ -n "${CLIP_TIME_OF_DAY:-}" ]; then
  TOD_ARGS=(--time-of-day "$CLIP_TIME_OF_DAY")
fi
if [ -n "${CLIP_ORBIT_CENTER:-}" ]; then
  CENTER_ARGS=(--orbit-center "$CLIP_ORBIT_CENTER")
fi
WIDTH=${CLIP_GIF_WIDTH:-560}
FPS=${CLIP_GIF_FPS:-12}

rm -rf capture
# ${ARR[@]+"${ARR[@]}"} rather than "${ARR[@]}": macOS ships bash 3.2, where
# expanding an EMPTY array under `set -u` is an unbound-variable error.
./build/voxel_engine "--capture-$MODE" "$FRAMES" \
    ${CENTER_ARGS[@]+"${CENTER_ARGS[@]}"} ${TOD_ARGS[@]+"${TOD_ARGS[@]}"}

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
