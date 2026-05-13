#pragma once

// Multi-identity feasibility probe for the LXMF gateway plan.
//
// Goal: verify empirically that each RNS::Destination uses the Identity
// it was constructed with (not Transport::identity()) for its hash,
// encryption, and signature. If this holds, the planned multi-tenant
// gateway is safe to build.
//
// Static analysis already confirms this from the upstream library:
//   - Destination::encrypt() at Destination.cpp:447 -> _identity.encrypt
//   - Destination::sign()    at Destination.cpp:518 -> _identity.sign
//   - Destination::announce  at Destination.cpp:300 -> _identity.sign
//   - Packet::pack           at Packet.cpp:289      -> _destination.hash()
//   - Destination hash       at Destination.cpp:61   derives from identity
//
// This probe checks the runtime behaviour: that two destinations under
// two distinct Identity instances produce distinct hashes that are
// neither equal to each other nor equal to Transport::identity()'s hash.
//
// Enabled at compile time via -DLXMF_PROBE_MULTI_IDENTITY=1. Output goes
// to the standard RNS log; grep the serial output for "LXMF-PROBE".

#if defined(LXMF_PROBE_MULTI_IDENTITY) && LXMF_PROBE_MULTI_IDENTITY

#include <Reticulum.h>
#include <Identity.h>
#include <Destination.h>
#include <Packet.h>
#include <Transport.h>
#include <Log.h>

namespace LXMFProbe {

  // Call once from setup() after reticulum.start(). Logs a PASS/FAIL
  // line at NOTICE level. Returns true on PASS.
  inline bool run() {
    HEAD("LXMF-PROBE: starting multi-identity verification", RNS::LOG_NOTICE);

    // Reference: Transport-level identity (the node's own).
    const std::string transport_hash = RNS::Transport::identity().hexhash();
    NOTICEF("LXMF-PROBE: transport identity hash = %s", transport_hash.c_str());

    // Two fresh per-account identities.
    RNS::Identity id_alice;
    RNS::Identity id_bob;
    const std::string alice_hash = id_alice.hexhash();
    const std::string bob_hash   = id_bob.hexhash();
    NOTICEF("LXMF-PROBE: identity alice hash     = %s", alice_hash.c_str());
    NOTICEF("LXMF-PROBE: identity bob   hash     = %s", bob_hash.c_str());

    if (alice_hash == bob_hash) {
      ERROR("LXMF-PROBE: FAIL - two fresh identities produced the same hash");
      return false;
    }
    if (alice_hash == transport_hash || bob_hash == transport_hash) {
      ERROR("LXMF-PROBE: FAIL - account identity collided with transport identity");
      return false;
    }

    // Build two destinations, one per identity. SINGLE/IN matches the
    // LXMF delivery destination shape we'll use in production.
    RNS::Destination dest_alice(id_alice,
                                RNS::Type::Destination::IN,
                                RNS::Type::Destination::SINGLE,
                                "lxmfprobe", "alice");
    RNS::Destination dest_bob(id_bob,
                              RNS::Type::Destination::IN,
                              RNS::Type::Destination::SINGLE,
                              "lxmfprobe", "bob");

    const std::string dest_alice_hash = dest_alice.hash().toHex();
    const std::string dest_bob_hash   = dest_bob.hash().toHex();
    NOTICEF("LXMF-PROBE: destination alice hash  = %s", dest_alice_hash.c_str());
    NOTICEF("LXMF-PROBE: destination bob   hash  = %s", dest_bob_hash.c_str());

    if (dest_alice_hash == dest_bob_hash) {
      ERROR("LXMF-PROBE: FAIL - two destinations under different identities produced the same hash");
      return false;
    }

    // Build a small packet from each destination and confirm the
    // destination_hash baked into the packet matches the per-destination
    // value (and so, transitively, the per-identity derivation).
    {
      RNS::Bytes payload((const uint8_t*)"probe-a", 7);
      RNS::Packet pkt(dest_alice, payload);
      pkt.pack();
      const std::string pkt_dh = pkt.destination_hash().toHex();
      NOTICEF("LXMF-PROBE: packet from alice    dh = %s", pkt_dh.c_str());
      if (pkt_dh != dest_alice_hash) {
        ERROR("LXMF-PROBE: FAIL - packet destination_hash != destination.hash() for alice");
        return false;
      }
    }
    {
      RNS::Bytes payload((const uint8_t*)"probe-b", 7);
      RNS::Packet pkt(dest_bob, payload);
      pkt.pack();
      const std::string pkt_dh = pkt.destination_hash().toHex();
      NOTICEF("LXMF-PROBE: packet from bob      dh = %s", pkt_dh.c_str());
      if (pkt_dh != dest_bob_hash) {
        ERROR("LXMF-PROBE: FAIL - packet destination_hash != destination.hash() for bob");
        return false;
      }
    }

    // Sign + verify a small payload with each identity, cross-check to
    // confirm the keys are distinct and bound to the right identity.
    {
      RNS::Bytes msg((const uint8_t*)"hello", 5);
      RNS::Bytes sig_a = id_alice.sign(msg);
      RNS::Bytes sig_b = id_bob.sign(msg);
      if (sig_a == sig_b) {
        ERROR("LXMF-PROBE: FAIL - two identities produced identical signatures over the same payload");
        return false;
      }
      if (!id_alice.validate(sig_a, msg)) {
        ERROR("LXMF-PROBE: FAIL - alice cannot validate its own signature");
        return false;
      }
      if (id_alice.validate(sig_b, msg)) {
        ERROR("LXMF-PROBE: FAIL - alice validated a signature made by bob's key");
        return false;
      }
      if (!id_bob.validate(sig_b, msg)) {
        ERROR("LXMF-PROBE: FAIL - bob cannot validate its own signature");
        return false;
      }
    }

    HEAD("LXMF-PROBE: PASS - per-Destination identities work as expected", RNS::LOG_NOTICE);
    return true;
  }

}

#endif // LXMF_PROBE_MULTI_IDENTITY
