#!/usr/bin/env bash
#
# Sweep the frame bench across stream radii using the runtime --radius flag,
# so no recompile or source edit is needed and the CI-gated world is never
# touched.
#
# Usage:
#   scripts/bench_sweep.sh                          # 8 10 12 14 16, 300 frames, center
#   scripts/bench_sweep.sh 6 8 10                   # custom radii
#   POSES="center ground high" scripts/bench_sweep.sh   # multi-pose per radius
#   SAMPLES=600 scripts/bench_sweep.sh              # override frame count

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

SAMPLES="${SAMPLES:-300}"
POSES="${POSES:-center}"

if [ "$#" -gt 0 ]; then
  RADII=("$@")
else
  RADII=(8 10 12 14 16)
fi

echo "Sweeping radii: ${RADII[*]}  poses: ${POSES}  samples: ${SAMPLES}"
for R in "${RADII[@]}"; do
  for P in $POSES; do
    ./build/voxel_engine --radius "$R" --bench-frame "$SAMPLES" --pose "$P" \
      | grep '^BENCH_FRAME'
  done
done
