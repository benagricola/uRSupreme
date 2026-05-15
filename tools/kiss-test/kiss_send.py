#!/usr/bin/env python3
"""KISS-side test driver: brings the SX up as an RNode via the project's
RNS config, creates an LXMF identity, announces it, and sends a text
message to a destination address.

Usage:
    python3 kiss_send.py --to <32-hex-dest> --body "hello"
    python3 kiss_send.py --announce-only

The RNode interface params come from tools/kiss-test/rnsconfig — change
them there (frequency, BW, SF, CR, TX power) and re-run, no edit needed
to this script.
"""
import argparse
import os
import sys
import time
import urllib.request

import RNS
import LXMF


def trigger_remote_announce(base_url, identity_id, token):
    """Fire a manual announce on the SX via its WebUI API. Best-effort."""
    if not base_url or not identity_id or not token:
        return
    url = base_url.rstrip("/") + f"/api/identities/{identity_id}/announce"
    req = urllib.request.Request(url, method="POST",
                                 headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            print(f"[remote-announce] HTTP {r.status}")
    except Exception as e:
        print(f"[remote-announce] failed: {e}")


def wait_for_interfaces_online(timeout=15.0):
    """RNode interface takes ~3-5s to start after the serial port opens.
    Block until at least one interface reports online, or `timeout`."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        for iface in RNS.Transport.interfaces:
            if getattr(iface, "online", False):
                print(f"[iface] {iface} online")
                return True
        time.sleep(0.3)
    print("[iface] WARN: no interface came online before timeout")
    return False

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--to",   help="32-hex destination LXMF address to send to.")
    ap.add_argument("--body", default="hello from kiss", help="Message text.")
    ap.add_argument("--title", default="", help="LXMF title field.")
    ap.add_argument("--announce-only", action="store_true",
                    help="Announce and exit; don't send.")
    ap.add_argument("--wait", type=float, default=30.0,
                    help="Seconds to keep listening for proof/announces.")
    ap.add_argument("--remote-url", default="http://192.168.1.116",
                    help="Base URL of the receiver's WebUI (for trigger-announce).")
    ap.add_argument("--remote-identity", default="",
                    help="Identity ID on the receiver to nudge into announcing.")
    ap.add_argument("--remote-token", default="",
                    help="Bearer token for the receiver's API.")
    ap.add_argument("--method", default="OPPORTUNISTIC",
                    choices=("OPPORTUNISTIC", "DIRECT", "PROPAGATED"),
                    help="LXMF desired_method. OPPORTUNISTIC = single packet "
                         "(<= ~295 B). DIRECT = open a Link first; LXMF "
                         "then picks PACKET (<= 319 B) or RESOURCE (> 319 B) "
                         "based on body size.")
    args = ap.parse_args()

    if not args.announce_only and not args.to:
        sys.exit("--to <hex> is required unless --announce-only is set")

    reticulum = RNS.Reticulum(HERE)
    storage = os.path.join(HERE, "lxmf-storage")
    os.makedirs(storage, exist_ok=True)

    # Persistent identity so the address is stable across reruns —
    # otherwise every invocation announces a brand-new identity and the
    # receiver has no chance to remember us.
    identity_path = os.path.join(HERE, "test_identity")
    if os.path.exists(identity_path):
        identity = RNS.Identity.from_file(identity_path)
    else:
        identity = RNS.Identity()
        identity.to_file(identity_path)

    router = LXMF.LXMRouter(identity=identity, storagepath=storage)
    local = router.register_delivery_identity(identity, display_name="kiss-test")
    print(f"[ours] address = {local.hash.hex()}")
    print(f"[ours] identity hash = {identity.hash.hex()}")

    # The RNode interface needs a few seconds after the serial port opens
    # before the radio is actually transmitting. Don't announce until
    # then or our own announce + any incoming announce will be missed.
    wait_for_interfaces_online()

    # Announce ourselves a couple of times so the peer hears us.
    print("[announce] sending…")
    local.announce()
    time.sleep(2)
    local.announce()

    # Nudge the receiver to announce too (its WebUI API supports a manual
    # announce trigger). The announce only reaches us via the RF link, so
    # this must happen after wait_for_interfaces_online() above.
    trigger_remote_announce(args.remote_url, args.remote_identity, args.remote_token)

    if args.announce_only:
        print(f"[wait] holding link up for {args.wait:.0f}s so peers can see the announce")
        time.sleep(args.wait)
        return

    # Build a Destination for the recipient. We may need to wait for an
    # announce from them before we have their public key; LXMRouter
    # handles that by sending a path request and queuing the message.
    # Wait until BOTH an identity and a live path are known. Identity
    # may be cached on disk from a prior run, but the path table is
    # in-memory only and needs a fresh announce processed by Transport.
    dest_hash = bytes.fromhex(args.to)
    print("[recv-wait] holding for fresh announce + path registration …")
    last_nudge = 0
    deadline = time.time() + 45
    while time.time() < deadline:
        ident = RNS.Identity.recall(dest_hash)
        has_path = RNS.Transport.has_path(dest_hash)
        if ident and has_path:
            print(f"[recv-wait] identity+path ready after {int(time.time() - (deadline - 45))}s")
            break
        if time.time() - last_nudge > 8:
            trigger_remote_announce(args.remote_url, args.remote_identity, args.remote_token)
            print(f"[recv-wait]  identity={'Y' if ident else 'n'} path={'Y' if has_path else 'n'}")
            last_nudge = time.time()
        time.sleep(0.5)
    recipient_identity = RNS.Identity.recall(dest_hash)
    if recipient_identity is None or not RNS.Transport.has_path(dest_hash):
        sys.exit(f"[fail] identity={'Y' if recipient_identity else 'n'} "
                 f"path={'Y' if RNS.Transport.has_path(dest_hash) else 'n'} "
                 "after 45s — radio link may be down or announce isn't being parsed.")
    recipient = RNS.Destination(recipient_identity, RNS.Destination.OUT,
                                RNS.Destination.SINGLE, "lxmf", "delivery")
    print(f"[recipient] identity hash = {recipient_identity.hash.hex()}")
    print(f"[recipient] dest hash     = {recipient.hash.hex()}")

    # Method dispatch:
    #   OPPORTUNISTIC = single-packet delivery (the firmware's
    #     baseline path; works for bodies <= ~295 bytes).
    #   DIRECT        = open a Link first; LXMF picks in-link PACKET
    #     (<= 319 B) or RESOURCE (> 319 B). Exercises the firmware's
    #     link/resource dispatch added in plan steps 6-10.
    method = {
        "OPPORTUNISTIC": LXMF.LXMessage.OPPORTUNISTIC,
        "DIRECT":        LXMF.LXMessage.DIRECT,
        "PROPAGATED":    LXMF.LXMessage.PROPAGATED,
    }[args.method]
    msg = LXMF.LXMessage(recipient, local, args.body, args.title,
                         desired_method=method)
    print(f"[send] method={args.method}  body={len(args.body)} bytes")

    def on_delivered(m):  print(f"[delivery] PROOF received — message delivered")
    def on_failed(m):     print(f"[delivery] FAILED")
    msg.register_delivery_callback(on_delivered)
    msg.register_failed_callback(on_failed)

    router.handle_outbound(msg)
    print(f"[send] queued, awaiting proof for up to {args.wait:.0f}s")
    STATE_NAMES = {
        0:  "GENERATING",
        1:  "OUTBOUND",
        2:  "SENDING",
        3:  "PROPAGATED",
        4:  "SENT",
        8:  "DELIVERED",
        253: "REJECTED",
        254: "CANCELLED",
        255: "FAILED",
    }
    name = lambda s: STATE_NAMES.get(s, f"state={s}")
    deadline = time.time() + args.wait
    last_state = None
    while time.time() < deadline and msg.state not in (
            LXMF.LXMessage.DELIVERED, LXMF.LXMessage.FAILED, LXMF.LXMessage.REJECTED):
        if msg.state != last_state:
            print(f"[state] {name(msg.state)}")
            last_state = msg.state
        time.sleep(0.5)
    print(f"[final] state = {name(msg.state)}")


if __name__ == "__main__":
    main()
