#!/usr/bin/env python3
"""Fail if a commit subject is not a Conventional Commit.

The rules here were derived from this repo's own 311 commits rather than
copied off the spec page, because a linter that rejects the history it is
being added to is the wrong linter. What that measurement found:

  309 non-merge commits
      4  do not parse as type(scope): description
     87  have a subject longer than 72 characters
      4  have a subject longer than 100
      0  end the description with a full stop
     18  start the description with a capital

So three of the usual rules are NOT enforced, deliberately:

  * the 72-character subject limit would reject 28% of this history. The
    commits here routinely say what changed and why in one line and that
    is a house style, not an accident.
  * "description must start lowercase" would reject all 18 of the capital
    ones, and every one of them is a proper noun: CRC-32, README, GPU,
    CSM, HDR, PNG, Perlin, Tracy, Dear ImGui, F12.
  * a scope allowlist would be churn - scopes here track the source tree
    and move with it.

What IS enforced is the part the history already agrees on, plus the four
commits that broke it: `docs+ci:` twice, `tune:` and `audit:` once each -
invented types that read fine and sort into nothing.

    scripts/check_commit_messages.py [<revision range>]

With no argument it checks commits on HEAD that are not on the upstream
base, which is what a pull request adds.
"""

import re
import subprocess
import sys

# The Conventional Commits set. No additions: `tune`, `audit` and
# `docs+ci` are exactly what this guard exists to catch, and each of them
# had a standard type that fit (perf, test, docs).
TYPES = ("feat", "fix", "docs", "style", "refactor", "perf", "test",
         "build", "ci", "chore", "revert")

SUBJECT = re.compile(
    r"^(?P<type>[a-zA-Z+]+)"
    r"(?:\((?P<scope>[^)]*)\))?"
    r"(?P<breaking>!)?"
    r": (?P<desc>.*)$"
)

# Only a bound against nonsense, not a style rule - see the module note.
MAX_SUBJECT = 100


def commits(rev_range):
    out = subprocess.run(
        ["git", "log", "--no-merges", "--format=%H%x1f%s", rev_range],
        capture_output=True, text=True,
    )
    if out.returncode != 0:
        print(f"FAIL  could not read '{rev_range}':\n{out.stderr.strip()}")
        sys.exit(1)
    for line in out.stdout.strip().split("\n"):
        if not line:
            continue
        sha, subject = line.split("\x1f", 1)
        yield sha, subject


def default_range():
    """What this branch adds on top of where it came from, or nothing.

    Returning nothing when HEAD is already the base matters. The obvious
    fallback - check the last N commits - would run this guard over
    published history on main, where four commits predate it and cannot be
    fixed without a rewrite. A guard that fails on a branch nobody can
    make pass is a guard people learn to skip.
    """
    for base in ("origin/main", "main"):
        probe = subprocess.run(["git", "merge-base", base, "HEAD"],
                               capture_output=True, text=True)
        if probe.returncode == 0:
            merge_base = probe.stdout.strip()
            if merge_base and merge_base != resolve("HEAD"):
                return f"{merge_base}..HEAD"
    return None           # on the base itself: this branch adds nothing


def resolve(ref):
    return subprocess.run(["git", "rev-parse", ref],
                          capture_output=True, text=True).stdout.strip()


def check(sha, subject):
    """Returns a list of complaints, empty when the subject is fine."""
    bad = []
    short = sha[:8]

    # Scrubbed by a secret-removal pass. The message is gone and cannot be
    # fixed without rewriting published history, so it is not a finding.
    if subject == "***REMOVED***":
        return bad

    m = SUBJECT.match(subject)
    if not m:
        bad.append(f"{short}  not 'type(scope): description'  -- {subject}")
        return bad

    kind = m.group("type")
    if kind not in TYPES:
        bad.append(f"{short}  unknown type '{kind}'  -- {subject}\n"
                   f"          use one of: {', '.join(TYPES)}")
    if not m.group("desc").strip():
        bad.append(f"{short}  empty description  -- {subject}")
    if m.group("desc").rstrip().endswith("."):
        bad.append(f"{short}  description ends with a full stop  -- {subject}")
    if len(subject) > MAX_SUBJECT:
        bad.append(f"{short}  subject is {len(subject)} chars, over "
                   f"{MAX_SUBJECT}  -- {subject[:60]}...")
    return bad


def main():
    rev_range = sys.argv[1] if len(sys.argv) > 1 else default_range()
    if rev_range is None:
        print("ok: no commits ahead of the base to check")
        return 0

    failures, seen = [], 0
    for sha, subject in commits(rev_range):
        seen += 1
        failures.extend(check(sha, subject))

    if failures:
        print(f"FAIL  {len(failures)} problem(s) in {seen} commit(s) "
              f"({rev_range}):\n")
        for f in failures:
            print(f"  {f}")
        print("\nConventional Commits: type(optional scope): description")
        return 1

    print(f"ok: {seen} commit subject(s) in {rev_range} are conventional")
    return 0


if __name__ == "__main__":
    sys.exit(main())
