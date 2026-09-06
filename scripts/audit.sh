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

# Both the exit status AND the pattern.
#
# This used to check only the pattern, and that made it a much weaker
# guard than it looked. --verify-4d prints its whole result line whether
# it passed or failed, so "changed=1 returned=1" appears in the output of
# a FAILING run - and the step reported PASS while the engine exited 1.
# Fault-injecting rotate_slice to a no-op produced exactly that: VERIFY4D
# ... tilt_changed=0 ... FAILED, exit code 1, audit verdict PASS.
#
# The pattern is still checked, because an exit status alone would not
# notice a mode that silently stopped measuring the thing it names.
grep_step() {
  local name=$1 pattern=$2; shift 2
  local out status
  out=$("$@" 2>&1); status=$?
  if [ "$status" -ne 0 ]; then
    echo "FAIL  $name (exit $status)"
    echo "$out" | tail -20
    failures=$((failures + 1))
  elif echo "$out" | grep -qE "$pattern"; then
    echo "PASS  $name"
  else
    echo "FAIL  $name (wanted /$pattern/)"
    echo "$out" | tail -20
    failures=$((failures + 1))
  fi
}

step      "unit tests"          ctest --test-dir build --output-on-failure
step      "mesher differential fuzz" ./build/mesher_fuzz_tests
# The ratio counts only faces a camera can reach: the chunk is meshed
# against its four real neighbours, so faces buried against the next chunk
# along are never emitted by either mesher. That is a smaller number than
# the chunk-local one this used to check (18.1x) and a defensible one.
grep_step "greedy ratio >= 4.5x" "[4-9]\.[0-9]x fewer quads|[1-9][0-9]\.[0-9]x fewer quads" \
          ./build/voxel_engine --bench
grep_step "GPU mesh validation" "bad_triangles=0 .* ok"    ./build/voxel_engine --validate
grep_step "naive mesher validates" "bad_triangles=0 .* ok" \
          ./build/voxel_engine --naive-mesh --validate

# The greedy win, measured on the running engine rather than the bench:
# build the same world both ways and compare what each leaves resident on
# the GPU. This is the claim a reader can check for themselves in two
# commands, so it is checked here too.
if [ "${AUDIT_SKIP_MESHER_AB:-0}" != "1" ]; then
  printf '%-34s' "greedy vs naive resident bytes"
  G=$(./build/voxel_engine --validate 2>/dev/null \
        | sed -n 's/.*gpu_mesh_mb=\([0-9.]*\).*/\1/p')
  N=$(./build/voxel_engine --naive-mesh --validate 2>/dev/null \
        | sed -n 's/.*gpu_mesh_mb=\([0-9.]*\).*/\1/p')
  # failures, not fail. This step set a variable named `fail` that
  # nothing in the script ever reads, so the only check here that does
  # not go through the step/grep_step harness was also the only one that
  # could not fail the audit: it printed FAIL, and the run ended "AUDIT:
  # all checks passed" with exit 0. Verified by running a copy against a
  # stub engine reporting the same footprint for both meshers.
  if [ -z "$G" ] || [ -z "$N" ]; then
    echo "FAIL (could not read gpu_mesh_mb)"; failures=$((failures + 1))
  else
    awk -v g="$G" -v n="$N" 'BEGIN {
      r = (g > 0) ? n / g : 0
      if (r >= 2.5) printf "PASS  greedy %.2f MB vs naive %.2f MB (%.2fx)\n", g, n, r
      else        { printf "FAIL  greedy %.2f MB vs naive %.2f MB (%.2fx, want >= 2.5x)\n", g, n, r; exit 1 }
    }' || failures=$((failures + 1))
  fi
fi
# Names all three legs. The pattern was "survived=1 ok", which is the
# in-session leg alone - the two that followed it, reaching disk and
# surviving a wipe, could have been deleted without this noticing.
grep_step "edit persistence" \
  "survived=1 survives_disk=1 wipe_clean=1 ok" \
  ./build/voxel_engine --verify-edit-persistence
# One run, one pattern naming BOTH motions. The pattern used to be just
# "changed=1 returned=1", which is the translation half - so the entire
# rotation feature could have been deleted without this step noticing.
grep_step "4D slice step + tilt" \
  "changed=1 returned=1.*tilt_changed=1 tilt_returned=1 notch=1 edit_survives_scroll=1 converges=1 pivot=1 ground_fixed=1 tilt_survives_travel=1 edit_survives_inflight=1 reversible_away=1 edit_rotates=1 settled=1 edit_ns_stable=1" \
  ./build/voxel_engine --verify-4d --radius 6
grep_step "save/load roundtrip" "roundtrip_ok=1"        ./build/voxel_engine --bench-io
step      "headers self-sufficient"   ./scripts/check_headers.py
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
