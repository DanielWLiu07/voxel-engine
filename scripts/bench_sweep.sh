#!/usr/bin/env bash
#
# Sweeps --bench-frame across a range of kStreamRadius values and prints one
# BENCH_FRAME row per radius. Each iteration edits src/main.cpp in place,
# rebuilds, runs the bench, and (when the loop completes) restores
# kStreamRadius to the original value so the CI cull-bench gates keep
# measuring the same world.
#
# Usage:
#   scripts/bench_sweep.sh                # default: 8 10 12 14 16, 300 frames each
#   scripts/bench_sweep.sh 6 8 10         # custom radii (300 frames each)
#   SAMPLES=600 scripts/bench_sweep.sh    # custom frame count
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

echo "Sweeping radii: ${RADII[*]} (samples=${SAMPLES})"
for R in "${RADII[@]}"; do
  sed -i.bak -E \
    "s/constexpr int   kStreamRadius   = [0-9]+;/constexpr int   kStreamRadius   = ${R};/" \
    src/main.cpp
  rm -f src/main.cpp.bak
  cmake --build build -j >/dev/null
  ./build/voxel_engine --bench-frame "$SAMPLES" | grep '^BENCH_FRAME'
done
