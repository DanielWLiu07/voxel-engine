#!/usr/bin/env bash
# The tilt triptych: one camera, one seed, one w, three slice orientations.
#
# The point of the images is that nothing moves except the angle of the
# cut. Pose, seed, time of day and w are all pinned, so any difference
# between the three frames is the fourth dimension and nothing else.
#
#     scripts/capture_tilt.sh              # -> docs/media/tilt_*.jpg
set -euo pipefail
cd "$(dirname "$0")/.."

POSE=${TILT_POSE:-20,105,20,-115,-26}
SEED=${TILT_SEED:-1337}
TOD=${TILT_TOD:-0.62}
RADIUS=${TILT_RADIUS:-12}
# 140 frames, not the captures' usual 60: a 4D world streams its slice in
# continuously, and at 60 the far chunks were still arriving.
#
# An elevated MID-DISTANCE vantage, and the choice is forced rather than
# aesthetic: the cut turns about the viewer, so the ground underfoot
# barely moves however far the wheel goes and the change lives 100-190
# blocks out. The first version of this script looked down at its own
# feet from 115 blocks up and the three frames were nearly identical.
#
# The sun is at 0.62 rather than the usual low 0.76. Both are deliberate.
# These images exist to be COMPARED, so the terrain has to be legible
# rather than atmospheric: at 0.76 with a radius-8 window the distance
# fog sat right on top of the geometry and all three frames were the same
# wash of pink. Radius 12 pushes the fog back behind the terrain.
FRAMES=${TILT_FRAMES:-140}

for t in 0.00 0.075 0.15; do
  ./build/voxel_engine --4d --seed "$SEED" --radius "$RADIUS" \
      --slice-tilt "$t" --pose-at "$POSE" --time-of-day "$TOD" \
      --screenshot-after "$FRAMES" --shot-file "tilt_$t.png" >/dev/null
  sips -s format jpeg -s formatOptions 72 -Z 1400 \
      "screenshots/tilt_$t.png" --out "docs/media/tilt_$t.jpg" >/dev/null
  printf '  docs/media/tilt_%s.jpg  %s\n' "$t" \
      "$(du -h "docs/media/tilt_$t.jpg" | cut -f1)"
done
