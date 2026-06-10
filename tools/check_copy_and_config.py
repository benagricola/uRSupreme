#!/usr/bin/env python3
"""Static guardrails for config and user-facing copy.

1. platformio.ini must not reference absolute developer-machine paths
   (an absolute symlink lib_dep once made the repo unbuildable for
   anyone else across 25 envs until 61cedd6).
2. User-facing SPA copy must not contain em dashes (project copy rule;
   slipped through twice before). JS/HTML comment lines are exempt:
   the rule covers what users read, not what developers read.

Run from the repo root: python3 tools/check_copy_and_config.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check_platformio() -> list[str]:
    errors = []
    path = os.path.join(ROOT, "platformio.ini")
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if line.lstrip().startswith(";"):
            continue
        if re.search(r"(symlink://)?/home/|/Users/", line):
            errors.append(f"platformio.ini:{n}: absolute path: {line.strip()}")
    return errors


def check_spa_emdash() -> list[str]:
    errors = []
    path = os.path.join(ROOT, "src", "Web", "spa", "index.html")
    in_block_comment = False
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        stripped = line.lstrip()
        if in_block_comment:
            if "*/" in stripped:
                in_block_comment = False
            continue
        if stripped.startswith(("//", "/*", "*", "<!--")):
            if stripped.startswith("/*") and "*/" not in stripped:
                in_block_comment = True
            continue
        if "em-dash-ok" in line:
            continue
        # Strip trailing line comments so an explanatory comment with an
        # em dash doesn't flag the code line it sits on.
        code = re.sub(r"//.*$", "", line)
        for m in re.finditer("—", code):
            before = code[:m.start()].rstrip()[-1:] or ""
            after = code[m.end():].lstrip()[:1] or ""
            # A standalone dash is a value placeholder ('—', >—<), which
            # the copy rule allows; a dash adjacent to prose is not.
            if before in "'\">:([{," and after in "'\"<,)]}":
                continue
            errors.append(f"index.html:{n}: em dash in user-facing copy: "
                          f"{line.strip()[:90]}")
            break
    return errors


def main() -> int:
    strict_copy = "--strict-copy" in sys.argv
    hard = check_platformio()
    copy = check_spa_emdash()
    if copy:
        # Report-only until the standing copy sweep lands: ~70 legacy em
        # dashes predate this check, and a permanently red job teaches
        # everyone to ignore CI. New code must still keep this list
        # shrinking; flip to --strict-copy once it reaches zero.
        print(f"copy warnings ({len(copy)} em-dash lines, "
              f"{'enforced' if strict_copy else 'report-only'}):")
        for e in copy:
            print(f"  {e}")
    if hard or (strict_copy and copy):
        print("guardrail failures:")
        for e in hard:
            print(f"  {e}")
        return 1
    print("OK: no absolute paths in platformio.ini"
          + ("" if copy else "; no em dashes in SPA copy"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
