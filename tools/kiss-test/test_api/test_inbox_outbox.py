"""Inbox / outbox / state shape + ordering.

Flat token-scoped routes: /api/state, /api/inbox, /api/outbox (the caller
identity comes from the bearer token).
"""
from conftest import assert_has_keys, assert_type

# Valid outbound status values, per LXMF::outbox_status_name.
_OUTBOX_STATUS = (
    "queued", "sent", "delivered", "failed", "finding_route", "generating_stamp",
)


def test_state_shape(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/state", timeout=10)
    assert r.status_code == 200
    body = r.json()
    assert_has_keys(body, ["identity", "conversations", "announces", "paths"])


def test_conversations_messages_shape(sx):
    s, d = sx
    body = s.get(f"{d.url}/api/state", timeout=10).json()
    for conv in body.get("conversations", []):
        assert_has_keys(conv, ["peer", "last_ts", "last_in",
                               "last_body", "messages"])
        for m in conv["messages"][:5]:
            assert_has_keys(m, ["seq", "ts", "boot_epoch", "received_ms",
                                "title", "body", "in", "sig_ok", "status"])
            assert_type(m["in"], bool, "msg.in")
            assert m["status"] in _OUTBOX_STATUS


def test_inbox_endpoint_returns_messages_array(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/inbox", timeout=10)
    assert r.status_code == 200
    body = r.json()
    assert "messages" in body
    assert isinstance(body["messages"], list)


def test_outbox_endpoint_returns_messages_array(sx):
    s, d = sx
    r = s.get(f"{d.url}/api/outbox", timeout=10)
    assert r.status_code == 200
    body = r.json()
    assert "messages" in body
    assert isinstance(body["messages"], list)
    assert "next_since" in body
