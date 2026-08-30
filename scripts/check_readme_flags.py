#!/usr/bin/env python3
"""Fail if the README documents a flag the engine does not have.

Written after a week of finding documentation that had quietly stopped
being true: the controls table listed seven blocks when there were eight,
docs/design.md called 18.1x "the CI-gated number" when the gate was 4.5,
and in the sibling repo an entire contract registry had resolved without
anything noticing.

Those share a shape. Nothing in the build reads the prose, so prose is the
only part of the project with no failure mode - it just gets quietly wrong
and stays that way until a person happens to read it next to the code.

This closes the cheapest slice of that: every --flag the README mentions
has to appear in --help. It does not check that the surrounding sentence
is true, which is the larger and unautomatable half. It checks the part a
machine can.

    scripts/check_readme_flags.py [path/to/voxel_engine]
"""

import pathlib
import re
import subprocess
import sys

# cmake and ctest flags appear in the build instructions and are not ours.
FOREIGN = {"--build", "--test-dir", "--output-on-failure", "--target"}


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./build/voxel_engine"
    root = pathlib.Path(__file__).resolve().parent.parent
    readme = (root / "README.md").read_text(encoding="utf-8")

    try:
        help_text = subprocess.run([binary, "--help"], capture_output=True,
                                   text=True, timeout=60).stdout
    except (OSError, subprocess.SubprocessError) as e:
        print(f"cannot run {binary}: {e}")
        return 1
    if not help_text.strip():
        print(f"{binary} --help printed nothing")
        return 1

    in_readme = set(re.findall(r"(--[a-z][a-z0-9-]+)", readme)) - FOREIGN
    in_help = set(re.findall(r"(--[a-z][a-z0-9-]+)", help_text))

    missing = sorted(in_readme - in_help)
    for f in missing:
        print(f"  README documents {f}, which --help does not list")

    # The reverse is a note, not a failure: a flag can reasonably exist
    # without being in the README, and several deliberately do.
    undocumented = sorted(in_help - in_readme)

    print(f"\n{len(in_readme)} flags in README, {len(in_help)} in --help")
    if undocumented:
        print(f"not mentioned in the README (fine, listed for awareness): "
              f"{' '.join(undocumented)}")
    if missing:
        print(f"\nFAIL: {len(missing)} documented flag(s) do not exist")
        return 1
    print("ok: every flag the README documents exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
