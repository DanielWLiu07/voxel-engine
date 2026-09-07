#!/usr/bin/env bash
#
# Prove a multi-frame capture is a pure function of its pose.
#
# The repo's screenshots are treated as artifacts with the same rules as
# its benchmarks: the caption carries the command, and running the command
# has to give the image back. verify_occlusion.sh checks that for a SINGLE
# frame across two runs. Nothing checked it ACROSS FRAMES of one run, and
# that is where it was failing.
#
# --capture-walk ping-pongs the camera out and back, so frame 0 and the
# middle frame are taken at the same position, on the same cut, at the
# same time of day. They must be byte-identical. They were not: the
# shadow-cascade refresh is staggered by frame_index, which counts settle
# frames and convergence waits, so a captured frame's shadows depended on
# how long streaming happened to take. Two runs in three matched and the
# third did not, with nothing else changing - which is the worst shape a
# defect can have, because it looks like nothing.
#
# Usage:
#   scripts/verify_capture_stable.sh
#   RADIUS=8 SEED=777 scripts/verify_capture_stable.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

RADIUS="${RADIUS:-6}"
SEED="${SEED:-1337}"
POSE="${POSE:-40,60,-70,-150,-12}"

# Both cuts. A flat cut is the control - walking it changes nothing about
# the world - and a compound tilt is the case that actually exercises
# streaming, because there walking IS travel along w and every step makes
# chunks stale.
status=0
for CUT in "flat" "tilted"; do
  ARGS=(--4d --slice-prisms)
  if [ "$CUT" = "tilted" ]; then
    ARGS+=(--slice-tilt 0.45 --slice-tilt-xw 0.45)
  fi

  run_walk() {
    rm -rf capture
    VOXEL_CAPTURE_TRACE=1 ./build/voxel_engine --capture-walk 4 "${ARGS[@]}" \
        --pose-at "$POSE" --time-of-day 0.68 --radius "$RADIUS" \
        --seed "$SEED" 2>"$TRACE" >/dev/null
    shasum capture/frame_*.png | cut -d' ' -f1 | tr '\n' ' '
  }

  # Two checks, and the second is the one that bites reliably.
  #
  # Within a run, the ping-pong puts frames 0 and 2 at the same position,
  # so they must be byte-identical. That catches the defect only when the
  # frame counter's phase happens to differ between them - with the bug
  # reintroduced it fired on two runs in four, so on its own it is a gate
  # that passes half the time while broken.
  #
  # Across two runs, the WHOLE sequence must match. Any dependence on
  # anything but the pose - a frame counter, a settle length, a
  # convergence wait - shows up here every time, because the two runs
  # never take the same number of frames to get anywhere.
  TRACE=$(mktemp)
  SEQ_A=$(run_walk)
  # Every captured frame reports the world converged. This is the check
  # that it converged to a world with no chunk meshed against fewer
  # neighbours than sit beside it - invisible geometry that a dropped
  # re-mesh leaves behind.
  #
  # PROBABILISTIC, unlike the two byte-identity legs below, and worth
  # saying so rather than letting a green run be read as proof. The
  # defect it guards is a race: a dirty mark discarded because a job was
  # already in flight, where that job captured its neighbours before the
  # neighbour landed. Reintroducing it fails this leg on three runs in
  # six. The byte-identity legs catch the cascade defect 3 times in 3.
  SHORT=$(grep -o "short=[0-9]*" "$TRACE" | grep -v "short=0" | head -1 || true)
  SEQ_B=$(run_walk)
  if [ -z "$SHORT" ]; then
    echo "CAPTURE_STABLE cut=$CUT all_chunks_fully_meshed=1"
  else
    echo "CAPTURE_STABLE cut=$CUT all_chunks_fully_meshed=0 FAILED ($SHORT)"
    status=1
  fi
  rm -f "$TRACE"

  # shellcheck disable=SC2086
  set -- $SEQ_A
  if [ "$1" = "$3" ]; then
    echo "CAPTURE_STABLE cut=$CUT same_pose_identical=1"
  else
    echo "CAPTURE_STABLE cut=$CUT same_pose_identical=0 FAILED"
    echo "  frame 0: $1"
    echo "  frame 2: $3  (same pose, same cut, same hour)"
    status=1
  fi

  if [ "$SEQ_A" = "$SEQ_B" ]; then
    echo "CAPTURE_STABLE cut=$CUT run_to_run_identical=1 ok"
  else
    echo "CAPTURE_STABLE cut=$CUT run_to_run_identical=0 FAILED"
    echo "  run 1: $SEQ_A"
    echo "  run 2: $SEQ_B"
    status=1
  fi
done
rm -rf capture
exit $status
