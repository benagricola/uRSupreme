"""Attachment upload / send / download paths."""
import hashlib
import io
import os

import pytest

import requests

try:
    from PIL import Image
    HAVE_PIL = True
except ImportError:
    HAVE_PIL = False


def _tiny_jpeg(n_bytes: int = 4096) -> bytes:
    """Return a real JPEG at-or-near the requested byte budget."""
    if HAVE_PIL:
        img = Image.new("RGB", (160, 120), (40, 80, 120))
        out = io.BytesIO()
        img.save(out, format="JPEG", quality=70)
        return out.getvalue()
    # Fallback: a 100-byte synthetic blob (any bytes work for the
    # upload/download path; only the image-decoder cares about JPEG
    # validity, and the test devices don't decode).
    return os.urandom(n_bytes)


# ---- upload-side checks ----------------------------------------------

def test_upload_round_trip(sx):
    s, d = sx
    blob = _tiny_jpeg()
    r = s.post(
        f"{d.url}/api/identities/{d.identity}/attachment/upload",
        headers={"X-Total-Length": str(len(blob))},
        files={"file": ("test.jpg", blob, "image/jpeg")},
        timeout=20,
    )
    assert r.status_code == 200, r.text
    body = r.json()
    assert body["total_bytes"] == len(blob)
    assert body["backend"] in ("psram", "sd")
    assert isinstance(body["staging_id"], int) and body["staging_id"] > 0


def test_upload_missing_total_header_400(sx):
    s, d = sx
    blob = _tiny_jpeg()
    r = s.post(
        f"{d.url}/api/identities/{d.identity}/attachment/upload",
        files={"file": ("test.jpg", blob, "image/jpeg")},
        timeout=20,
    )
    assert r.status_code == 400
    assert "X-Total-Length" in r.text


def test_upload_invalid_total_header_400(sx):
    s, d = sx
    blob = _tiny_jpeg()
    r = s.post(
        f"{d.url}/api/identities/{d.identity}/attachment/upload",
        headers={"X-Total-Length": "0"},
        files={"file": ("test.jpg", blob, "image/jpeg")},
        timeout=20,
    )
    assert r.status_code == 400


def test_upload_too_large_400(sx):
    """Asking for 64 MiB (above ABSOLUTE_MAX_BYTES=32 MiB) must be
    refused at the header-parse stage with a clean 400."""
    s, d = sx
    blob = _tiny_jpeg()
    r = s.post(
        f"{d.url}/api/identities/{d.identity}/attachment/upload",
        headers={"X-Total-Length": str(64 * 1024 * 1024)},
        files={"file": ("big.jpg", blob, "image/jpeg")},
        timeout=20,
    )
    assert r.status_code == 400
    assert "ceiling" in r.text.lower() or "abs" in r.text.lower()


# ---- send + receive --------------------------------------------------
# (full bidir is covered in bidir_attachment_test.py at the parent
# directory; here we just verify the /send-with-staging contract.)

def test_send_with_staging_id(sx, lr):
    """SX uploads + sends; LR doesn't actually need to receive for this
    test (covered elsewhere). We just confirm the /send accepts the
    staging_id and returns 202. Triggers an LR announce first so the SX
    has a public key for the recipient."""
    import time as _time
    s_sx, dsx = sx
    s_lr, dlr = lr
    # Prime: LR fires an announce so SX learns the public key.
    s_lr.post(f"{dlr.url}/api/identities/{dlr.identity}/announce", timeout=10)
    _time.sleep(3)
    blob = _tiny_jpeg()
    up = s_sx.post(
        f"{dsx.url}/api/identities/{dsx.identity}/attachment/upload",
        headers={"X-Total-Length": str(len(blob))},
        files={"file": ("t.jpg", blob, "image/jpeg")},
        timeout=20,
    ).json()
    r = s_sx.post(
        f"{dsx.url}/api/identities/{dsx.identity}/send",
        json={
            "to": dlr.address,
            "content": "regression-test",
            "attachments": [{"tag": 6, "staging_id": up["staging_id"],
                             "filename": "t.jpg", "mime": "image/jpeg",
                             "ext": "jpg"}],
        },
        timeout=30,
    )
    assert r.status_code in (200, 202), r.text
    body = r.json()
    assert "queued_seq" in body
    assert body["status"] in ("queued", "sent", "delivered")


def test_send_missing_staging_id_400(sx, lr):
    s_sx, dsx = sx
    _, dlr = lr
    r = s_sx.post(
        f"{dsx.url}/api/identities/{dsx.identity}/send",
        json={"to": dlr.address, "content": "x",
              "attachments": [{"tag": 6}]},
        timeout=10,
    )
    assert r.status_code == 400


# ---- download --------------------------------------------------------

def test_download_unknown_404(sx):
    s, d = sx
    r = s.get(
        f"{d.url}/api/identities/{d.identity}/attachment/download/"
        f"{'0' * 64}_06_0.bin",
        timeout=10,
    )
    assert r.status_code == 404
