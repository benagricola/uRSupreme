"""WebSocket /api/ws stream contract.

The realtime event channel for the SPA. Replaces the SSE short-poll.
Auth via `?token=<bearer>&identity_id=<id>` query string. First frame
back is always `{"type":"hello", ...}`.
"""
import json
import time

import pytest
import websocket   # `websocket-client` package


def _ws_url(d, token: str) -> str:
    base = d.url.replace("http://", "ws://").replace("https://", "wss://")
    return f"{base}/api/ws?token={token}&identity_id={d.identity}"


def _connect(d, token: str, timeout: float = 5) -> "websocket.WebSocket":
    ws = websocket.create_connection(_ws_url(d, token), timeout=timeout)
    return ws


def _drain(ws, max_seconds: float = 4.0) -> list[dict]:
    """Pull frames until the connection idles for max_seconds."""
    out: list[dict] = []
    ws.settimeout(max_seconds)
    deadline = time.time() + max_seconds
    while time.time() < deadline:
        try:
            frame = ws.recv()
        except (websocket.WebSocketTimeoutException, OSError):
            break
        if not frame:
            continue
        try:
            out.append(json.loads(frame))
        except json.JSONDecodeError:
            out.append({"_raw": frame})
    return out


def test_ws_handshake_and_hello(sx, tokens):
    _, d = sx
    ws = _connect(d, tokens["sx"], timeout=5)
    try:
        first = json.loads(ws.recv())
    finally:
        ws.close()
    assert first.get("type") == "hello"
    assert first.get("identity_id") == d.identity
    assert "now_ms" in first


def test_ws_rejects_missing_token(sx):
    _, d = sx
    base = d.url.replace("http://", "ws://")
    with pytest.raises((websocket.WebSocketBadStatusException,
                        websocket.WebSocketException)):
        websocket.create_connection(f"{base}/api/ws", timeout=5)


def test_ws_rejects_bad_token(sx):
    _, d = sx
    base = d.url.replace("http://", "ws://")
    with pytest.raises((websocket.WebSocketBadStatusException,
                        websocket.WebSocketException)):
        websocket.create_connection(
            f"{base}/api/ws?token=deadbeefdeadbeef&identity_id={d.identity}",
            timeout=5)


def test_ws_ping_pong(sx, tokens):
    _, d = sx
    ws = _connect(d, tokens["sx"], timeout=5)
    try:
        ws.recv()  # hello
        ws.send('{"type":"ping"}')
        # Drain whatever the server pushes (sensor_update, system_update,
        # etc.) until the pong response lands. Cap at 20 frames to avoid
        # an infinite loop on a broken implementation.
        ws.settimeout(5)
        pong = None
        for _ in range(20):
            frame = json.loads(ws.recv())
            if frame.get("type") == "pong":
                pong = frame
                break
        assert pong is not None, "no pong reply within 20 frames"
    finally:
        ws.close()


def test_ws_announce_seen_after_peer_announce(sx, lr, tokens):
    """SX subscribes via WS. LR fires an announce. SX should receive an
    `announce_seen` event identifying the LR address within ~15 s."""
    _, dsx = sx
    s_lr, dlr = lr
    ws = _connect(dsx, tokens["sx"], timeout=5)
    try:
        ws.recv()  # hello
        # Trigger announce on LR.
        s_lr.post(f"{dlr.url}/api/identities/{dlr.identity}/announce",
                  timeout=10)
        events = _drain(ws, max_seconds=15.0)
    finally:
        ws.close()
    announces = [e for e in events if e.get("type") == "announce_seen"]
    assert announces, \
        f"no announce_seen after LR announce; got {[e.get('type') for e in events]}"
    assert any(dlr.address in json.dumps(e) for e in announces), \
        f"announce_seen events present but none reference {dlr.address}"
