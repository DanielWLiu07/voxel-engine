#!/usr/bin/env bash
#
# Thread-scaling sweep for chunk generation + greedy meshing: run the
# headless --save path at each worker pool size and report chunks/sec, so
# the parallel-efficiency number is reproducible instead of a one-off.
#
# Usage:
#   scripts/bench_scaling.sh              # 1 2 4 6 9 workers, radius 12
#   scripts/bench_scaling.sh 1 3 9        # custom worker counts
#   RADIUS=8 scripts/bench_scaling.sh     # override world size

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

RADIUS="${RADIUS:-12}"

if [ "$#" -gt 0 ]; then
  THREADS=("$@")
else
  THREADS=(1 2 4 6 9)
fi

TMPDIR_SCALING="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_SCALING"' EXIT

echo "Scaling sweep: workers ${THREADS[*]}  radius: ${RADIUS}"
for T in "${THREADS[@]}"; do
  ./build/voxel_engine --save "$TMPDIR_SCALING/w$T" --radius "$RADIUS" \
      --threads "$T" \
    | grep 'chunks loaded in\|worker total'
  rm -rf "$TMPDIR_SCALING/w$T"
done
