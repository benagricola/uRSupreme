#!/usr/bin/env python3
"""Static guardrails for config and copy.

1. platformio.ini must not reference absolute developer-machine paths
   (an absolute symlink lib_dep once made the repo unbuildable for
   anyone else across 25 envs until 61cedd6).
2. No em dashes anywhere in the tracked tree - code, comments, docs and
   user-facing copy alike (project style rule). Generated and vendored
   files are excluded; everything else fails the build on a single
   em dash, with file:line.

Run from the repo root: python3 tools/check_copy_and_config.py
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

EXCLUDE = {
    "src/Web/SPAEmbedded.h",     # generated (gzip embed of the SPA)
    "src/Web/spa/alpine.min.js", # vendored
    "src/Web/spa/leaflet.min.js",# vendored (map renderer)
    "src/Web/spa/leaflet.css",   # vendored (map renderer)
    "src/Web/spa/protomaps-leaflet.js",  # vendored (vector map renderer)
    "MIRROR.md",                 # upstream author's notice, mirrored verbatim
}
# Mirrored/vendored trees we must never rewrite: upstream content is
# excluded wholesale so a future upstream sync can't trip the style ban
# (and so nobody "fixes" text the project doesn't own).
EXCLUDE_PREFIXES = (
    "Console/",
)


def check_platformio() -> list[str]:
    errors = []
    path = os.path.join(ROOT, "platformio.ini")
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        if line.lstrip().startswith(";"):
            continue
        if re.search(r"(symlink://)?/home/|/Users/", line):
            errors.append(f"platformio.ini:{n}: absolute path: {line.strip()}")
    return errors


def check_no_emdash() -> list[str]:
    errors = []
    files = subprocess.run(["git", "ls-files"], cwd=ROOT,
                           capture_output=True, text=True).stdout.splitlines()
    for f in files:
        if f in EXCLUDE or f.startswith(EXCLUDE_PREFIXES):
            continue
        try:
            with open(os.path.join(ROOT, f), encoding="utf-8") as fh:
                emdash = chr(0x2014)  # numeric so this checker never flags itself
                for n, line in enumerate(fh, 1):
                    if emdash in line:
                        errors.append(f"{f}:{n}: em dash: {line.strip()[:80]}")
        except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
            continue
    return errors


def main() -> int:
    errors = check_platformio() + check_no_emdash()
    if errors:
        print("guardrail failures:")
        for e in errors:
            print(f"  {e}")
        return 1
    print("OK: no absolute paths in platformio.ini; no em dashes in the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
