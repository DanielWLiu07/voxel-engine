#!/usr/bin/env bash
#
# Runs the frame benchmark R times and reports the run-to-run distribution
# (mean, stddev, min, max, coefficient of variation) for avg frame time and
# fps. Frame time is machine-load dependent, so a single number is misleading;
# this quantifies the noise so a quoted figure can be defended ("5.7 ms,
# CV 4%" rather than a lone "5.7 ms"). The deterministic counts (sections,
# triangles) do not vary run to run and are gated separately by `--bench`.
#
# Usage:
#   scripts/bench_variance.sh                 # 10 runs, 300 frames, center pose
#   scripts/bench_variance.sh 20 600 ground   # runs / frames-per-run / pose
#
# Run from the repo root after a Release build (cmake --build build).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

RUNS="${1:-10}"
FRAMES="${2:-300}"
POSE="${3:-center}"
BIN=./build/voxel_engine

if [ ! -x "$BIN" ]; then
  echo "Build first: cmake --build build  (need $BIN)" >&2
  exit 1
fi

echo "bench_variance: $RUNS runs x $FRAMES frames, pose=$POSE"
echo "run     avg_ms   avg_fps"

ms_samples=()
fps_samples=()
for r in $(seq 1 "$RUNS"); do
  line=$("$BIN" --bench-frame "$FRAMES" --pose "$POSE" | grep '^BENCH_FRAME')
  ms=$(echo "$line"  | grep -oE 'avg_ms=[0-9.]+'  | cut -d= -f2)
  fps=$(echo "$line" | grep -oE 'avg_fps=[0-9.]+' | cut -d= -f2)
  printf "%3d  %9s %9s\n" "$r" "$ms" "$fps"
  ms_samples+=("$ms")
  fps_samples+=("$fps")
done

stats() {
  # prints: mean stddev min max cv%   from stdin (one number per line)
  awk '
    { x[NR]=$1; sum+=$1; if (NR==1||$1<mn) mn=$1; if (NR==1||$1>mx) mx=$1 }
    END {
      n=NR; mean=sum/n;
      for (i=1;i<=n;i++) ss+=(x[i]-mean)*(x[i]-mean);
      sd=(n>1)?sqrt(ss/(n-1)):0;
      cv=(mean!=0)?100*sd/mean:0;
      printf "  mean=%.3f  stddev=%.3f  min=%.3f  max=%.3f  cv=%.1f%%\n", mean, sd, mn, mx, cv;
    }'
}

echo ""
echo "avg_ms  distribution:"
printf "%s\n" "${ms_samples[@]}" | stats
echo "avg_fps distribution:"
printf "%s\n" "${fps_samples[@]}" | stats
