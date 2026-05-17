"""GET /api/system_status — detailed status: storage, clock, sensors,
outbound_caps, battery detail.

Each datum has exactly one home. Things on /api/info (radio/wifi/transport/
battery summary) must NOT appear here.
"""
from conftest import assert_has_keys, assert_type


def test_system_status_top_level(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/system_status", timeout=15)
    assert r.status_code == 200
    body = r.json()
    assert_has_keys(body, ["storage", "clock", "sensors", "outbound_caps"])


def test_system_status_does_not_duplicate_info(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    for k in ("radio", "transport", "wifi", "uptime_ms",
              "fw_version", "kiss_serial_output"):
        assert k not in body, f"/api/system_status should not carry {k!r} (lives on /api/info)"


def test_storage_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    st = body["storage"]
    assert_has_keys(st["flash"], ["total_bytes", "free_bytes", "used_bytes"])
    assert "sd" in st
    assert "present" in st["sd"]
    if st["sd"]["present"]:
        assert_has_keys(st["sd"], ["card_type", "total_bytes", "used_bytes"])


def test_outbound_caps_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    oc = body["outbound_caps"]
    assert_has_keys(oc, ["max_bytes", "backend", "psram_free_bytes", "sd_present"])
    assert oc["backend"] in ("psram", "sd")
    assert_type(oc["max_bytes"], int, "outbound_caps.max_bytes")
    assert oc["max_bytes"] > 0


def test_battery_detail_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    if "battery" not in body:
        return
    b = body["battery"]
    # Detail block carries voltage / vbus / slope. Percent + state are
    # on /api/info, NOT here.
    for k in ("percent", "state"):
        assert k not in b, f"battery detail must not duplicate {k!r} (lives on /api/info)"
    assert "voltage_v" in b
    assert_type(b["voltage_v"], (int, float), "battery.voltage_v")
    assert "vbus_present" in b
    if b["vbus_present"]:
        assert "vbus_voltage_v" in b


def test_sensors_block(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    sensors = body["sensors"]
    # gps must always appear on Supreme builds.
    assert "gps" in sensors
    g = sensors["gps"]
    assert_has_keys(g, ["available", "valid", "fix_age_ms", "last_byte_ms",
                        "powered", "pulse_state"])
    assert g["pulse_state"] in ("idle", "acquiring")
    assert_type(g["powered"], bool, "sensors.gps.powered")


def test_clock_block(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/system_status", timeout=15).json()
    c = body["clock"]
    assert "calibrated" in c
    assert "unix_ms" in c
    assert "source" in c
    if c["calibrated"]:
        assert c["unix_ms"] > 1_700_000_000_000  # post-2023
