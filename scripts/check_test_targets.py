#!/usr/bin/env python3
"""Fail if the test suite is not the same suite on every platform.

Written after `if(APPLE)` in CMakeLists.txt was left unclosed. The
`endif()` that was meant to close it sat forty lines further down, so
`add_executable(prism_tests)` and `add_executable(hyperslice_tests)` -
the two binaries that cover the whole 4D cross-section kernel - were
inside the Apple branch. On macOS everything built and eight tests ran.
On Linux the two targets did not exist at all.

The failure mode is the point. Linux CI did not report a missing test;
it reported

    100% tests passed, 0 tests failed out of 6

and went green, for months, on a suite two tests short. Nothing in
ctest can notice this: ctest runs the tests that were registered, and
the tests were not registered. A test that only exists on the machine
where it passes is not a test, and a suite whose size is a function of
the platform cannot say what it covers.

So the expected suite is written down here, as a set, and both CI jobs
compare against it. Adding a test means adding a line below - that is
deliberate. The list is the claim about what is covered; if it is not
maintained by hand it is not a claim.

    scripts/check_test_targets.py [build-dir]
"""

import json
import pathlib
import subprocess
import sys

# Every ctest test this project defines. Not per-platform: that is the
# bug. If a test genuinely cannot run somewhere, it belongs in ctest's
# own DISABLED/skip machinery, where it is still registered and still
# visible, not behind a CMake branch where it silently ceases to exist.
EXPECTED = {
    "world_unit",           # chunk bookkeeping, section bounds, mesher equivalence
    "prism_mesh",           # 4D cross-section meshing: closed surface, tiling, winding
    "hyperslice",           # the slice kernel: clipping, cell ownership, convexity
    "mesher_equivalence",   # greedy vs naive, face-for-face, fuzzed
    "noise4d",              # 4D gradient noise
    "terrain4d",            # 4D terrain generation, caves, structures
    "cli_options",          # argv -> options
    "mpmc_queue",           # lock-free queue + worker pool under stress
    "slice_ease",           # the wheel's angle debt: exact, and never a visible jump
}


def main() -> int:
    build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build")
    if not (build / "CTestTestfile.cmake").exists():
        print(f"FAIL  {build} is not a configured build directory")
        return 1

    out = subprocess.run(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        print(f"FAIL  ctest --show-only failed:\n{out.stderr}")
        return 1

    found = {t["name"] for t in json.loads(out.stdout)["tests"]}

    missing = EXPECTED - found
    extra = found - EXPECTED
    for name in sorted(missing):
        print(f"FAIL  test '{name}' is expected but was not registered - "
              f"a platform branch in CMakeLists.txt has swallowed its target")
    for name in sorted(extra):
        print(f"FAIL  test '{name}' is registered but not in EXPECTED - "
              f"add it to scripts/check_test_targets.py")
    if missing or extra:
        print(f"\n{len(found)} registered, {len(EXPECTED)} expected")
        return 1

    print(f"OK  all {len(EXPECTED)} tests registered in {build}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
