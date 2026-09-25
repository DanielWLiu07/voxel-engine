#!/usr/bin/env bash
# Regenerates a README clip from a live engine run.
#
#   ./scripts/capture_clip.sh orbit [frames] [out.gif]
#   ./scripts/capture_clip.sh cycle [frames] [out.gif]
#   ./scripts/capture_clip.sh tilt  [frames] [out.gif]
#   ./scripts/capture_clip.sh tiltxw [frames] [out.gif]
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
# The tilt, tiltxw and walk clips rebuild the whole chunk window between
# frames rather than riding a streaming budget - a frame has to be a pure
# function of its cut or the loop cannot close - so each frame costs a
# second or two instead of a vsync. 90 frames is a nine-second loop at
# the 10 fps these encode to, and takes about three minutes end to end.
case "${1:-orbit}" in
  tilt|tiltxw|walk) FRAMES=${2:-90} ;;
  *)         FRAMES=${2:-360} ;;
esac
case "$MODE" in
  orbit) OUT=${3:-docs/media/orbit.gif} ;;
  cycle) OUT=${3:-docs/media/daycycle.gif} ;;
  tilt)  OUT=${3:-docs/media/slice_tilt.gif} ;;
  tiltxw) OUT=${3:-docs/media/cells_turn.gif} ;;
  walk)  OUT=${3:-docs/media/slice_walk.gif} ;;
  *) echo "usage: $0 orbit|cycle|tilt|tiltxw|walk [frames] [out.gif]" >&2; exit 1 ;;
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
if [ "$MODE" = "tiltxw" ]; then
  # The one clip where the BLOCK SHAPES change rather than the terrain.
  #
  # ZW is held at 0.45 and the XW plane sweeps. A cut turned in one plane
  # alone presents four-sided cells at every angle - a cube is exact
  # there - so the second plane is what makes them pentagons and
  # hexagons. --slice-prisms is not optional for this clip: with cubes
  # drawn instead there is nothing to see, because cubes are what the
  # clip exists to disprove.
  #
  # Close to the ground, where every other clip is elevated: this one is
  # about the CORNERS of individual blocks, and from the usual vantage a
  # block is a few pixels and the only legible change is the terrain
  # reorganising - which is what slice_tilt.gif already shows. Same pose
  # as the blocks_cube/blocks_prism stills for that reason. Daylight for
  # the same reason: at the sunset the other clips use, the faces go to
  # silhouette and an obtuse corner reads the same as a right one.
  POSE_ARGS=(--pose-at "${CLIP_CELLS_POSE:-22,44,26,-125,-14}"
             --radius "${CLIP_CELLS_RADIUS:-8}"
             --slice-prisms
             --slice-tilt "${CLIP_CELLS_TILT:-0.45}"
             --slice-tilt-xw "${CLIP_CELLS_TILT_XW:-0.45}")
fi
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
# The mode name and the flag are not always the same word: the XW sweep
# reads as "tiltxw" on this command line and as --capture-tilt-xw on the
# engine's, where it sits beside --slice-tilt-xw.
CAPTURE_FLAG="--capture-$MODE"
if [ "$MODE" = "tiltxw" ]; then CAPTURE_FLAG="--capture-tilt-xw"; fi

./build/voxel_engine "$CAPTURE_FLAG" "$FRAMES" \
    ${CENTER_ARGS[@]+"${CENTER_ARGS[@]}"} ${TOD_ARGS[@]+"${TOD_ARGS[@]}"} \
    ${POSE_ARGS[@]+"${POSE_ARGS[@]}"}

# Every converging clip is a PING-PONG, so frame 0 and frame N/2 are the
# same cut seen from the same camera and have to render to the same image.
# That is a real invariant and it is cheap to check, so it is checked here
# rather than trusted - the clip that motivated it looked entirely
# plausible while being wrong.
#
# It went wrong this way: the capture used to converge the chunk window in
# the block that writes the PNG, which runs after the draw, so every saved
# frame showed the previous frame's cut. The images still swept, the trace
# still printed the right angles, and the GIF still looped - it just was
# not a film of the angles it said it was. Measured, frames 0 and N/2 came
# out 28/255 apart while consecutive frames were 0.02/255 apart, which is
# the signature: the sweep lands in the file one frame late.
#
# Not a ctest case because it needs a window and a GPU and CI has neither.
# Here is the next best place: it runs every time a clip is made, which is
# the only time the answer can change.
case "$MODE" in
  tilt|tiltxw|walk)
    HALF=$((FRAMES / 2))
    if [ $((FRAMES % 2)) -eq 0 ] && [ "$HALF" -gt 0 ]; then
      A=$(printf 'capture/frame_%04d.png' 0)
      B=$(printf 'capture/frame_%04d.png' "$HALF")
      ffmpeg -v error -i "$A" -pix_fmt gray -f rawvideo /tmp/clip_a.raw -y
      ffmpeg -v error -i "$B" -pix_fmt gray -f rawvideo /tmp/clip_b.raw -y
      python3 - "$HALF" <<'EOF'
import sys
a = open("/tmp/clip_a.raw", "rb").read()
b = open("/tmp/clip_b.raw", "rb").read()
if len(a) != len(b):
    sys.exit("loop check: frame sizes differ")
d = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
half = sys.argv[1]
# 0.02/255 measured when correct, 28/255 when the convergence ran after
# the draw. Anything under 2 is the residual of atmosphere that
# integrates real wall-clock dt and does not repeat frame for frame.
verdict = "ok" if d < 2.0 else "FAIL"
print("loop check: frame 0 vs frame %s differ by %.2f/255 - %s"
      % (half, d, verdict))
if d >= 2.0:
    sys.exit("the ping-pong does not close: frames at the same cut are "
             "different images, so the clip is not a film of the angles "
             "it reports. Check that the capture converges BEFORE it "
             "draws.")
EOF
    fi
    ;;
esac

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
