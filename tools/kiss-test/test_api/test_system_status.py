"""System snapshot (storage, sensors, battery, outbound_caps, rtc) is
delivered via the WS `hello` frame on connect and refreshed by
`system_update` events. The REST endpoint was retired — these tests
pin the WS contract.

Each datum still has exactly one home. Things on /api/info
(radio/wifi/transport/battery summary for the topbar icon) must NOT
appear in the system block here.
"""
import json
import time

import pytest
import websocket
from conftest import assert_has_keys, assert_type


def _hello(d, token: str, identity: str) -> dict:
    base = d.url.replace("http://", "ws://")
    sock = websocket.create_connection(
        f"{base}/api/ws?token={token}&identity_id={identity}", timeout=5)
    try:
        return json.loads(sock.recv())
    finally:
        sock.close()


def _drain(d, token: str, identity: str, max_seconds: float = 35.0,
           wanted_type: str = "system_update") -> dict | None:
    base = d.url.replace("http://", "ws://")
    sock = websocket.create_connection(
        f"{base}/api/ws?token={token}&identity_id={identity}", timeout=5)
    try:
        sock.settimeout(max_seconds)
        deadline = time.time() + max_seconds
        while time.time() < deadline:
            try:
                frame = json.loads(sock.recv())
            except (websocket.WebSocketTimeoutException, OSError):
                return None
            if frame.get("type") == wanted_type:
                return frame
        return None
    finally:
        sock.close()


def test_hello_carries_system_snapshot(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    assert h["type"] == "hello"
    # Must include the system blocks the SPA renders pre-popover.
    assert_has_keys(h, ["storage", "sensors", "outbound_caps", "rtc"])


def test_hello_does_not_duplicate_info(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    for k in ("radio", "transport", "wifi", "uptime_ms",
              "fw_version", "kiss_serial_output"):
        assert k not in h, f"WS hello should not carry {k!r} (lives on /api/info)"


def test_hello_storage_shape(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    st = h["storage"]
    assert_has_keys(st["flash"], ["total_bytes", "free_bytes", "used_bytes"])
    assert "sd" in st
    assert "present" in st["sd"]
    if st["sd"]["present"]:
        assert_has_keys(st["sd"], ["card_type", "total_bytes", "used_bytes"])


def test_hello_outbound_caps_shape(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    oc = h["outbound_caps"]
    assert_has_keys(oc, ["max_bytes", "backend", "psram_free_bytes",
                         "flash_free_bytes", "sd_present"])
    assert oc["backend"] in ("psram", "flash", "sd")
    assert_type(oc["max_bytes"], int, "outbound_caps.max_bytes")
    assert oc["max_bytes"] > 0


def test_hello_battery_detail_shape(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    if "battery" not in h:
        pytest.skip("no PMU on this device")
    b = h["battery"]
    # Single block now (percent/state + detail in one place).
    assert "voltage_v" in b
    assert_type(b["voltage_v"], (int, float), "battery.voltage_v")
    assert "vbus_present" in b
    if b["vbus_present"]:
        assert "vbus_voltage_v" in b


def test_hello_sensors_block(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    sensors = h["sensors"]
    assert "gps" in sensors
    g = sensors["gps"]
    assert_has_keys(g, ["available", "valid", "fix_age_ms", "last_byte_ms",
                        "powered", "pulse_state", "model"])
    assert g["pulse_state"] in ("idle", "acquiring")
    assert_type(g["powered"], bool, "sensors.gps.powered")
    assert g["model"], "sensors.gps.model must be non-empty"


def test_hello_rtc_block(sx, tokens):
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    r = h["rtc"]
    assert "available" in r
    if r["available"]:
        assert "vl_set" in r
        assert "unix_ms" in r


def test_hello_carries_clock(sx, tokens):
    """Wall-clock + boot-epoch anchor on the same hello frame."""
    _, d = sx
    h = _hello(d, tokens["sx"], d.identity)
    c = h.get("clock") or {}
    for k in ("now_ms", "unix_ms", "calibrated", "source", "current_boot_epoch"):
        assert k in c, f"hello.clock missing {k!r} — got {list(c.keys())}"


def test_system_status_endpoint_gone(sx):
    """The REST endpoint is retired — anyone polling it must migrate
    to the WS hello/system_update contract."""
    s, d = sx
    r = s.get(f"{d.url}/api/system_status", timeout=5)
    assert r.status_code == 404, \
        f"/api/system_status should be 404, got {r.status_code}"


def test_periodic_system_update_arrives(sx, tokens):
    """Within ~35 s of connecting, a `system_update` frame should
    land. Carries the same payload shape the hello frame did, under
    the `payload` key."""
    _, d = sx
    ev = _drain(d, tokens["sx"], d.identity, max_seconds=35.0,
                wanted_type="system_update")
    assert ev is not None, "no system_update frame within 35s"
    p = ev["payload"]
    assert "storage" in p
    assert "sensors" in p
    assert "outbound_caps" in p
