"""GET /api/diag/routes — the running device's registered route table
must match src/Web/api_routes.def (the declared API surface).

This is the runtime half of the API-parity contract: the static check
(tools/check_api_parity.py) proves nobody bypasses the def in source;
this test proves the booted firmware actually serves what the def
declares. ROUTE_OPT entries are build-flag-gated and may be absent;
everything else must be registered, and the device must not register
an /api route the def does not declare.
"""
import os
import re

REPO_ROOT = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", ".."))
DEF = os.path.join(REPO_ROOT, "src", "Web", "api_routes.def")
DEF_LINE = re.compile(
    r'^\s*ROUTE(_OPT)?\(\s*([A-Z][A-Z0-9_]*)\s*,\s*"(/api/[^"]+)"\s*\)\s*$')


def _def_entries():
    required, optional = set(), set()
    for line in open(DEF, encoding="utf-8"):
        m = DEF_LINE.match(line)
        if not m:
            continue
        (optional if m.group(1) else required).add(m.group(3))
    assert required, "api_routes.def parsed to zero required routes"
    return required, optional


def test_registered_routes_match_def(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/diag/routes", timeout=15)
    assert r.status_code == 200
    served = {e["p"] for e in r.json()["routes"]}
    required, optional = _def_entries()
    missing = required - served
    undeclared = served - required - optional
    assert not missing, \
        f"declared in api_routes.def but not registered: {sorted(missing)}"
    assert not undeclared, \
        f"registered but not declared in api_routes.def: {sorted(undeclared)}"
