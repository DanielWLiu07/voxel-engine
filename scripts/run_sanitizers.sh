#!/usr/bin/env bash
#
# Builds and runs the test suites under sanitizers:
#   - ThreadSanitizer over the concurrency primitives (lock-free MPMC queue +
#     worker pool) — validates the atomic memory ordering and that the pool has
#     no data races.
#   - AddressSanitizer + UndefinedBehaviorSanitizer over the full logic suite
#     (mesher, RLE codec incl. its fuzz cases, world bookkeeping).
#
# Any sanitizer hit fails the script (halt_on_error). The only allowed UBSan
# suppression is the vendored FastNoiseLite hash's intentional integer
# wraparound (tools/sanitizer/ubsan_suppressions.txt) — our own code must be
# clean. This is what the CI "sanitizers" job runs.
#
# Usage: scripts/run_sanitizers.sh   (run from anywhere; cds to repo root)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
SUPP="$REPO_ROOT/tools/sanitizer/ubsan_suppressions.txt"

echo "==== ThreadSanitizer: concurrency primitives ===="
cmake -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVOXEL_BUILD_BENCH=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" >/dev/null
cmake --build build-tsan --target mpmc_tests
TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1" ./build-tsan/mpmc_tests

echo ""
echo "==== AddressSanitizer + UndefinedBehaviorSanitizer: full logic suite ===="
cmake -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DVOXEL_BUILD_BENCH=OFF \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null
cmake --build build-asan --target voxel_tests mpmc_tests
export ASAN_OPTIONS="halt_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:suppressions=$SUPP"
./build-asan/voxel_tests
./build-asan/mpmc_tests

echo ""
echo "All sanitizer runs clean (TSan + ASan + UBSan)."
