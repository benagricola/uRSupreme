"""Identity routes (flat, token-scoped - the caller identity comes from
the bearer token, there is no {id} path segment):
  GET  /api/identity            - caller's identity detail
  POST /api/identity/settings   - update caller's settings
  POST /api/announce            - announce the caller's identity
"""
from conftest import assert_has_keys, assert_type


def test_identity_get_shape(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/identity", timeout=15)
    assert r.status_code == 200
    body = r.json()
    assert_has_keys(body, ["id", "display_name", "address",
                           "announce_interval_ms",
                           "persist_outbound_attachments",
                           "inbox_size", "outbox_size",
                           "next_announce_in_ms"])
    assert body["id"] == d.identity
    assert body["address"] == d.address
    assert_type(body["persist_outbound_attachments"], bool, "persist_outbound_attachments")


def test_identity_unauthenticated_401(devices):
    """GET /api/identity is token-scoped: no token => 401 (there is no
    cross-identity access by path any more)."""
    import requests
    d = devices[0]
    r = requests.get(f"{d.url}/api/identity", timeout=5)
    assert r.status_code == 401


def test_settings_persist_outbound_toggle_roundtrip(sx):
    """Flip persist_outbound_attachments, read back, restore."""
    s, d = sx
    before = s.get(f"{d.url}/api/identity", timeout=15).json()
    initial = before["persist_outbound_attachments"]
    new_val = not initial
    r = s.post(f"{d.url}/api/identity/settings",
               json={"persist_outbound_attachments": new_val}, timeout=15)
    assert r.status_code == 200
    after = r.json()
    assert after["persist_outbound_attachments"] == new_val
    # Restore.
    s.post(f"{d.url}/api/identity/settings",
           json={"persist_outbound_attachments": initial}, timeout=15)


def test_announce_endpoint(sx):
    s, d = sx
    r = s.post(f"{d.url}/api/announce", timeout=15)
    assert r.status_code == 200
