#!/usr/bin/env python3
"""Fail if the prose documents a flag the engine does not have.

Written after a week of finding documentation that had quietly stopped
being true: the controls table listed seven blocks when there were eight,
docs/design.md called 18.1x "the CI-gated number" when the gate was 4.5,
and in the sibling repo an entire contract registry had resolved without
anything noticing.

Those share a shape. Nothing in the build reads the prose, so prose is the
only part of the project with no failure mode - it just gets quietly wrong
and stays that way until a person happens to read it next to the code.

This closes the cheapest slice of that: every --flag the prose mentions
has to appear in --help. It does not check that the surrounding sentence
is true, which is the larger and unautomatable half. It checks the part a
machine can.

    scripts/check_readme_flags.py [path/to/build]

The README documents more than one executable, so the flags it mentions
are checked against the union of every binary's --help. Scoping it to
voxel_engine alone made it fail on `slice4d --tilt`, which is a real flag
on a different program - a false failure, and a guard that cries wolf is
one people learn to skip.
"""

import pathlib
import re
import subprocess
import sys

# cmake and ctest flags appear in the build instructions and are not ours.
FOREIGN = {"--build", "--test-dir", "--output-on-failure", "--target"}

# Every executable the README gives a command line for. Each must answer
# --help, which is a small contract but the one this check rests on: a
# binary that ignores --help and runs its normal job instead would make
# this script slow, or worse, pass by printing something flag-shaped.
BINARIES = ("voxel_engine", "slice4d", "hyperslice")


def main():
    build = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "./build")
    # Accept a binary path as well as a build directory. The argument used
    # to be the engine binary, and silently reinterpreting that as a
    # directory would have turned every existing caller into a confusing
    # "cannot run ./build/voxel_engine/voxel_engine".
    if build.is_file():
        build = build.parent
    root = pathlib.Path(__file__).resolve().parent.parent
    # The README is not the only prose that names flags any more. Every
    # doc that does is a place a renamed or deleted one can rot, and
    # docs/atmosphere.md shipped with seven of them the day this was
    # widened - which is exactly when a guard is cheapest to extend.
    sources = [root / "README.md"]
    sources += sorted((root / "docs").glob("*.md"))
    prose = "\n".join(p.read_text(encoding="utf-8")
                      for p in sources if p.exists())
    readme = prose

    help_text = ""
    for name in BINARIES:
        binary = build / name
        try:
            out = subprocess.run([str(binary), "--help"], capture_output=True,
                                 text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError) as e:
            print(f"cannot run {binary}: {e}")
            return 1
        if not out.strip():
            print(f"{binary} --help printed nothing")
            return 1
        help_text += out

    in_readme = set(re.findall(r"(--[a-z][a-z0-9-]+)", readme)) - FOREIGN
    in_help = set(re.findall(r"(--[a-z][a-z0-9-]+)", help_text))

    missing = sorted(in_readme - in_help)
    for f in missing:
        print(f"  the docs mention {f}, which --help does not list")

    # The reverse is a note, not a failure: a flag can reasonably exist
    # without being in the README, and several deliberately do.
    undocumented = sorted(in_help - in_readme)

    print(f"\n{len(in_readme)} flags in the docs, {len(in_help)} across "
          f"{len(BINARIES)} binaries")
    if undocumented:
        print(f"not mentioned in the docs (fine, listed for awareness): "
              f"{' '.join(undocumented)}")
    if missing:
        print(f"\nFAIL: {len(missing)} documented flag(s) do not exist")
        return 1
    print("ok: every flag the docs mention exists")
    return 0


if __name__ == "__main__":
    sys.exit(main())
