#!/usr/bin/env bash
#
# End-to-end check of the chunk-format-v3 persistence contract:
#   1. Save a world; its manifest must record the seed.
#   2. Reload with the SAME seed, edit one block, stream away and back:
#      exactly ONE chunk (the edit) may stash on eviction - everything
#      else is regenerable and must not bloat the stash.
#   3. Reload with a DIFFERENT seed: nothing is regenerable, so every
#      loaded chunk must be conservatively preserved, and the edit must
#      still survive.
#
# Usage: scripts/verify_persistence.sh   (builds expected in ./build)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

RADIUS=6
SEED=42
CHUNKS=$(( (2 * RADIUS + 1) * (2 * RADIUS + 1) ))
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT

./build/voxel_engine --save "$DIR" --seed "$SEED" --radius "$RADIUS" >/dev/null 2>&1

if ! grep -q "^seed $SEED$" "$DIR/world.manifest"; then
  echo "PERSIST_VERIFY manifest missing or wrong: $(cat "$DIR/world.manifest" 2>&1)" >&2
  exit 1
fi

run_case() {
  local seed="$1" want_stashed="$2" label="$3"
  local line
  line=$(./build/voxel_engine --load "$DIR" --seed "$seed" --radius "$RADIUS" \
           --verify-edit-persistence 2>/dev/null | grep '^EDIT_PERSIST') || {
    echo "PERSIST_VERIFY $label: no EDIT_PERSIST line" >&2; exit 1; }
  case "$line" in
    *"stashed=$want_stashed "*" ok") ;;
    *) echo "PERSIST_VERIFY $label FAILED: $line (wanted stashed=$want_stashed ... ok)" >&2
       exit 1 ;;
  esac
  echo "PERSIST_VERIFY $label ok ($line)"
}

# Matching seed: the stash holds the edit and nothing else.
run_case "$SEED" 1 "seed-match"
# Mismatched seed: every loaded chunk is preserved; the edit still survives.
run_case "$((SEED + 1))" "$CHUNKS" "seed-mismatch"

echo "PERSIST_VERIFY all ok"
