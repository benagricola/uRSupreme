#!/usr/bin/env python3
"""Brings up the LR (over KISS at /dev/ttyACM0) as an RNS+LXMF endpoint,
registers a persistent identity, announces, and prints every incoming
LXMF message until Ctrl-C.

Pair with the SX's WebUI API to drive end-to-end SX → LR tests:
    POST http://192.168.1.116/api/identities/<id>/send
         body: {"to": "<our-address>", "content": "hello"}

Re-uses tools/kiss-test/test_identity so our address is stable across
runs — that way the receiver (this script) and the sender (the SX's
contact list) don't need re-pairing every restart.
"""
import os
import sys
import time
import argparse

import RNS
import LXMF

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--display-name", default="kiss-test",
                    help="Display name advertised in our announce.")
    args = ap.parse_args()

    reticulum = RNS.Reticulum(HERE)
    storage = os.path.join(HERE, "lxmf-storage")
    os.makedirs(storage, exist_ok=True)

    identity_path = os.path.join(HERE, "test_identity")
    if os.path.exists(identity_path):
        identity = RNS.Identity.from_file(identity_path)
    else:
        identity = RNS.Identity()
        identity.to_file(identity_path)

    router = LXMF.LXMRouter(identity=identity, storagepath=storage)
    local = router.register_delivery_identity(identity, display_name=args.display_name)
    print(f"[ours] address = {local.hash.hex()}")
    print(f"[ours] identity hash = {identity.hash.hex()}")

    def on_lxmessage(message):
        ts   = time.strftime("%H:%M:%S", time.localtime(message.timestamp or time.time()))
        src  = message.source_hash.hex()
        title = message.title.decode("utf-8", "replace") if message.title else ""
        body = message.content.decode("utf-8", "replace") if message.content else ""
        sig_ok = "yes" if message.signature_validated else "NO"
        print(f"\n[{ts}] INBOUND  from={src}  sig={sig_ok}")
        if title: print(f"  title: {title}")
        if body:  print(f"  body : {body}")
        print(f"  state={message.state}  method={message.method}\n")
    router.register_delivery_callback(on_lxmessage)

    # Wait for the radio to come up, then announce.
    deadline = time.time() + 15
    while time.time() < deadline:
        for iface in RNS.Transport.interfaces:
            if getattr(iface, "online", False):
                break
        else:
            time.sleep(0.3); continue
        break
    local.announce()
    print("[announce] sent — listening forever; Ctrl-C to stop")

    # Re-announce every couple of minutes so the SX keeps a fresh ratchet
    # and path.
    last_announce = time.time()
    while True:
        time.sleep(1)
        if time.time() - last_announce > 120:
            local.announce()
            last_announce = time.time()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
