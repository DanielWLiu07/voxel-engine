#!/usr/bin/env bash
#
# Sweeps --bench-frame across kStreamRadius values and optionally a list of
# camera poses per radius. Prints one BENCH_FRAME row per (radius, pose)
# combination. Each radius rebuilds once; poses share the build because
# they only change the camera at runtime. On any exit path, kStreamRadius
# is restored to the original value (and the binary rebuilt with it) so
# the CI cull-bench gates keep measuring the same world.
#
# Usage:
#   scripts/bench_sweep.sh                          # 8 10 12 14 16, 300 frames, pose=center
#   scripts/bench_sweep.sh 6 8 10                   # custom radii
#   POSES="center ground high" scripts/bench_sweep.sh   # multi-pose per radius
#   SAMPLES=600 scripts/bench_sweep.sh              # override frame count
#
# Run from the repo root. Requires a prior cmake configure (cmake -B build).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f src/main.cpp ] || [ ! -d build ]; then
  echo "Run from repo root; build/ must exist (cmake -B build first)." >&2
  exit 1
fi

SAMPLES="${SAMPLES:-300}"
POSES="${POSES:-center}"

if [ "$#" -gt 0 ]; then
  RADII=("$@")
else
  RADII=(8 10 12 14 16)
fi

# Capture the current kStreamRadius so we can restore it at the end.
ORIGINAL_RADIUS=$(grep -oE 'constexpr int   kStreamRadius   = [0-9]+;' src/main.cpp \
                    | grep -oE '[0-9]+' | head -1)
if [ -z "$ORIGINAL_RADIUS" ]; then
  echo "Could not find kStreamRadius in src/main.cpp." >&2
  exit 1
fi

restore_radius() {
  sed -i.bak -E \
    "s/constexpr int   kStreamRadius   = [0-9]+;/constexpr int   kStreamRadius   = ${ORIGINAL_RADIUS};/" \
    src/main.cpp
  rm -f src/main.cpp.bak
  cmake --build build -j >/dev/null
}
trap restore_radius EXIT

echo "Sweeping radii: ${RADII[*]}  poses: ${POSES}  samples: ${SAMPLES}"
for R in "${RADII[@]}"; do
  sed -i.bak -E \
    "s/constexpr int   kStreamRadius   = [0-9]+;/constexpr int   kStreamRadius   = ${R};/" \
    src/main.cpp
  rm -f src/main.cpp.bak
  cmake --build build -j >/dev/null
  for P in $POSES; do
    ./build/voxel_engine --bench-frame "$SAMPLES" --pose "$P" | grep '^BENCH_FRAME'
  done
done
