#!/usr/bin/env bash
#
# Prove the occlusion culler is invisible: render the same deterministic
# pose with the section-graph BFS on and off and require the PNGs to be
# byte-identical. The cave pose is the strong case -- occlusion drops
# ~98% of frustum-visible sections there, and the image still may not
# change by a single byte.
#
# Usage:
#   scripts/verify_occlusion.sh                # center + cave poses
#   scripts/verify_occlusion.sh ground high    # custom pose list
#   SETTLE=90 scripts/verify_occlusion.sh      # more settle frames
#   RADIUS=8 SEED=777 scripts/verify_occlusion.sh   # different world

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

SETTLE="${SETTLE:-60}"
RADIUS="${RADIUS:-12}"
SEED="${SEED:-1337}"

if [ "$#" -gt 0 ]; then
  POSES=("$@")
else
  POSES=(center cave)
fi

fail=0
for P in "${POSES[@]}"; do
  ./build/voxel_engine --pose "$P" --radius "$RADIUS" --seed "$SEED" \
      --screenshot-after "$SETTLE" --shot-file "verify_${P}_on.png" \
    | grep '^\[screenshot\]'
  ./build/voxel_engine --pose "$P" --radius "$RADIUS" --seed "$SEED" \
      --screenshot-after "$SETTLE" --shot-file "verify_${P}_off.png" \
      --no-occlusion \
    | grep '^\[screenshot\]'
  if cmp -s "screenshots/verify_${P}_on.png" "screenshots/verify_${P}_off.png"; then
    echo "OCCLUSION_VERIFY pose=$P identical=1 ok"
  else
    echo "OCCLUSION_VERIFY pose=$P identical=0 FAILED" >&2
    fail=1
  fi
  rm -f "screenshots/verify_${P}_on.png" "screenshots/verify_${P}_off.png"
done

exit "$fail"
