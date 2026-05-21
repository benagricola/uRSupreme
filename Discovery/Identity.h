// Network-level identity — a per-device RNS::Identity persisted to
// /lxmf/network_identity.bin and reused across boots.
//
// This is the analogue of upstream Reticulum's `network_identity`
// config — a single stable keypair the device uses for cross-cutting
// network features that don't belong to any individual LXMF chat
// identity. The first such feature is interface-discovery announces
// (Discovery::Announcer, to land in a later commit) which sign
// (and optionally encrypt) the discovery payload with this identity;
// future features (rmap participation beyond the announce, peer
// discovery, etc.) can share it without entangling the user's
// per-conversation chat identities.
//
// Generated at first boot when /lxmf/network_identity.bin doesn't
// exist; loaded from that file on every subsequent boot. The hash
// is exposed as a hex string so the SPA can show "Network ID: …"
// in the Discovery tab and the user has a stable handle to point
// at if they need to whitelist their device on a downstream
// consumer like rmap.world.
//
// Storage path is fixed at /lxmf/network_identity.bin (not under
// the per-LXMF-identity dirs) so it survives a per-identity factory
// reset — the user can rotate any LXMF chat identity without
// breaking their device's discovery presence.

#pragma once

#include <Arduino.h>
#include <string>
#include <Identity.h>
#include <Bytes.h>
#include <Log.h>

namespace Discovery {
namespace Identity {

inline constexpr const char* PERSIST_PATH = "/lxmf/network_identity.bin";

namespace _detail {
  inline RNS::Identity& slot() {
    static RNS::Identity inst{RNS::Type::NONE};
    return inst;
  }
  inline bool& ready_ref() { static bool v = false; return v; }
}

// Idempotent. Loads the persisted identity if /lxmf/network_identity.bin
// exists; otherwise generates a fresh keypair and persists it. Logs
// the destination hash (NOT the secret) on first generation so the
// user has a record. Safe to call from setup() once the filesystem
// is mounted; second + calls return immediately.
inline void ensure() {
  if (_detail::ready_ref()) return;
  RNS::Identity loaded = RNS::Identity::from_file(PERSIST_PATH);
  if (loaded) {
    _detail::slot() = loaded;
    _detail::ready_ref() = true;
    NOTICEF("Discovery: loaded network identity (hash=%s)",
            loaded.hash().toHex().c_str());
    return;
  }
  RNS::Identity fresh;
  if (!fresh.to_file(PERSIST_PATH)) {
    ERRORF("Discovery: failed to persist network identity to %s",
           PERSIST_PATH);
    // Hold the in-RAM one anyway so callers don't crash on null.
    // It just won't survive reboot — the next ensure() will try
    // again and most likely succeed (transient FS error).
  } else {
    NOTICEF("Discovery: generated new network identity (hash=%s, persisted to %s)",
            fresh.hash().toHex().c_str(), PERSIST_PATH);
  }
  _detail::slot() = fresh;
  _detail::ready_ref() = true;
}

// True once ensure() has succeeded at least once. Other modules
// should gate any use of get() on this.
inline bool ready() { return _detail::ready_ref(); }

// The persisted identity. Returns an "empty" Identity if ensure()
// hasn't run yet — callers should check ready() first or expect
// undefined behaviour from the returned object's methods.
inline const RNS::Identity& get() { return _detail::slot(); }

// Hex representation of the identity hash. Useful for surfacing
// the device's network ID in the SPA + logs. Returns the empty
// string before ensure().
inline std::string address_hex() {
  if (!ready()) return {};
  return _detail::slot().hash().toHex();
}

}  // namespace Identity
}  // namespace Discovery
