#!/usr/bin/env python3
"""Fail if the SPA calls an API path the firmware does not serve.

Firmware routes are extracted from server.on("..."), on_json_post("...")
and uri("...{}...") registrations under src/Web/. SPA calls are the
/api/... string literals in src/Web/spa/index.html. A literal ending in
'/' is a concatenation prefix (e.g. '/api/conversations/' + peer) and
matches if some firmware route extends it; anything else must match a
route exactly ({} segments match one path segment).

This check exists because a silent endpoint rename (40c3ad8,
/api/system/migrate_flash_to_sd vs /api/storage/...) left a UI button
404ing for 19 days. Run from the repo root: python3 tools/check_api_parity.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "src", "Web")
SPA = os.path.join(WEB, "spa", "index.html")

# Calls the SPA composes in ways this script cannot statically resolve.
# Keep this list short and justified.
ALLOWLIST: set[str] = set()


def firmware_routes() -> set[str]:
    # server.on("..."), on_json_post("..."), uri("...{}..."), and the
    # AsyncWebSocket constructor path (e.g. AsyncWebSocket s("/api/ws")).
    pat = re.compile(
        r'(?:server\.on|on_json_post|AsyncWebSocket\s+\w+)\(\s*(?:uri\()?\s*"([^"]+)"')
    routes = set()
    for dirpath, _dirs, files in os.walk(WEB):
        if os.path.basename(dirpath) == "spa":
            continue
        for fn in files:
            if not fn.endswith(".h"):
                continue
            with open(os.path.join(dirpath, fn), encoding="utf-8") as f:
                routes.update(pat.findall(f.read()))
    return routes


def spa_calls() -> set[str]:
    text = open(SPA, encoding="utf-8").read()
    lits = set(re.findall(r"""['"`](/api/[^'"`\s?]*)['"`?]""", text))
    # The WebSocket path is composed as a relative URL elsewhere; include
    # plain non-api absolute paths only if they ever appear. /api/ scope
    # keeps noise out (static assets are served by their own handlers).
    return lits


def route_regex(route: str) -> re.Pattern:
    parts = [re.escape(p) if p != "{}" else "[^/]+" for p in route.split("/")]
    return re.compile("^" + "/".join(parts) + "$")


def main() -> int:
    routes = firmware_routes()
    regexes = [(r, route_regex(r)) for r in routes]
    missing = []
    for call in sorted(spa_calls()):
        if call in ALLOWLIST:
            continue
        if call.endswith("/"):
            # Concatenation prefix: some firmware route must extend it.
            if any(r.startswith(call) for r in routes):
                continue
            missing.append(call + " (prefix)")
        else:
            if any(rx.fullmatch(call) for _r, rx in regexes):
                continue
            missing.append(call)
    if missing:
        print("SPA calls with no matching firmware route:")
        for m in missing:
            print(f"  {m}")
        print(f"\nfirmware routes checked: {len(routes)}")
        return 1
    print(f"OK: every SPA /api call matches a firmware route "
          f"({len(routes)} routes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
