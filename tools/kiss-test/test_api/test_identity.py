"""Identity routes:
  GET  /api/identities
  GET  /api/identities/{id}
  POST /api/identities/{id}/settings
  POST /api/identities/{id}/announce
"""
from conftest import assert_has_keys, assert_type


# NOTE: GET /api/identities (no id, list-all) is not currently exposed —
# only POST (create) and GET /{id}. Migration is a good time to add it
# if the SPA grows multi-identity awareness.


def test_identity_get_shape(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/identities/{d.identity}", timeout=15)
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


def test_identity_unknown_404(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/identities/{'0' * 16}", timeout=15)
    # Caller != requested → forbidden, since auth scopes to caller's id.
    assert r.status_code in (403, 404)


def test_settings_persist_outbound_toggle_roundtrip(sx):
    """Flip persist_outbound_attachments, read back, restore."""
    s, d = sx
    before = s.get(f"{d.url}/api/identities/{d.identity}", timeout=15).json()
    initial = before["persist_outbound_attachments"]
    new_val = not initial
    r = s.post(f"{d.url}/api/identities/{d.identity}/settings",
               json={"persist_outbound_attachments": new_val}, timeout=15)
    assert r.status_code == 200
    after = r.json()
    assert after["persist_outbound_attachments"] == new_val
    # Restore.
    s.post(f"{d.url}/api/identities/{d.identity}/settings",
           json={"persist_outbound_attachments": initial}, timeout=15)


def test_announce_endpoint(sx):
    s, d = sx
    r = s.post(f"{d.url}/api/identities/{d.identity}/announce", timeout=15)
    assert r.status_code == 200
