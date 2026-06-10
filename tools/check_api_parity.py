#!/usr/bin/env python3
"""API-surface guardrail: the contract is declared, not inferred.

src/Web/api_routes.def is the single source of truth for every device
API path. The C++ side consumes it as ApiRoutes:: constants (compile
error on a typo); the SPA consumes it as the build-generated `API`
table. This check fails anything that bypasses the def:

1. No quoted "/api/..." literal anywhere in src/Web C++ (routes go
   through ApiRoutes::).
2. No quoted /api literal in the SPA source (calls go through API.*).
3. Every `API.NAME` the SPA references exists in the def.
4. Every def entry is referenced from the C++ side (dead entries rot
   the contract).
5. No duplicate names or paths in the def.

Nothing here infers routes from code shapes; bypasses are string-banned
outright, so a new registration helper or call pattern cannot silently
escape the check. The runtime half of the contract is asserted by
tools/kiss-test/test_api/test_routes.py against GET /api/diag/routes.

Run from the repo root: python3 tools/check_api_parity.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "src", "Web")
SPA = os.path.join(WEB, "spa", "index.html")
DEF = os.path.join(WEB, "api_routes.def")

DEF_LINE = re.compile(r'^\s*ROUTE(_OPT)?\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"(/api/[^"]+)"\s*\)\s*$')


def parse_def():
    entries = []
    errors = []
    for n, line in enumerate(open(DEF, encoding="utf-8"), 1):
        if not line.strip() or line.lstrip().startswith("//"):
            continue
        m = DEF_LINE.match(line)
        if not m:
            errors.append(f"api_routes.def:{n}: line does not match the "
                          f"ROUTE grammar: {line.strip()[:80]}")
            continue
        entries.append((m.group(2), m.group(3), bool(m.group(1))))
    names = [e[0] for e in entries]
    paths = [e[1] for e in entries]
    for dup in {x for x in names if names.count(x) > 1}:
        errors.append(f"api_routes.def: duplicate name {dup}")
    for dup in {x for x in paths if paths.count(x) > 1}:
        errors.append(f"api_routes.def: duplicate path {dup}")
    return entries, errors


def cpp_files():
    for dirpath, dirs, files in os.walk(WEB):
        if os.path.basename(dirpath) == "spa":
            dirs[:] = []
            continue
        for fn in files:
            if fn.endswith(".h"):
                yield os.path.join(dirpath, fn)


def main() -> int:
    entries, errors = parse_def()
    names = {e[0] for e in entries}

    # 1. C++: quoted /api literals are banned (comments use bare paths,
    # which never match the quoted pattern, so no comment-stripping).
    referenced = set()
    for path in cpp_files():
        text = open(path, encoding="utf-8").read()
        rel = os.path.relpath(path, ROOT)
        for n, line in enumerate(text.splitlines(), 1):
            if re.search(r'"/api/', line):
                errors.append(f"{rel}:{n}: quoted /api literal; use "
                              f"ApiRoutes:: from api_routes.def")
        referenced.update(re.findall(r"ApiRoutes::([A-Z][A-Z0-9_]*)", text))

    # 2+3. SPA: quoted /api literals banned; API.* refs must exist.
    spa_text = open(SPA, encoding="utf-8").read()
    for n, line in enumerate(spa_text.splitlines(), 1):
        if "__API_ROUTES__" in line:
            continue
        if re.search(r"""['"`]/api/""", line):
            errors.append(f"src/Web/spa/index.html:{n}: quoted /api "
                          f"literal; use the generated API table")
    for ref in set(re.findall(r"\bAPI\.([A-Z][A-Z0-9_]*)", spa_text)):
        if ref not in names:
            errors.append(f"SPA references API.{ref}, which is not in "
                          f"api_routes.def")

    # 4. Dead def entries: every route must be registered somewhere.
    for name in sorted(names - referenced):
        errors.append(f"api_routes.def entry {name} is never referenced "
                      f"from src/Web C++ (dead route?)")

    if errors:
        print("API parity failures:")
        for e in errors:
            print(f"  {e}")
        return 1
    print(f"OK: {len(entries)} routes declared in api_routes.def; no "
          f"bypassing literals; all SPA references and C++ registrations "
          f"line up")
    return 0


if __name__ == "__main__":
    sys.exit(main())
