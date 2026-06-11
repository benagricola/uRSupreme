"""GET /api/info - lightweight always-polled endpoint covering
radio + transport + WiFi + a battery summary for the topbar icon.

Storage / sensors / outbound_caps / battery detail must NOT appear
here - those belong to /api/system/status. Each datum has one home.
"""
from conftest import assert_has_keys, assert_type


def test_info_top_level_keys(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/info", timeout=15)
    assert r.status_code == 200
    body = r.json()
    # The full top-level surface this endpoint owns.
    assert_has_keys(body, ["radio", "transport", "wifi", "uptime_ms",
                           "fw_version", "kiss_serial_output"])


def test_info_does_not_duplicate_system_status(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/info", timeout=15).json()
    # These live on /api/system/status - must NOT be duplicated here.
    for k in ("storage", "outbound_caps", "sensors"):
        assert k not in body, f"/api/info should not carry {k!r} (lives on system_status)"


def test_info_radio_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/info", timeout=15).json()
    r = body["radio"]
    # Required regardless of online state.
    assert_has_keys(r, ["have_conf", "model", "online"])
    assert_type(r["have_conf"], bool, "radio.have_conf")
    assert_type(r["online"], bool, "radio.online")
    # When configured, these are required.
    if r["have_conf"]:
        for k in ("frequency", "bandwidth", "spreading_factor",
                  "coding_rate", "tx_power"):
            assert k in r, f"radio.{k} missing"


def test_info_battery_summary_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/info", timeout=15).json()
    if "battery" not in body:
        # Non-PMU board build - acceptable.
        return
    b = body["battery"]
    # Summary surface only: percent + state. Voltage / slope / vbus
    # belong on system_status.
    assert set(b.keys()) <= {"percent", "state"}, \
        f"battery summary should only carry percent+state, got {list(b.keys())}"
    assert b["state"] in ("charging", "discharging", "charged", "absent", "unknown")
    assert isinstance(b["percent"], int)


def test_info_transport_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/info", timeout=15).json()
    t = body["transport"]
    assert "enabled" in t
    assert_type(t["enabled"], bool, "transport.enabled")


def test_info_unauthenticated_ok(sx):
    """The status indicator polls /api/info before login - must succeed
    without a token."""
    s, d = sx
    r = s.get(f"{d.url}/api/info",
              headers={"Authorization": ""},  # strip the token
              timeout=15)
    # Either explicitly unauthed (most likely) OR auth-not-required (200).
    assert r.status_code == 200, f"got {r.status_code}: {r.text[:200]}"
