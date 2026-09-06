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

# An edit comes back by one of two mechanisms, and which one is correct
# depends on whether the terrain generator could reproduce the chunk.
#
#   replay   the chunk is regenerated and the edit replayed on top. This
#            is the path for anything the generator can rebuild, and it is
#            what lets an edited chunk keep changing when the 4D slice
#            moves instead of freezing at the moment it was built.
#   stash    the chunk is kept whole and handed back verbatim. Only for
#            chunks the generator CANNOT reproduce: saved with edits
#            already in them, or loaded under a different seed.
#
# The contract used to demand the stash in both cases, because the stash
# was the only mechanism. Demanding it for a reproducible chunk is now
# demanding the defect.
run_case() {
  local seed="$1" want="$2" label="$3"
  local line
  line=$(./build/voxel_engine --load "$DIR" --seed "$seed" --radius "$RADIUS" \
           --verify-edit-persistence 2>/dev/null | grep '^EDIT_PERSIST') || {
    echo "PERSIST_VERIFY $label: no EDIT_PERSIST line" >&2; exit 1; }
  case "$line" in
    *"$want "*" ok") ;;
    *) echo "PERSIST_VERIFY $label FAILED: $line (wanted $want ... ok)" >&2
       exit 1 ;;
  esac
  echo "PERSIST_VERIFY $label ok ($line)"
}

# Matching seed, clean save: every chunk is reproducible, so the edit made
# by the check itself rides back on the replay path and nothing is stashed.
run_case "$SEED" "stashed=0 restored=0 replayed=1" "seed-match"
# Mismatched seed: no loaded chunk is reproducible, so all of them are
# preserved whole and the edit survives inside one of them.
run_case "$((SEED + 1))" "stashed=$CHUNKS" "seed-mismatch"

echo "PERSIST_VERIFY all ok"
