#!/usr/bin/env python3
"""Bidirectional image-attachment test driving the new /attachment/upload +
/send-with-staging-id pipeline (#130).

For each direction:
  1. Resize a sample JPEG down to a chosen byte budget (LoRa-friendly).
  2. POST the bytes as multipart to /api/identities/{id}/attachment/upload?total=N.
     → server reports back { staging_id, total_bytes, backend }.
  3. POST /api/identities/{id}/send with attachments=[{tag:6, staging_id, ...}].
  4. Poll receiver /state until a matching attachment appears in its inbox.
  5. Verify the receiver's GET /attachment/download/{filename} returns the
     same bytes we uploaded.
"""
import hashlib
import io
import os
import sys
import time

import requests
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
def _f(name): return open(os.path.join(HERE, name)).read().strip()

SX_URL  = 'http://192.168.1.116'
LR_URL  = 'http://192.168.1.118'

# Auto-discover the active identity from /api/info so the script
# survives full chip-erase wipes. Falls back to a fresh login with the
# bench tester passwords (memory: sxtester2026 / lrtester2026).
def _discover(url: str, pw: str):
    info = requests.get(f'{url}/api/info', timeout=5).json()
    if not info.get('identities'):
        raise RuntimeError(f'no identity on {url} - provision it first')
    iden = info['identities'][0]['id']
    addr = info['identities'][0]['address']
    tok = requests.post(f'{url}/api/auth/login',
                        json={'identity_id': iden, 'password': pw},
                        timeout=5).json()['token']
    return iden, addr, tok

SX_IDEN, SX_ADDR, SX_TOK = _discover(SX_URL, 'sxtester2026')
LR_IDEN, LR_ADDR, LR_TOK = _discover(LR_URL, 'lrtester2026')

SAMPLE_IMG = os.path.normpath(os.path.join(HERE, '..', '..', 'Documentation', 'rnfw_1.jpg'))
# LoRa-friendly: keep the on-wire payload small so the test finishes
# within ~minute, not hours. Resource layer handles up to ~1 MiB; we
# don't need to stress that here, just exercise the staging path.
TARGET_BYTES = 6 * 1024

def resize_to_budget(src_path: str, budget: int) -> bytes:
    """Iteratively scale + quality-drop a JPEG until it fits the byte budget."""
    img = Image.open(src_path).convert('RGB')
    for max_dim in (320, 256, 192, 160, 128):
        w, h = img.size
        scale = min(1.0, max_dim / max(w, h))
        nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
        thumb = img.resize((nw, nh), Image.LANCZOS)
        for q in (78, 65, 55, 45, 35, 25):
            out = io.BytesIO()
            thumb.save(out, format='JPEG', quality=q, optimize=True)
            data = out.getvalue()
            if len(data) <= budget:
                return data
    raise RuntimeError(f'could not squeeze {src_path} below {budget} bytes')


def upload_attachment(base_url, iden, tok, data: bytes, filename: str, mime: str):
    """POST bytes to /attachment/upload and return the server's JSON response."""
    url = f'{base_url}/api/identities/{iden}/attachment/upload'
    headers = {'Authorization': f'Bearer {tok}',
               'X-Total-Length': str(len(data))}
    files = {'file': (filename, data, mime)}
    r = requests.post(url, headers=headers, files=files, timeout=30)
    if not r.ok:
        print(f'    [upload] HTTP {r.status_code}: {r.text}')
    r.raise_for_status()
    return r.json()


def send_with_attachment(base_url, iden, tok, to_addr, body, attachment):
    """POST /send with a staging_id-referenced attachment."""
    url = f'{base_url}/api/identities/{iden}/send'
    headers = {'Authorization': f'Bearer {tok}', 'Content-Type': 'application/json'}
    payload = {'to': to_addr, 'content': body, 'attachments': [attachment]}
    r = requests.post(url, headers=headers, json=payload, timeout=30)
    return r.status_code, (r.json() if r.headers.get('content-type','').startswith('application/json') else r.text)


def wait_for_attachment(base_url, iden, tok, peer_addr, expected_size: int, timeout_s: int):
    """Poll receiver /state for an inbound message from `peer_addr` whose
    attachments include at least one entry with size == expected_size."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        r = requests.get(f'{base_url}/api/identities/{iden}/state',
                         headers={'Authorization': f'Bearer {tok}'}, timeout=8)
        if r.status_code == 200:
            doc = r.json()
            for conv in doc.get('conversations', []):
                if conv.get('peer') != peer_addr:
                    continue
                for m in conv.get('messages', []):
                    if not m.get('in'):
                        continue
                    for a in m.get('attachments', []):
                        if a.get('size') == expected_size and a.get('tag') == 6:
                            return a
        time.sleep(1.2)
    return None


def fetch_attachment(base_url, iden, tok, filename: str) -> bytes:
    url = f'{base_url}/api/identities/{iden}/attachment/download/{filename}'
    r = requests.get(url, headers={'Authorization': f'Bearer {tok}'}, timeout=30)
    r.raise_for_status()
    return r.content


def trigger_announce(base_url, iden, tok):
    requests.post(f'{base_url}/api/identities/{iden}/announce',
                  headers={'Authorization': f'Bearer {tok}'}, timeout=15)


def run_leg(label, send, recv, body, image_bytes, timeout_s):
    print(f"\n{'='*68}\n{label}\n  img={len(image_bytes)} B  body={len(body)!r}\n  timeout={timeout_s}s")
    t0 = time.time()

    print('  → uploading via /attachment/upload …')
    up = upload_attachment(send['url'], send['iden'], send['tok'],
                           image_bytes, 'test.jpg', 'image/jpeg')
    print(f'    staging_id={up["staging_id"]} backend={up["backend"]} total_bytes={up["total_bytes"]}')
    assert up['total_bytes'] == len(image_bytes), 'server reported wrong total_bytes'

    print('  → POST /send with staging_id reference …')
    att = {'tag': 6, 'staging_id': up['staging_id'],
           'filename': 'test.jpg', 'mime': 'image/jpeg', 'ext': 'jpg'}
    st, resp = send_with_attachment(send['url'], send['iden'], send['tok'],
                                    recv['addr'], body, att)
    if st not in (200, 202):
        print(f'    [FAIL] /send returned HTTP {st}: {resp}')
        return False
    print(f'    /send accepted in {time.time()-t0:.1f}s (status={st}, queued_seq={resp.get("queued_seq")})')

    print(f'  → polling receiver /state for inbound attachment of size {len(image_bytes)} …')
    meta = wait_for_attachment(recv['url'], recv['iden'], recv['tok'],
                               send['addr'], len(image_bytes), timeout_s)
    if not meta:
        print(f'    [FAIL] no matching attachment after {time.time()-t0:.1f}s')
        return False
    print(f'    received in {time.time()-t0:.1f}s · filename={meta.get("filename")} backend={meta.get("backend")}')

    print(f'  → downloading via /attachment/download/{meta["filename"]} and verifying bytes …')
    received = fetch_attachment(recv['url'], recv['iden'], recv['tok'], meta['filename'])
    if hashlib.sha256(received).digest() != hashlib.sha256(image_bytes).digest():
        print(f'    [FAIL] sha256 mismatch (sent={len(image_bytes)} B, got={len(received)} B)')
        return False
    print(f'    [PASS] receiver bytes verified ({len(received)} B match) - total elapsed {time.time()-t0:.1f}s')

    # Sender-side persistence check: the sender's own outbox should now
    # carry a filename pointing at a persisted blob, and downloading it
    # should match the same bytes the recipient received.
    print(f'  → checking sender outbox for persisted copy of own attachment …')
    sender_meta = wait_for_outbound_attachment(send['url'], send['iden'], send['tok'],
                                               recv['addr'], len(image_bytes), timeout_s=10)
    if not sender_meta or not sender_meta.get('filename'):
        print('    [WARN] sender outbox has no persisted filename - toggle off, or persist failed')
        return True
    print(f'    sender stored as {sender_meta["filename"]} (backend={sender_meta.get("backend")})')
    sent_back = fetch_attachment(send['url'], send['iden'], send['tok'], sender_meta['filename'])
    if hashlib.sha256(sent_back).digest() != hashlib.sha256(image_bytes).digest():
        print(f'    [FAIL] sender-side bytes diverge (got {len(sent_back)} B)')
        return False
    print(f'    [PASS] sender-side bytes verified ({len(sent_back)} B match)')
    return True


def wait_for_outbound_attachment(base_url, iden, tok, peer_addr,
                                  expected_size, timeout_s):
    """Same shape as wait_for_attachment but looks for an *outbound* msg."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        r = requests.get(f'{base_url}/api/identities/{iden}/state',
                         headers={'Authorization': f'Bearer {tok}'}, timeout=8)
        if r.status_code == 200:
            doc = r.json()
            for conv in doc.get('conversations', []):
                if conv.get('peer') != peer_addr:
                    continue
                for m in reversed(conv.get('messages', [])):
                    if m.get('in'):
                        continue
                    for a in m.get('attachments', []):
                        if a.get('size') == expected_size and a.get('tag') == 6:
                            return a
        time.sleep(0.6)
    return None


def main():
    print(f'Resizing {SAMPLE_IMG} down to ≤ {TARGET_BYTES} bytes …')
    img = resize_to_budget(SAMPLE_IMG, TARGET_BYTES)
    print(f'  → {len(img)} B JPEG, sha256={hashlib.sha256(img).hexdigest()[:16]}…')

    sx = {'url': SX_URL, 'iden': SX_IDEN, 'tok': SX_TOK, 'addr': SX_ADDR}
    lr = {'url': LR_URL, 'iden': LR_IDEN, 'tok': LR_TOK, 'addr': LR_ADDR}

    print('\n→ pre-test: re-announcing both sides so paths are fresh')
    trigger_announce(SX_URL, SX_IDEN, SX_TOK)
    trigger_announce(LR_URL, LR_IDEN, LR_TOK)
    time.sleep(6)

    legs = [
        ('SX → LR image attachment', sx, lr, 'attachment test (SX→LR)', img, 240),
        ('LR → SX image attachment', lr, sx, 'attachment test (LR→SX)', img, 240),
    ]
    results = []
    for label, snd, rcv, body, image, t_o in legs:
        ok = run_leg(label, snd, rcv, body, image, t_o)
        results.append((label, ok))
        time.sleep(3)

    print('\n' + '=' * 68)
    print('SUMMARY')
    print('=' * 68)
    for name, ok in results:
        print(f'  [{ "PASS" if ok else "FAIL" }]  {name}')
    return 0 if all(ok for _, ok in results) else 1


if __name__ == '__main__':
    sys.exit(main())
