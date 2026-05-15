#!/usr/bin/env python3
"""Bidirectional Resource-protocol test sweep between two firmware-mode
devices (SX1262 and LR1121, both running our LXMF gateway). Each side
is driven entirely via the device's HTTP API — no Python RNS involved.

Each test sends a body of a chosen size from one device to the other,
then polls the receiver's /state until the message appears (or a
timeout elapses). Different body sizes exercise different code paths:

  <= 295 B   : OPPORTUNISTIC (single Packet to SINGLE dest)
  296..319 B : DIRECT mode, in-link PACKET
  320..8192  : DIRECT mode, Resource (heap-backed buffer)
  > 8192     : DIRECT mode, Resource (flash-backed buffer)

Usage:
    python3 bidir_test.py
"""
import json
import sys
import time
import urllib.request

SX_URL  = 'http://192.168.1.116'
LR_URL  = 'http://192.168.1.118'

# Per-device handles. Loaded from .token / .lr-token / .lr-iden / .lr-addr
# stashed during the create-identity step.
HERE = __file__.rsplit('/', 1)[0]
def _f(name):
    return open(HERE + '/' + name).read().strip()

SX_IDEN = '140991649b164ece'
SX_TOK  = _f('.token')
SX_ADDR = 'e60cf2202cd0609925c0948cf84147a9'

LR_IDEN = _f('.lr-iden')
LR_TOK  = _f('.lr-token')
LR_ADDR = _f('.lr-addr')


def http(method, url, token, body=None, timeout=20):
    headers = {'Authorization': f'Bearer {token}'}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read() or b'null')
    except urllib.error.HTTPError as e:
        try:
            err = json.loads(e.read())
        except Exception:
            err = {'error': str(e)}
        return e.code, err


def send(from_url, from_iden, from_tok, to_addr, body):
    return http('POST', f'{from_url}/api/identities/{from_iden}/send',
                from_tok, {'to': to_addr, 'content': body})


def wait_for_message(recv_url, recv_iden, recv_tok, body, timeout_s):
    """Poll the receiver's inbox for the expected body. Returns the
    matching message dict on success, None on timeout."""
    deadline = time.time() + timeout_s
    last_seen = 0
    while time.time() < deadline:
        st, doc = http('GET', f'{recv_url}/api/identities/{recv_iden}/state',
                       recv_tok, timeout=5)
        if st == 200:
            for conv in doc.get('conversations', []):
                last = conv.get('last_body') or ''
                # Match by exact body for short messages, by length+prefix for long.
                if last == body:
                    return {'peer': conv.get('peer'), 'last_ts': conv.get('last_ts'),
                            'last_body': last, 'method': 'unknown'}
                if len(body) >= 200 and last.startswith(body[:100]) and len(last) == len(body):
                    return {'peer': conv.get('peer'), 'last_ts': conv.get('last_ts'),
                            'last_body_len': len(last)}
        time.sleep(0.8)
    return None


def trigger_announce(url, iden, tok):
    http('POST', f'{url}/api/identities/{iden}/announce', tok, timeout=20)


def expected_path(body_len):
    if body_len <= 295: return 'OPPORTUNISTIC'
    if body_len <= 319: return 'DIRECT/PACKET'
    if body_len <= 8192: return 'DIRECT/Resource (heap)'
    return 'DIRECT/Resource (flash)'


def run_test(label, send_url, send_iden, send_tok, recv_url, recv_iden,
             recv_tok, recv_addr, body, timeout_s):
    print(f"\n{'='*64}\n{label}\n  body_len={len(body)}  expected={expected_path(len(body))}\n  timeout={timeout_s}s")
    t0 = time.time()
    st, resp = send(send_url, send_iden, send_tok, recv_addr, body)
    # 200 = sync-delivered, 202 = queued for async (Resource/Link send),
    # both indicate the API accepted the send. The actual delivery is
    # confirmed by polling the receiver's inbox below.
    if st not in (200, 202):
        print(f"  [FAIL] send returned HTTP {st}: {resp}")
        return False
    print(f"  send queued in {time.time()-t0:.1f}s; polling receiver...")
    msg = wait_for_message(recv_url, recv_iden, recv_tok, body, timeout_s)
    elapsed = time.time() - t0
    if msg:
        print(f"  [PASS] delivered in {elapsed:.1f}s")
        return True
    print(f"  [FAIL] no inbox match after {elapsed:.1f}s")
    return False


# Make sure both sides have fresh paths to each other.
print("=== pre-test: triggering announces both sides ===")
trigger_announce(SX_URL, SX_IDEN, SX_TOK)
trigger_announce(LR_URL, LR_IDEN, LR_TOK)
time.sleep(6)

tests = [
    ('T1  SX -> LR  short OPPORTUNISTIC',  SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'ping (short)',           30),
    ('T2  LR -> SX  short OPPORTUNISTIC',  LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'pong (short)',           30),
    ('T3  SX -> LR  250B OPPORTUNISTIC',   SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'A' * 250,                45),
    ('T4  LR -> SX  250B OPPORTUNISTIC',   LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'B' * 250,                45),
    ('T5  SX -> LR  350B DIRECT/PACKET',   SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'C' * 350,                60),
    ('T6  LR -> SX  350B DIRECT/PACKET',   LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'D' * 350,                60),
    ('T7  SX -> LR  600B Resource heap',   SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'E' * 600,                90),
    ('T8  LR -> SX  600B Resource heap',   LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'F' * 600,                90),
    ('T9  SX -> LR  2000B Resource heap',  SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'G' * 2000,              120),
    ('T10 LR -> SX  2000B Resource heap',  LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'H' * 2000,              120),
    ('T11 SX -> LR  12000B Resource flash',SX_URL, SX_IDEN, SX_TOK, LR_URL, LR_IDEN, LR_TOK, LR_ADDR, 'I' * 12000,             300),
    ('T12 LR -> SX  12000B Resource flash',LR_URL, LR_IDEN, LR_TOK, SX_URL, SX_IDEN, SX_TOK, SX_ADDR, 'J' * 12000,             300),
]

results = []
for t in tests:
    ok = run_test(*t)
    results.append((t[0], ok))
    if not ok:
        # On failure, keep going so we get a full picture.
        pass
    # Small inter-test gap so we don't confuse path table with rapid-fire traffic
    time.sleep(2)

print("\n\n" + "=" * 64)
print("SUMMARY")
print("=" * 64)
for name, ok in results:
    mark = 'PASS' if ok else 'FAIL'
    print(f"  [{mark}]  {name}")
passes = sum(1 for _, ok in results if ok)
print(f"\n  {passes}/{len(results)} tests passed")
sys.exit(0 if passes == len(results) else 1)
