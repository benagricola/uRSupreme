// Discovery announcer - periodic per-interface
// `rnstransport.discovery.interface` announces.
//
// Mirrors the upstream Reticulum InterfaceAnnouncer.job() loop
// (RNS/Discovery.py:66-86) in spirit: at each tick, walk every
// registered interface, pick those that (a) are flagged discoverable
// in Discovery::Config and (b) are due to re-announce (now >
// last_announce + interval). For each due interface, build the
// announce app_data via Discovery::Announce::Builder and call the
// shared discovery destination's announce().
//
// Single destination shared across all interfaces (matches upstream
// - the announce identifies the originating interface in the
// app_data, not via separate per-interface destinations). Constructed
// once at setup() using the Discovery::Identity persistent keypair
// so consumers can verify the announce signature against a stable
// device-wide identity.
//
// Master toggle (Discovery::State::current().enabled) short-circuits
// the whole tick when false - privacy-by-design, nothing on-air
// until the user explicitly enables.

#pragma once

#include <Arduino.h>
#include <Log.h>
#include <Destination.h>
#include <Transport.h>
#include <Reticulum.h>     // for transport_enabled()
#include <Identity.h>
#include <Bytes.h>
#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "Identity.h"
#include "Config.h"
#include "State.h"
#include "Announce.h"
#include "Stamp.h"
#include "../Common/RnsLock.h"

// LoRa-radio + GPS state we sample for outbound announces. Per-device
// extern globals live in Config.h / RNode_Firmware.ino.
#include "../Sensors/Position/Gnss.h"
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern int      lora_cr;

namespace Discovery {
namespace Announcer {

// Upstream calls the LoRa interface type "RNodeInterface" - that's
// what InterfaceAnnounceHandler whitelists (DISCOVERABLE_INTERFACE_TYPES
// in RNS/Discovery.py). Match it so cross-implementation listeners
// (downstream RNS listeners, other RNS nodes) accept our LoRa announces.
inline constexpr const char* TYPE_LORA       = "RNodeInterface";
inline constexpr const char* TYPE_TCP_CLIENT = "TCPClientInterface";

namespace _detail {
  inline std::unique_ptr<RNS::Destination>& destination() {
    static std::unique_ptr<RNS::Destination> d;
    return d;
  }
  // Per-interface "last announce" timestamps, keyed by interface name.
  inline std::map<std::string, uint32_t>& last_announce_ms() {
    static std::map<std::string, uint32_t> m;
    return m;
  }
  inline uint32_t& last_tick_ms() { static uint32_t v = 0; return v; }
  // Most recent announce, regardless of interface. Used by the API
  // to surface "this device last shouted N seconds ago" without the
  // caller needing to walk per-interface map keys.
  inline uint32_t& last_any_announce_ms() { static uint32_t v = 0; return v; }
  // Running count since boot, all interfaces. Useful for sanity-
  // checking that the announcer is alive at all on a long-uptime
  // device where the per-interface ms timestamp could be stale.
  inline uint32_t& total_announce_count() { static uint32_t v = 0; return v; }
  // Rate-limit the tick itself - upstream's job loop runs every 60s.
  // No point evaluating "is anything due" more often than once a
  // minute since the minimum cadence in State is also several minutes.
  inline constexpr uint32_t TICK_PERIOD_MS = 60UL * 1000UL;

  // Populate the Announce::Builder for one running interface. Returns
  // true on a complete payload, false if mandatory fields are missing
  // (e.g. unknown interface type - we just skip those rather than
  // emitting half-formed announces).
  // Sanitise the user-supplied label the same way upstream
  // RNS/Discovery.py does: strip newlines + carriage returns +
  // leading/trailing whitespace. Without this, a multi-line label
  // would break the on-wire msgpack-string and confuse listeners
  // that split log lines by \n.
  inline std::string sanitise_name(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    size_t lo = s.find_first_not_of(" \t");
    size_t hi = s.find_last_not_of(" \t");
    if (lo == std::string::npos) return std::string();
    return s.substr(lo, hi - lo + 1);
  }

  inline bool build_for_interface(Announce::Builder& b,
                                  const RNS::Interface& iface,
                                  const Config::Entry& cfg) {
    // Announcement label: prefer the user-supplied advertised_name
    // (from the Discovery tab), fall back to the raw interface name
    // when empty. The interface name is the technical identifier
    // ("LoRaInterface") which isn't a great label on a downstream RNS listener.
    const std::string user_name = State::current().advertised_name;
    const std::string nm = sanitise_name(!user_name.empty()
        ? user_name
        : const_cast<RNS::Interface&>(iface).name());
    b.name(nm);
    b.transport_enabled(RNS::Reticulum::transport_enabled());
    b.transport_id(RNS::Transport::identity().hash());
    // GPS - include lat/lon only when we actually have a fix.
    // Skip if invalid to avoid lighting up (0, 0) on the map for
    // devices that never got a fix.
    const auto fix = Sensors::Gnss::last_fix();
    if (fix.valid) {
      b.lat(fix.latitude_deg);
      b.lon(fix.longitude_deg);
    }
    // HEIGHT: GPS altitude in metres when we have a GGA-derived fix,
    // 0.0 otherwise. Matches upstream's "always emit, default to 0.0
    // if unknown" convention.
    b.height(fix.altitude_valid ? fix.altitude_m : 0.0);
    switch (cfg.type) {
      case Config::Type::Lora:
        b.interface_type(TYPE_LORA);
        b.frequency((int64_t)lora_freq);
        b.bandwidth((int64_t)lora_bw);
        b.spreading_factor(lora_sf);
        b.coding_rate(lora_cr);
        return true;
      case Config::Type::TcpClient:
        b.interface_type(TYPE_TCP_CLIENT);
        // host/port aren't required for a discoverability announce on
        // an outbound TCP client (the device is the connector, not
        // the connectable) - leave off.
        return true;
      case Config::Type::Udp:
        // Upstream's DISCOVERABLE_INTERFACE_TYPES doesn't include UDP;
        // listeners following upstream will reject. We still emit
        // "UDPInterface" for our own consumers - explicit type for
        // forward-compat.
        b.interface_type("UDPInterface");
        return true;
      default:
        return false;
    }
  }
}

// Construct the shared discovery destination from the persistent
// network identity. Called once at boot after Discovery::Identity::
// ensure() has loaded/generated the keypair.
inline void setup() {
  if (_detail::destination()) return;
  if (!Identity::ready()) {
    WARNING("Discovery::Announcer: cannot setup before Identity::ensure() - skipping");
    return;
  }
  // app_name = "rnstransport", aspects = "discovery.interface". The
  // upstream Python passes three aspects positionally; the C++ port
  // joins them with dots in expand_name(), so the second argument
  // is the dot-joined tail.
  _detail::destination().reset(new RNS::Destination(
      Identity::get(),
      RNS::Type::Destination::IN,
      RNS::Type::Destination::SINGLE,
      "rnstransport",
      "discovery.interface"));
  NOTICEF("Discovery::Announcer: destination %s ready",
          _detail::destination()->hash().toHex().c_str());
  // Spin up the background stamp worker so the first announce doesn't
  // pay the FreeRTOS task-create cost on the hot path.
  Stamp::start();
}

// Main-loop pulse. Cheap on the no-op path: returns immediately if
// the master toggle is off OR the per-tick rate limiter says it's
// not time yet OR no interfaces are due. Called from the main loop
// at whatever cadence; internal rate-limit handles the rest.
inline void tick() {
  const State::Master& s = State::current();
  if (!s.enabled) return;
  if (!_detail::destination()) return;
  const uint32_t now = millis();
  if (now - _detail::last_tick_ms() < _detail::TICK_PERIOD_MS
      && _detail::last_tick_ms() != 0) {
    return;
  }
  _detail::last_tick_ms() = now;

  const uint32_t interval_ms = s.default_interval_min * 60UL * 1000UL;
  auto& last_map = _detail::last_announce_ms();

  // Find the one interface most overdue for its next announce.
  // Mirrors upstream: pick the staleness-leader, announce it, defer
  // the others to the next tick. Avoids bursts on multi-interface
  // devices.
  std::string due_name;
  uint32_t due_overdue_ms = 0;
  Config::Entry due_cfg;

  for (auto& kv : RNS::Transport::get_interfaces()) {
    RNS::Interface& iface = kv.second;
    if (!iface) continue;
    const std::string nm = iface.name();
    Config::Entry cfg;
    if (!Config::get(nm, &cfg)) continue;
    if (!cfg.discoverable) continue;
    const uint32_t last = last_map.count(nm) ? last_map[nm] : 0;
    const uint32_t since = (last == 0) ? interval_ms : (now - last);
    if (since < interval_ms) continue;
    if (since > due_overdue_ms) {
      due_overdue_ms = since;
      due_name       = nm;
      due_cfg        = cfg;
    }
  }
  if (due_name.empty()) return;

  // Build the payload for the selected interface and announce.
  Announce::Builder builder;
  // Find the actual interface again so build_for_interface gets the
  // live RNS::Interface (for any field it reads from the wrapper).
  for (auto& kv : RNS::Transport::get_interfaces()) {
    RNS::Interface& iface = kv.second;
    if (!iface) continue;
    if (iface.name() != due_name) continue;
    if (!_detail::build_for_interface(builder, iface, due_cfg)) {
      WARNINGF("Discovery::Announcer: skipping interface '%s' - unknown type",
               due_name.c_str());
      return;
    }
    break;
  }
  const RNS::Bytes packed = builder.serialize_unstamped();
  if (packed.size() == 0) {
    WARNINGF("Discovery::Announcer: serialize_unstamped() returned empty for '%s'",
             due_name.c_str());
    return;
  }

  // PoW + announce runs on the Stamp worker. We pass the on_done
  // callback that captures the interface name (by value - the worker
  // outlives this scope) so the snapshot is consistent even if the
  // config changes between dispatch and completion.
  const uint32_t cost = s.default_stamp_cost;  // 0 = disable PoW entirely
  const std::string iface_name = due_name;
  const uint32_t default_interval_min = s.default_interval_min;
  auto on_done = [packed, iface_name, default_interval_min](const RNS::Bytes& stamp) {
    if (!_detail::destination()) return;
    const RNS::Bytes app_data = Announce::Builder::serialize_with_stamp(packed, stamp);
    if (app_data.size() == 0) {
      WARNINGF("Discovery::Announcer: serialize_with_stamp() empty for '%s'",
               iface_name.c_str());
      return;
    }
    // The RNS path tables aren't reentrant. Take the recursive lock
    // before calling into the destination - the WebUI handlers grab
    // the same lock when they touch state.
    Common::RnsLock::Guard guard;
    if (!guard) {
      WARNINGF("Discovery::Announcer: rns_lock timed out for '%s'", iface_name.c_str());
      return;
    }
    _detail::destination()->announce(app_data);
    _detail::last_announce_ms()[iface_name] = millis();
    _detail::last_any_announce_ms() = millis();
    _detail::total_announce_count()++;
    NOTICEF("Discovery::Announcer: announced interface '%s' (%u B app_data, next due in %u min)",
            iface_name.c_str(), (unsigned)app_data.size(), (unsigned)default_interval_min);
  };

  if (!Stamp::submit(packed, cost, std::move(on_done))) {
    // Worker is busy with a prior submission - leave last_map untouched
    // so this interface remains "due" and we retry on the next tick.
    NOTICEF("Discovery::Announcer: stamp worker busy, deferring '%s'",
            due_name.c_str());
    return;
  }
  // Pre-record the timestamp so we don't resubmit the same interface
  // every tick while the worker is computing. The interface will be
  // suppressed for one interval period; if the stamp callback ends up
  // not firing for any reason, the next tick after the interval will
  // try again.
  last_map[due_name] = now;
}

// Snapshot for /api/discovery/state. Read-only view that lets the
// SPA tell the user when the device last shouted and how many times
// since boot - useful when serial isn't available and the user
// wants to verify the announcer is actually running.
struct Status {
  uint32_t last_any_announce_ms;   // device millis() of most recent announce, 0 = never
  uint32_t total_announce_count;   // count since boot
  std::map<std::string, uint32_t> per_interface_last_ms;  // copy keyed by name
};
inline Status status() {
  Status s;
  s.last_any_announce_ms  = _detail::last_any_announce_ms();
  s.total_announce_count  = _detail::total_announce_count();
  s.per_interface_last_ms = _detail::last_announce_ms();
  return s;
}

}  // namespace Announcer
}  // namespace Discovery
