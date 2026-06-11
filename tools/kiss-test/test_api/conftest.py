"""Shared fixtures for the API regression suite.

Runs against the two bench devices (SX + LR). Discovers them by trying
the cached IPs in .token / .lr-token sibling files, refreshes the
bearer tokens at session start, exposes per-device `requests.Session`
fixtures with the Authorization header pre-set.

Each test should be tolerant of empty inboxes / fresh boots - the
fixtures don't pre-stage any state. Tests that need state (an inbound
message, a sent attachment) should set it up themselves.

Run from the repo root:
    cd microReticulum_Firmware/tools/kiss-test
    pytest test_api/ -v
"""
from __future__ import annotations

import os
import time
from dataclasses import dataclass

import pytest
import requests

HERE = os.path.dirname(os.path.abspath(__file__))
KISS_TEST_DIR = os.path.dirname(HERE)


@dataclass
class Device:
    name: str            # "sx" / "lr"
    url: str             # base URL (no trailing slash)
    identity: str        # 16-hex identity id   (auto-discovered)
    address: str         # 32-hex address       (auto-discovered)
    password: str
    token_file: str      # path under tools/kiss-test/


# Bench layout. Identity id/address are auto-discovered from /api/info at
# session start so the suite survives full flash wipes - the user just
# keeps the display_name + password stable.
_DEVICES = [
    Device(
        name="sx",
        url="http://192.168.1.116",
        identity="",
        address="",
        password="sxtester2026",
        token_file=os.path.join(KISS_TEST_DIR, ".token"),
    ),
    Device(
        name="lr",
        url="http://192.168.1.118",
        identity="",
        address="",
        password="lrtester2026",
        token_file=os.path.join(KISS_TEST_DIR, ".lr-token"),
    ),
]


# Display-name mapping for auto-discovery. The first identity matching
# the expected display name (or, if absent, the first identity at all)
# wins. Lets the suite handle either the seeded "sx-tester" / "lr-tester"
# pair or a freshly-named identity post-wipe.
_DISPLAY_NAMES = {"sx": "sx-tester", "lr": "lr-tester"}


def _discover(dev: Device) -> None:
    """Populate dev.identity + dev.address from /api/info if empty."""
    if dev.identity and dev.address:
        return
    r = requests.get(f"{dev.url}/api/info", timeout=DEFAULT_TIMEOUT)
    r.raise_for_status()
    idents = r.json().get("identities", [])
    if not idents:
        raise RuntimeError(
            f"{dev.name}: no identity provisioned on {dev.url}. "
            "Press the button + POST /api/identities to create one."
        )
    target = _DISPLAY_NAMES.get(dev.name)
    pick = None
    for i in idents:
        if target and i.get("display_name") == target:
            pick = i
            break
    if not pick:
        pick = idents[0]
    dev.identity = pick["id"]
    dev.address  = pick["address"]


DEFAULT_TIMEOUT = 15  # Some endpoints (paths/lookup over slow LoRa) take >5 s.


def _login(dev: Device) -> str:
    """Fresh bearer token from /api/auth/login. Cached on disk too."""
    _discover(dev)
    r = requests.post(
        f"{dev.url}/api/auth/login",
        json={"identity_id": dev.identity, "password": dev.password},
        timeout=DEFAULT_TIMEOUT,
    )
    r.raise_for_status()
    tok = r.json()["token"]
    with open(dev.token_file, "w") as f:
        f.write(tok)
    return tok


# KISS_TEST_DEVICES=sx (comma-separated names) limits the suite to a
# subset of the rig, e.g. when the other device is reserved for
# unrelated work. Tests whose fixtures need an excluded device fail
# their setup; deselect them for such runs (-m "not lora", -k ...).
_only = os.environ.get("KISS_TEST_DEVICES")
if _only:
    _names = {n.strip() for n in _only.split(",") if n.strip()}
    _DEVICES[:] = [d for d in _DEVICES if d.name in _names]


@pytest.fixture(scope="session")
def devices() -> list[Device]:
    return list(_DEVICES)


@pytest.fixture
def tokens(devices) -> dict[str, str]:
    """Function-scoped (NOT session-scoped). AuthTokens caps tokens at
    MAX_PER_ACCOUNT=4 per identity; if we shared one token across
    every test, the first test that does its own explicit login (like
    test_login_success_returns_token) plus any concurrent SPA / bidir
    session would race past the cap and evict our fixture's token,
    leaving subsequent tests with 401s. Per-test login is cheap (~50 ms
    each) and avoids the eviction trap entirely."""
    out: dict[str, str] = {}
    for d in devices:
        out[d.name] = _login(d)
    return out


@pytest.fixture
def session_factory(tokens):
    """A factory: session_factory("sx") → requests.Session with token set."""
    def make(name: str) -> requests.Session:
        s = requests.Session()
        s.headers.update({"Authorization": f"Bearer {tokens[name]}"})
        return s
    return make


@pytest.fixture
def sx(session_factory, devices):
    """Authenticated session + device tuple for the SX device."""
    return session_factory("sx"), next(d for d in devices if d.name == "sx")


@pytest.fixture
def lr(session_factory, devices):
    return session_factory("lr"), next(d for d in devices if d.name == "lr")


# Helpers for assertions ------------------------------------------------

def assert_has_keys(obj: dict, keys: list[str], path: str = "<root>"):
    """Assert every key in `keys` is present in `obj` and not None."""
    for k in keys:
        assert k in obj, f"{path}: missing key {k!r} - got {list(obj.keys())}"
        assert obj[k] is not None, f"{path}.{k} is None"


def assert_type(obj, t, name: str):
    assert isinstance(obj, t), f"{name}: expected {t.__name__}, got {type(obj).__name__}"


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "lora: end-to-end tests that transmit over the shared LoRa radio "
        "environment and take minutes; deselect with -m 'not lora'")
