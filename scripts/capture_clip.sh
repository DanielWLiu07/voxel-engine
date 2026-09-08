#!/usr/bin/env bash
# Regenerates a README clip from a live engine run.
#
#   ./scripts/capture_clip.sh orbit [frames] [out.gif]
#   ./scripts/capture_clip.sh cycle [frames] [out.gif]
#   ./scripts/capture_clip.sh tilt  [frames] [out.gif]
#
# orbit flies one deterministic camera circle; cycle holds the camera and
# runs one full day of time-of-day; tilt holds the camera and turns the 4D
# slice instead - the only clip where the world moves and the viewer does
# not. Frames land in ./capture, then ffmpeg
# assembles a palette-optimized looping GIF sized for GitHub's README
# renderer (it must stay under roughly 10 MB to display inline). Both
# modes cover one full period with frozen extras, so the last frame meets
# the first and the loop is seamless.

set -euo pipefail

MODE=${1:-orbit}
# The tilt clip converges the whole window between frames rather than
# riding a streaming budget, so each frame costs about a second. 90 is a
# three-second loop at 30 fps and takes a couple of minutes to capture.
# The tilt and walk clips converge the whole window between frames rather
# than riding a streaming budget, so each frame costs about a second.
case "${1:-orbit}" in
  tilt|walk) FRAMES=${2:-90} ;;
  *)         FRAMES=${2:-360} ;;
esac
case "$MODE" in
  orbit) OUT=${3:-docs/media/orbit.gif} ;;
  cycle) OUT=${3:-docs/media/daycycle.gif} ;;
  tilt)  OUT=${3:-docs/media/slice_tilt.gif} ;;
  walk)  OUT=${3:-docs/media/slice_walk.gif} ;;
  *) echo "usage: $0 orbit|cycle|tilt|walk [frames] [out.gif]" >&2; exit 1 ;;
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
# The procedural sky put clouds and stars in every frame, which is a lot
# more entropy for a palette to carry: at the old 560px/12fps/128 colors
# the orbit clip encoded to 10 MB and GitHub stops rendering a GIF inline
# somewhere around there. Slightly smaller and slightly fewer colors puts
# it back under 7 MB with no visible loss at README width.
WIDTH=${CLIP_GIF_WIDTH:-520}
FPS=${CLIP_GIF_FPS:-10}
COLORS=${CLIP_GIF_COLORS:-96}

rm -rf capture
# ${ARR[@]+"${ARR[@]}"} rather than "${ARR[@]}": macOS ships bash 3.2, where
# expanding an EMPTY array under `set -u` is an unbound-variable error.
# The tilt clip needs a pose: the orbit start looks across the spawn
# triple point, which is where the biome variety is, and a rotation is
# only legible against terrain that has something in it.
POSE_ARGS=()
if [ "$MODE" = "walk" ]; then
  # The complement of the tilt clip: the cut is HELD and the camera walks.
  # On a flat cut that shows only parallax; the tilt is what makes it 4D
  # travel, because the slice's own z axis leans into w. Tilted by default
  # for that reason - CLIP_WALK_TILT=0 gives the control, where the same
  # walk changes nothing about the world.
  POSE_ARGS=(--pose-at "${CLIP_WALK_POSE:-40,60,-70,-150,-12}"
             --radius "${CLIP_WALK_RADIUS:-10}"
             --slice-prisms
             --slice-tilt "${CLIP_WALK_TILT:-0.45}"
             --slice-tilt-xw "${CLIP_WALK_TILT_XW:-0.45}")
fi
if [ "$MODE" = "tilt" ]; then
  # An elevated mid-distance vantage, not a close one. The cut turns
  # about the VIEWER, so the ground underfoot is nearly still whatever the
  # wheel does and the change lives 100-190 blocks out - a pose looking
  # down at its own feet shows almost nothing happening.
  POSE_ARGS=(--pose-at "${CLIP_TILT_POSE:-20,105,20,-115,-26}"
             --radius "${CLIP_TILT_RADIUS:-12}"
             --slice-tilt "${CLIP_TILT_AMPLITUDE:-0.15}")
fi
./build/voxel_engine "--capture-$MODE" "$FRAMES" \
    ${CENTER_ARGS[@]+"${CENTER_ARGS[@]}"} ${TOD_ARGS[@]+"${TOD_ARGS[@]}"} \
    ${POSE_ARGS[@]+"${POSE_ARGS[@]}"}

# Two-pass palette assembly: a shared palette across the whole clip avoids
# per-frame palette flicker, and lanczos keeps block edges crisp at README
# width. The capture is 30 fps worth of orbit steps; the fps filter drops
# to the target rate evenly.
ffmpeg -y -framerate 30 -i capture/frame_%04d.png \
    -vf "fps=$FPS,scale=$WIDTH:-1:flags=lanczos,palettegen=max_colors=$COLORS" \
    /tmp/clip_palette.png
ffmpeg -y -framerate 30 -i capture/frame_%04d.png -i /tmp/clip_palette.png \
    -lavfi "fps=$FPS,scale=$WIDTH:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=5" \
    "$OUT"

ls -lh "$OUT"
