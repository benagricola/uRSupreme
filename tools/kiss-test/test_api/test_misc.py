"""Endpoints not covered by the per-area files but still important
for the WebServer migration baseline.
"""
import json
import pytest
import websocket


def _ws_hello(d, token: str) -> dict:
    """Fetch the WS hello frame - single round-trip for the system
    snapshot the SPA renders pre-popover."""
    base = d.url.replace("http://", "ws://")
    sock = websocket.create_connection(
        f"{base}/api/ws?token={token}&identity_id={d.identity}", timeout=5)
    try:
        return json.loads(sock.recv())
    finally:
        sock.close()


def test_storage_migrate_sd_absent_returns_409(sx, tokens):
    """When no SD card is present, the migration endpoint must refuse
    with a structured error - not a 500."""
    s, d = sx
    h = _ws_hello(d, tokens["sx"])
    if h["storage"]["sd"]["present"]:
        pytest.skip("SD card is inserted on the bench device - skip the absent-path test")
    r = s.post(f"{d.url}/api/storage/migrate_flash_to_sd", timeout=10)
    assert r.status_code == 409
    body = r.json()
    assert body.get("error") == "sd_absent"
    assert "message" in body


def test_inbox_config_get(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/inbox_config", timeout=15)
    assert r.status_code == 200
    body = r.json()
    # Schema: { default_retention: { kind, value } }. kind is one of
    # none | time | count (LXMF::retention_kind_name).
    assert "default_retention" in body
    dr = body["default_retention"]
    assert "kind" in dr and "value" in dr
    assert dr["kind"] in ("none", "time", "count")


def test_inbox_config_post_roundtrip(sx):
    s, d = sx
    before = s.get(f"{d.url}/api/inbox_config", timeout=15).json()["default_retention"]
    # Set a known retention, read it back, then restore the original.
    r = s.post(f"{d.url}/api/inbox_config",
               json={"default_retention": {"kind": "count", "value": 25}},
               timeout=15)
    assert r.status_code == 200
    after = r.json()["default_retention"]
    assert after["kind"] == "count" and after["value"] == 25
    # Restore.
    s.post(f"{d.url}/api/inbox_config",
           json={"default_retention": {"kind": before["kind"],
                                       "value": before["value"]}},
           timeout=15)


def test_sensors_config_post_roundtrip(sx, tokens):
    """Toggle the environment sensor enable on/off + back."""
    s, d = sx
    h = _ws_hello(d, tokens["sx"])
    env = h.get("sensors", {}).get("environment")
    if not env or not env.get("available"):
        pytest.skip("environment sensor not available on this board")
    initial = env.get("enabled", True)
    interval = max(1, int(env.get("interval_ms", 60000) / 1000))
    r = s.post(f"{d.url}/api/sensors/config",
               json={"sensor": "environment", "enabled": not initial,
                     "interval_s": interval}, timeout=15)
    assert r.status_code == 200
    # Restore.
    s.post(f"{d.url}/api/sensors/config",
           json={"sensor": "environment", "enabled": initial,
                 "interval_s": interval}, timeout=15)


def test_paths_lookup_unknown(sx):
    """Looking up an all-zero (unknown) destination returns either 200
    with empty result or 404."""
    s, d = sx
    r = s.post(f"{d.url}/api/paths/lookup",
               json={"to": "0" * 32}, timeout=15)
    assert r.status_code in (200, 404)


def test_paths_estimate(sx, lr):
    """We know the SX has a path to LR (the bidir test sets it up)."""
    s_sx, dsx = sx
    _, dlr = lr
    r = s_sx.get(
        f"{dsx.url}/api/paths/estimate?to={dlr.address}&bytes=1024",
        timeout=10,
    )
    assert r.status_code == 200
    body = r.json()
    assert "kind" in body
    # Canonical kind values, per handle_path_estimate:
    #   local   - destination matches an identity on this device
    #   path    - known route via Transport
    #   unknown - no path table entry
    assert body["kind"] in ("local", "path", "unknown")


def test_radio_get(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/radio", timeout=15)
    assert r.status_code == 200
    body = r.json()
    # have_conf may be false on a fresh device - that's still a 200.
    assert "have_conf" in body
