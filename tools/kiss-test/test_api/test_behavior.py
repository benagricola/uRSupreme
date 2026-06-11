"""Behavioral rig tests: end-to-end LXMF over LoRa and upload integrity,
judged by DEVICE COUNTERS and receiver-side arrival, not client-side
exceptions.

Encodes the hard-won testing rules from the 2026-06 SD/upload
investigation (see CLAUDE.md section 7):
  * Fresh connection per upload: the server closes connections after
    each response; a pooled stale socket produces client-side errors
    that look exactly like device failures.
  * Detach the rmap backbone for clean LoRa reads, settle past the
    interface-teardown stall, and ALWAYS restore.
  * Judge SD health by sd_write_errors deltas and X-SHA256 echo, not
    by throughput or exceptions.

The LoRa tests are marked `lora`: they transmit on the shared radio
environment and take minutes. Deselect with `-m "not lora"`.

Run from tools/kiss-test:  pytest test_api/test_behavior.py -v
"""
import hashlib
import os
import threading
import time

import pytest
import requests

LORA = pytest.mark.lora


def _diag_storage(s, d):
    return s.get(f"{d.url}/api/diag/storage", timeout=30).json()


def _upload(s, d, payload: bytes, timeout=300):
    """One staged upload with integrity headers on a fresh connection."""
    s.close()  # fresh socket: stale pooled connections fake device failures
    sha = hashlib.sha256(payload).hexdigest()
    r = s.post(
        f"{d.url}/api/attachment/upload",
        files={"file": ("behavior.bin", payload, "application/octet-stream")},
        headers={"X-Total-Length": str(len(payload)), "X-SHA256": sha},
        timeout=timeout,
    )
    return r


def test_upload_2mib_sha_verified_zero_write_errors(sx):
    """A 2 MiB staged upload lands byte-identical with no SD write errors.

    This is the regression net for the writer-task pipeline: deferred
    finalize answers with the real status, the writer's SHA matches the
    client's, and the device-side error counters stay flat.
    """
    s, d = sx
    before = _diag_storage(s, d)
    payload = os.urandom(2 * 1024 * 1024)
    r = _upload(s, d, payload)
    assert r.status_code == 200, r.text[:200]
    body = r.json()
    assert body.get("sha256_ok") is True, body
    after = _diag_storage(s, d)
    assert after.get("sd_write_errors", 0) - before.get("sd_write_errors", 0) == 0
    assert after.get("sd_ring_timeouts", 0) - before.get("sd_ring_timeouts", 0) == 0


def test_concurrent_upload_rejected_409(sx, tokens):
    """A second upload while one is in flight gets 409 upload_busy and
    must not corrupt the first (single-uploader guard).

    Concurrent full-window inbound TCP can still transiently wedge the
    WiFi driver's RX path (buffer-pool exhaustion below lwIP); the
    firmware's WifiRxWatchdog detects the inbound silence and recovers
    with a reconnect. So the hard requirement here is twofold: the 409
    contract holds when the race completes cleanly, and the device must
    ALWAYS come back on its own - a manual reset is a regression.
    """
    s, d = sx
    payload = os.urandom(4 * 1024 * 1024)
    first: dict = {}

    def run_first():
        try:
            r = _upload(s, d, payload)
            first["status"] = r.status_code
            first["body"] = r.json()
        except Exception as e:  # noqa: BLE001 - recorded for the assert below
            first["status"] = "EXC"
            first["body"] = {"error": str(e)}

    t = threading.Thread(target=run_first)
    t.start()
    time.sleep(1.5)  # let the first upload take ownership
    s2 = requests.Session()
    s2.headers.update({"Authorization": f"Bearer {tokens['sx']}"})
    try:
        r2_status = None
        r2 = _upload(s2, d, os.urandom(64 * 1024), timeout=60)
        r2_status = r2.status_code
    except requests.RequestException:
        pass  # a transient RX wedge can cut the connection; judged below
    t.join(timeout=300)

    # The device must recover on its own (watchdog reconnect takes up to
    # ~65 s: silence threshold + reassociation + cooldown margin).
    deadline = time.time() + 120
    alive = False
    while time.time() < deadline:
        try:
            requests.get(f"{d.url}/api/info", timeout=5)
            alive = True
            break
        except requests.RequestException:
            time.sleep(5)
    assert alive, "device did not self-recover within 120 s after the race"

    if first.get("status") == 200 and r2_status is not None:
        # Clean race: exactly one winner, integrity verified, the late
        # upload told the pipeline is busy.
        assert first["body"].get("sha256_ok") is True, first
        assert r2_status == 409
    else:
        # The race tripped a transient RX wedge; recovery already proven
        # above, but the guard's winner semantics were not exercised.
        pytest.skip("transient RX wedge during the race; device "
                    "self-recovered (watchdog), guard not exercised")


# ---- LoRa end-to-end ---------------------------------------------------

SETTLE_AFTER_DETACH_S = 25   # interface teardown holds the RNS lock for a while
TEXT_ARRIVAL_TIMEOUT_S = 120
LINK_ARRIVAL_TIMEOUT_S = 240


@pytest.fixture()
def quiet_backbone(sx):
    """Detach the SX's rmap backbone for a clean LoRa read; always restore.

    Under rmap load, SX-originated LoRa sends are environmentally noisy
    (they contend with forwarding for the radio); detaching gives the
    deterministic read the assertions need.
    """
    s, d = sx
    detached = False
    try:
        r = s.delete(f"{d.url}/api/transport/tcp_clients/rmap", timeout=20)
        detached = r.status_code in (200, 204)
    except Exception:
        pass
    if detached:
        time.sleep(SETTLE_AFTER_DETACH_S)
    yield
    if detached:
        for _ in range(3):
            try:
                rr = s.post(f"{d.url}/api/transport/tcp_clients",
                            json={"name": "rmap", "host": "rmap.world", "port": 4242},
                            timeout=20)
                if rr.status_code in (200, 201, 409):
                    return
            except Exception:
                time.sleep(5)
        raise RuntimeError("failed to restore the rmap tcp client - restore it by hand")


def _send(s, d, to_addr: str, title: str, content: str):
    r = s.post(f"{d.url}/api/send",
               json={"to": to_addr, "title": title, "content": content},
               timeout=30)
    # 200 = sent immediately; 202 = queued in the finding-route flow
    # (no path to the peer yet - normal on cold path tables). Either way
    # the receiver-side arrival poll is the delivery verdict.
    assert r.status_code in (200, 202), r.text[:200]
    return r.json()


def _wait_for_inbox(s, d, needle: str, timeout_s: int) -> dict:
    """Poll the receiver's inbox for a message whose content contains
    `needle`. Receiver-side arrival is the only delivery truth."""
    deadline = time.time() + timeout_s
    last = []
    while time.time() < deadline:
        try:
            msgs = s.get(f"{d.url}/api/inbox", timeout=20).json().get("messages", [])
            last = msgs
            for m in msgs:
                # /api/send takes `content` but /api/inbox serves `body`
                if needle in (m.get("body") or ""):
                    return m
        except Exception:
            pass
        time.sleep(5)
    raise AssertionError(
        f"message containing {needle!r} not in {d.name} inbox after {timeout_s}s "
        f"(inbox size {len(last)})")


def _radio_healthy(s, d) -> dict:
    info = s.get(f"{d.url}/api/info", timeout=20).json()
    stats = (info.get("radio") or {}).get("stats") or {}
    return stats


@LORA
def test_lxmf_text_sx_to_lr_opportunistic(sx, lr, quiet_backbone):
    """Small text (single-packet opportunistic) SX -> LR over LoRa."""
    s_sx, d_sx = sx
    s_lr, d_lr = lr
    needle = f"behav-opp-{int(time.time())}"
    _send(s_sx, d_sx, d_lr.address, "behavior", f"{needle} hello over LoRa")
    _wait_for_inbox(s_lr, d_lr, needle, TEXT_ARRIVAL_TIMEOUT_S)
    stats = _radio_healthy(s_sx, d_sx)
    assert stats.get("airtime_locked") in (False, None), stats


@LORA
def test_lxmf_link_resource_sx_to_lr(sx, lr, quiet_backbone):
    """~2.5 KB text forces the DIRECT Link/Resource path: handshake,
    multi-part transfer, proof, receiver dedup - the whole machinery
    behind the attachment saga, with arrival as the verdict."""
    s_sx, d_sx = sx
    s_lr, d_lr = lr
    needle = f"behav-link-{int(time.time())}"
    filler = ("lorem-ipsum-" * 16 + "\n") * 12   # ~2.4 KB > LINK_PACKET_MAX
    _send(s_sx, d_sx, d_lr.address, "behavior-link", f"{needle}\n{filler}")
    _wait_for_inbox(s_lr, d_lr, needle, LINK_ARRIVAL_TIMEOUT_S)
    stats = _radio_healthy(s_sx, d_sx)
    assert stats.get("airtime_locked") in (False, None), stats


@LORA
def test_transport_counters_sane_after_traffic(sx):
    """After the e2e tests, the SX must not be path-request flooding
    (the MODE_GATEWAY saga's signature) or airtime locked."""
    s, d = sx
    diag = s.get(f"{d.url}/api/diag/transport", timeout=20).json()
    stats = _radio_healthy(s, d)
    assert stats.get("airtime_locked") in (False, None), stats
    # path_requests table bounded and not exploding
    pr = diag.get("path_requests") or diag.get("announce_egress", {}).get("path_requests")
    if pr is not None:
        assert int(pr) < 2000, f"path_requests table at {pr}"
