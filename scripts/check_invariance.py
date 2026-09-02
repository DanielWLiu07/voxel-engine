#!/usr/bin/env python3
"""Fail if a figure claimed hardware-independent is not.

The README's first performance table is titled "The numbers that do not
depend on the machine" and states its own premise: a ratio and a byte
count are properties of the engine alone, and they reproduce exactly on
any GPU. Nothing enforced that. A parallel-efficiency figure sat in the
table for months and measured 6.3x, 6.8x, 6.9x and 8.5x across four runs
purely from background load, while a caveat 400 lines below the table
described the same swing.

So the claim was being made in one place and contradicted in another, and
the only way to notice was to read both. This is that reading, automated:
run --bench several times and require BENCH_SUMMARY to come out
byte-identical.

Byte-identical rather than within-tolerance is the right bar here.
--bench is a fixed-seed CPU computation over generated terrain; every
field in it is a count, a ratio of counts, or a byte total. If any of them
moves at all, either the bench became nondeterministic or a figure that is
not a property of the engine has been added to it. Both are worth failing
on, and a tolerance would hide the second.

    scripts/check_invariance.py [runs] [path/to/voxel_engine]
"""

import subprocess
import sys


def summary(binary):
    out = subprocess.run([binary, "--bench"], capture_output=True, text=True,
                         timeout=900).stdout
    for line in out.splitlines():
        if line.startswith("BENCH_SUMMARY"):
            return line.strip()
    return None


def main():
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    binary = sys.argv[2] if len(sys.argv) > 2 else "./build/voxel_engine"

    seen = []
    for i in range(runs):
        line = summary(binary)
        if line is None:
            print(f"run {i + 1}: no BENCH_SUMMARY in --bench output")
            return 1
        seen.append(line)
        print(f"  run {i + 1}: {line}")

    if len(set(seen)) == 1:
        fields = len(seen[0].split()) - 1
        print(f"\nok: {fields} fields identical across {runs} runs")
        return 0

    # Name the fields that moved, not just the fact that something did.
    print("\nFAIL: BENCH_SUMMARY is not reproducible across runs")
    parsed = [dict(tok.split("=", 1) for tok in s.split()[1:]) for s in seen]
    for key in parsed[0]:
        values = {p.get(key) for p in parsed}
        if len(values) > 1:
            print(f"  {key} varied: {' '.join(sorted(values))}")
            print(f"    -> not a property of the engine; it does not belong "
                  f"in the hardware-independent table")
    return 1


if __name__ == "__main__":
    sys.exit(main())
