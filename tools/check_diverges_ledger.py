#!/usr/bin/env python3
"""DIVERGES ledger nudge: a commit that adds or edits a `DIVERGES:`
marker must touch DIVERGENCES.md in the same commit (CLAUDE.md rule 1).
The ledger is only trustworthy if it moves with the code; a stale
trusted ledger is worse than none.

Checks each commit in BASE..HEAD (BASE from the first argument or
origin/master). Run from the repo root.
"""
import os
import subprocess
import sys


def sh(*args):
    return subprocess.run(args, capture_output=True, text=True).stdout


def main() -> int:
    base = (sys.argv[1] if len(sys.argv) > 1 else
            os.environ.get("DIVERGES_BASE", "origin/master"))
    mb = sh("git", "merge-base", base, "HEAD").strip()
    if not mb:
        print(f"OK: no merge base with {base}; skipping")
        return 0
    commits = sh("git", "rev-list", f"{mb}..HEAD").split()
    errors = []
    for c in commits:
        diff = sh("git", "show", "--format=", c)
        touched = sh("git", "show", "--format=", "--name-only", c).split()
        # Only markers in source files count: docs, tools (including this
        # script) and workflows mention the token without declaring a
        # divergence, and must not self-flag.
        adds_diverges = False
        current = ""
        for line in diff.splitlines():
            if line.startswith("+++ b/"):
                current = line[6:]
                continue
            if not current.startswith("src/"):
                continue
            if line.startswith(("+", "-")) and not line.startswith(("+++", "---")) \
                    and "DIVERGES:" in line:
                adds_diverges = True
                break
        if adds_diverges and "DIVERGENCES.md" not in touched:
            subj = sh("git", "log", "-1", "--format=%h %s", c).strip()
            errors.append(f"{subj}: touches a DIVERGES: marker without "
                          f"updating DIVERGENCES.md")
    if errors:
        print("DIVERGES ledger failures:")
        for e in errors:
            print(f"  {e}")
        return 1
    print(f"OK: {len(commits)} commit(s) checked; DIVERGES markers and "
          f"the ledger move together")
    return 0


if __name__ == "__main__":
    sys.exit(main())
