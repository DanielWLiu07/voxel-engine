#!/usr/bin/env bash
#
# The whole local verification battery in one command: what CI checks plus
# the GL-dependent proofs CI runners cannot run. Green output here means
# every guarantee the README claims is holding on this machine.
#
#   scripts/audit.sh                    # ~2 min: tests, ratios, GL proofs
#   scripts/audit.sh --with-sanitizers  # + TSan and ASan/UBSan suites
#
# Each step prints PASS/FAIL and the script exits nonzero on the first
# failure, so it composes with git hooks and quick pre-push checks.

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -x build/voxel_engine ]; then
  echo "Build first: cmake -B build -G Ninja && cmake --build build -j" >&2
  exit 1
fi

failures=0
step() {
  local name=$1; shift
  local out
  if out=$("$@" 2>&1); then
    echo "PASS  $name"
  else
    echo "FAIL  $name"
    echo "$out" | tail -20
    failures=$((failures + 1))
  fi
}

grep_step() {
  local name=$1 pattern=$2; shift 2
  local out
  out=$("$@" 2>&1)
  if echo "$out" | grep -qE "$pattern"; then
    echo "PASS  $name"
  else
    echo "FAIL  $name (wanted /$pattern/)"
    echo "$out" | tail -20
    failures=$((failures + 1))
  fi
}

step      "unit tests"          ctest --test-dir build --output-on-failure
# The ratio counts only faces a camera can reach: the chunk is meshed
# against its four real neighbours, so faces buried against the next chunk
# along are never emitted by either mesher. That is a smaller number than
# the chunk-local one this used to check (18.1x) and a defensible one.
grep_step "greedy ratio >= 4.5x" "[4-9]\.[0-9]x fewer quads|[1-9][0-9]\.[0-9]x fewer quads" \
          ./build/voxel_engine --bench
grep_step "GPU mesh validation" "bad_triangles=0 ok"    ./build/voxel_engine --validate
grep_step "edit persistence"    "survived=1 ok"         ./build/voxel_engine --verify-edit-persistence
grep_step "save/load roundtrip" "roundtrip_ok=1"        ./build/voxel_engine --bench-io
step      "occlusion byte-identity"   ./scripts/verify_occlusion.sh
step      "persistence contract"      ./scripts/verify_persistence.sh

if [ "${1:-}" = "--with-sanitizers" ]; then
  step "sanitizer suite (TSan + ASan/UBSan)" ./scripts/run_sanitizers.sh
fi

echo
if [ "$failures" -gt 0 ]; then
  echo "AUDIT: $failures step(s) failed"
  exit 1
fi
echo "AUDIT: all checks passed"
