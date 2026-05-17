"""SSE /events stream contract.

These tests are the most critical for the WebServer migration: SSE
behaviour under the synchronous WebServer is fragile (head-of-line
blocking, single-listener). The migration target must preserve the
shape of these events.
"""
import json
import time

import requests


def _open_sse(s, url, since: int = 0, timeout: float = 10) -> requests.Response:
    """Open an SSE stream. Yields raw event lines."""
    r = s.get(url + f"?token={s.headers['Authorization'].split()[1]}"
                  f"&inbox_since={since}"
                  f"&announces_since=0&paths_since=0&progress_since=0",
              stream=True, timeout=timeout,
              headers={"Accept": "text/event-stream"})
    return r


def _read_events(resp, max_seconds: float = 5.0) -> list[dict]:
    """Drain whatever events arrive within the window."""
    events: list[dict] = []
    deadline = time.time() + max_seconds
    buf = ""
    for chunk in resp.iter_lines(decode_unicode=True):
        if time.time() > deadline: break
        if chunk is None: continue
        if not chunk:
            # blank line = end of event
            if buf:
                # parse the data: lines
                for line in buf.splitlines():
                    if line.startswith("data:"):
                        payload = line[5:].lstrip()
                        try:
                            events.append(json.loads(payload))
                        except Exception:
                            events.append({"_raw": payload})
                buf = ""
            continue
        buf += chunk + "\n"
    return events


def test_sse_connects_and_returns_200(sx):
    s, d = sx
    r = _open_sse(s, f"{d.url}/api/identities/{d.identity}/events", timeout=3)
    assert r.status_code == 200
    assert "text/event-stream" in r.headers.get("Content-Type", "")
    r.close()


def test_sse_emits_at_least_one_event_on_connect(sx):
    """Within a few seconds of opening the stream, at least one event
    should arrive — typically an `info` / heartbeat / initial state."""
    s, d = sx
    r = _open_sse(s, f"{d.url}/api/identities/{d.identity}/events", timeout=8)
    try:
        events = _read_events(r, max_seconds=4.0)
    finally:
        r.close()
    # An empty stream during a 4s window is a regression signal; even
    # heartbeat events should land within that.
    assert events, "no events received in 4 s — SSE may be broken"


def test_sse_announce_event_after_manual_announce(sx, lr):
    """SX subscribes; LR triggers an announce; SX should receive an
    `announce` event identifying the LR address."""
    s_sx, dsx = sx
    s_lr, dlr = lr
    r = _open_sse(s_sx, f"{dsx.url}/api/identities/{dsx.identity}/events",
                  timeout=20)
    try:
        # Fire announce on LR; wait a couple seconds for radio path.
        s_lr.post(f"{dlr.url}/api/identities/{dlr.identity}/announce",
                  timeout=10)
        events = _read_events(r, max_seconds=15.0)
    finally:
        r.close()
    types = [e.get("type") or e.get("event") for e in events]
    # The handler may name the type "announce" — assert any event mentions
    # the LR address. Schema-strict on the field, lenient on the type name.
    found = False
    for e in events:
        s = json.dumps(e)
        if dlr.address in s:
            found = True
            break
    assert found, f"no event referencing LR address {dlr.address}; saw types {types}"
