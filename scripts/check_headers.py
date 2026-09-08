#!/usr/bin/env python3
"""Fail if a header uses a standard-library name it does not include.

Written after `terrain_gen4d.h` called std::cos with no <cmath>, built
clean on macOS, and failed the Linux CI build. libc++ pulls <cmath> in
through another header; libstdc++ does not. The bug was invisible on the
only machine it was ever compiled on.

The obvious guard - compile every header standalone - does NOT catch it,
and that is worth writing down rather than discovering twice. Standalone
compilation still runs under the local standard library, so on macOS the
header compiles on its own for exactly the reason the bug survived:
libc++ supplies <cmath> transitively no matter how the translation unit
is arranged. Deleting the include again and rerunning it reports success.

So this checks the dependency directly instead, which works under any
standard library: if a header names a symbol, it must include the header
that owns it. Textual, not semantic - it does not parse C++ and cannot
know that a name in a comment is not a use. That is the trade: it is
cheap, it runs anywhere, and it catches the class of bug that only shows
up on someone else's toolchain.

    scripts/check_headers.py
"""

import pathlib
import re
import sys

# Symbol -> the header that owns it. Deliberately conservative: only names
# whose home is unambiguous. A guard with false positives is one people
# learn to skip, so a missing entry here is much cheaper than a wrong one.
OWNS = {
    "<cmath>": ("cos", "sin", "tan", "acos", "asin", "atan", "atan2", "sqrt",
                "cbrt", "hypot", "pow", "exp", "log", "log2", "log10",
                "floor", "ceil", "round", "trunc", "fmod", "fabs",
                "isnan", "isinf", "isfinite", "lerp"),
    "<vector>": ("vector",),
    "<string>": ("string", "to_string", "stoi", "stof"),
    "<array>": ("array",),
    "<optional>": ("optional", "nullopt_t"),
    "<memory>": ("unique_ptr", "shared_ptr", "weak_ptr", "make_unique",
                 "make_shared"),
    "<functional>": ("function", "hash", "ref", "cref"),
    "<algorithm>": ("sort", "stable_sort", "clamp", "find", "find_if",
                    "count_if", "lower_bound", "upper_bound", "fill",
                    "copy", "reverse", "rotate", "all_of", "any_of",
                    "none_of"),
    "<numeric>": ("accumulate", "iota"),
    "<unordered_map>": ("unordered_map",),
    "<unordered_set>": ("unordered_set",),
    "<map>": ("map",),
    "<set>": ("set",),
    "<atomic>": ("atomic", "atomic_flag", "memory_order"),
    "<mutex>": ("mutex", "lock_guard", "unique_lock", "scoped_lock"),
    "<thread>": ("thread", "jthread"),
    "<chrono>": ("chrono",),
    "<span>": ("span",),
    "<cstdint>": ("uint8_t", "uint16_t", "uint32_t", "uint64_t",
                  "int8_t", "int16_t", "int32_t", "int64_t", "uintptr_t"),
    "<cstddef>": ("size_t", "ptrdiff_t", "byte"),
    "<utility>": ("move", "forward", "pair", "swap", "exchange"),
    "<expected>": ("expected", "unexpected"),
    "<type_traits>": ("is_same_v", "enable_if_t", "decay_t",
                      "remove_cvref_t", "underlying_type_t"),
    "<limits>": ("numeric_limits",),
    "<stdexcept>": ("runtime_error", "logic_error", "out_of_range"),
}

# <cmath> also reaches these through <cstdlib>, and std::max/std::min are
# in <algorithm> but every container header drags them along in practice;
# both are common enough that flagging them would be noise, not signal.
IGNORE_IF_ALSO = {"<cmath>": ("<cstdlib>",)}

# A comment or a string is not a use. Strip them before matching, so a
# header that only MENTIONS std::sort in an explanation is not flagged -
# this file is full of such prose and would flag itself otherwise.
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.S)
LINE_COMMENT = re.compile(r"//[^\n]*")
STRING_LIT = re.compile(r'"(?:[^"\\]|\\.)*"')


def strip_noise(text):
    text = BLOCK_COMMENT.sub(" ", text)
    text = LINE_COMMENT.sub(" ", text)
    return STRING_LIT.sub('""', text)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent
    headers = sorted((root / "src").rglob("*.h"))
    if not headers:
        print("no headers found - wrong directory?")
        return 1

    problems = []
    for h in headers:
        raw = h.read_text(encoding="utf-8")
        code = strip_noise(raw)
        included = set(re.findall(r"#\s*include\s*(<[^>]+>)", raw))
        for header, symbols in OWNS.items():
            if header in included:
                continue
            if any(alt in included for alt in IGNORE_IF_ALSO.get(header, ())):
                continue
            for sym in symbols:
                if re.search(r"\bstd::" + re.escape(sym) + r"\b", code):
                    problems.append((h.relative_to(root), sym, header))
                    break

    for path, sym, header in problems:
        print(f"  {path}: uses std::{sym} but does not include {header}")

    if problems:
        print(f"\nFAIL: {len(problems)} header(s) rely on a transitive include")
        return 1
    print(f"ok: all {len(headers)} headers include what they use")
    return 0


if __name__ == "__main__":
    sys.exit(main())
