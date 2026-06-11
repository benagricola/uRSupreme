#pragma once

// C-linkage callbacks the patched microReticulum code calls to source
// ratchet material from our LXMF gateway.
//
// The matching `rns_set_outbound_ratchet_provider` / `rns_set_inbound_ratchet_provider`
// setters are injected into microReticulum's Destination.cpp / Identity.cpp
// by extra_script.py - see patch_outbound_ratchet and patch_inbound_ratchet
// there.  The patched code stores the function pointer in a static, calls
// the registered provider during announce/decrypt, and falls back to its
// pre-patch behaviour if no provider has been registered.

#include "LXMFGateway.h"

extern "C" {

typedef bool (*outbound_ratchet_provider_fn)(const uint8_t* dest_hash, uint8_t* out_pubkey_32);
typedef bool (*inbound_ratchet_privkey_fn)(const uint8_t* identity_hash, size_t index, uint8_t* out_privkey_32);

// Defined by the Destination.cpp and Identity.cpp patches respectively.
void rns_set_outbound_ratchet_provider(outbound_ratchet_provider_fn fn);
void rns_set_inbound_ratchet_provider(inbound_ratchet_privkey_fn fn);

// Adapters with C linkage that forward to LXMFGateway's static methods.
inline bool lxmf_outbound_ratchet_adapter(const uint8_t* dest_hash, uint8_t* out_pubkey_32) {
  return LXMF::LXMFGateway::rotate_outbound_ratchet(dest_hash, out_pubkey_32);
}

inline bool lxmf_inbound_ratchet_adapter(const uint8_t* identity_hash, size_t index, uint8_t* out_privkey_32) {
  return LXMF::LXMFGateway::inbound_ratchet_privkey(identity_hash, index, out_privkey_32);
}

}  // extern "C"

namespace LXMF {
  // Call once at startup, after LXMFGateway::setup(), to wire the
  // patched microReticulum code to our gateway-backed providers.
  inline void register_ratchet_providers() {
    rns_set_outbound_ratchet_provider(&lxmf_outbound_ratchet_adapter);
    rns_set_inbound_ratchet_provider(&lxmf_inbound_ratchet_adapter);
  }
}
