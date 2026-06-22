#pragma once

// LXMF propagation-node CLIENT support.
//
// This header holds the device-wide propagation config (persisted to
// /lxmf/propagation.json, exposed by the web app) and a RAM registry of
// propagation nodes learned from lxmf.propagation announces (mirrors
// AnnounceLog; rebuilds from announces after a reboot, so it is not
// persisted). The sync/transfer state machine and the outbound PROPAGATED
// path are not yet implemented; they will pull current() / the chosen node
// here.
//
// The config is DEVICE-WIDE: one propagation node for all identities (upstream
// LXMRouter is router-wide, LXMRouter.py:389). Inbound sync will identify per
// identity at sync time so each gets its own mailbox.
//
// Faithful to upstream LXMF: a PN announces on Destination(identity, SINGLE,
// "lxmf", "propagation") (LXMRouter.py:509) with msgpack app_data
//   [ legacy(bool), timebase(uint), node_state(bool), per_transfer_kb(uint),
//     per_sync(uint), [stamp_cost, flex, peering](uint[]), metadata(map) ]
// (LXMRouter.get_propagation_node_app_data, LXMRouter.py:306-318). The node
// name is metadata key PN_META_NAME = 0x01 (LXMF.py:99).

#include <stdint.h>
#include <vector>
#include <deque>
#include <string>
#include <memory>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <Bytes.h>
#include <Identity.h>
#include <Transport.h>
#include <microStore/FileSystem.h>

#include "../Common/PsramAllocator.h"
#include "../Common/MsgPack.h"

namespace LXMF {
namespace Propagation {

// ---- device-wide config (/lxmf/propagation.json) -------------------------

inline constexpr const char* CONFIG_PATH = "/lxmf/propagation.json";

struct Config {
  bool        enabled          = false;  // master switch for propagation
  std::string pn_hash;                   // chosen PN destination hash (hex); "" = none
  uint32_t    sync_interval_s  = 0;      // 0 = manual sync only; else periodic cadence
  bool        retain_on_node   = false;  // true = keep synced messages on the PN
  bool        use_when_offline = true;   // outbound: fall back to the PN when a peer is unreachable
};

inline Config& current() { static Config c; return c; }

// A Reticulum destination hash is 16 bytes = 32 hex chars; clamp anything
// else to empty so a malformed value can't target a bogus destination.
inline void sanitize(Config& c) {
  if (!c.pn_hash.empty() && c.pn_hash.size() != 32) c.pn_hash.clear();
  if (c.sync_interval_s != 0 && c.sync_interval_s < 60) c.sync_interval_s = 60;
  if (c.sync_interval_s > 24UL * 3600) c.sync_interval_s = 24UL * 3600;
}

inline void load(microStore::FileSystem& fs) {
  Config& c = current();
  c = Config{};
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  if (doc["enabled"].is<bool>())           c.enabled          = doc["enabled"].as<bool>();
  if (doc["pn_hash"].is<const char*>())     c.pn_hash          = doc["pn_hash"].as<const char*>();
  if (doc["sync_interval_s"].is<uint32_t>()) c.sync_interval_s = doc["sync_interval_s"].as<uint32_t>();
  if (doc["retain_on_node"].is<bool>())     c.retain_on_node   = doc["retain_on_node"].as<bool>();
  if (doc["use_when_offline"].is<bool>())   c.use_when_offline = doc["use_when_offline"].as<bool>();
  sanitize(c);
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = current();
  Common::PsramJsonDocument doc;
  doc["enabled"]          = c.enabled;
  doc["pn_hash"]          = c.pn_hash;
  doc["sync_interval_s"]  = c.sync_interval_s;
  doc["retain_on_node"]   = c.retain_on_node;
  doc["use_when_offline"] = c.use_when_offline;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

inline void set(microStore::FileSystem& fs, const Config& next) {
  current() = next;
  sanitize(current());
  persist(fs);
}

// ---- PN registry (discovered nodes, RAM-only) ----------------------------

struct Node {
  RNS::Bytes  hash;                 // destination hash
  std::string name;                 // metadata name (may be empty)
  bool        active          = true;  // node_state (announce field 2)
  uint16_t    stamp_cost      = 0;     // propagation stamp cost (field 5[0])
  uint32_t    per_transfer_kb = 0;     // field 3
  uint32_t    per_sync        = 0;     // field 4
  uint32_t    last_seen_ms    = 0;
};

static constexpr size_t CAPACITY = 16;

inline std::deque<Node>& nodes() { static std::deque<Node> d; return d; }

// Best-effort parse of the announce app_data into n (hash/last_seen set by the
// caller). Returns false only when the payload isn't a usable array; partial
// arrays keep the fields parsed so far.
inline bool parse_app_data(const RNS::Bytes& app_data, Node& n) {
  const uint8_t* d = app_data.data();
  size_t len = app_data.size();
  size_t off = 0;
  int count = Common::MsgPack::read_array_header(d, len, off);
  if (count < 3) return false;                          // need at least node_state
  Common::MsgPack::skip_element(d, len, off);           // 0: legacy
  Common::MsgPack::skip_element(d, len, off);           // 1: timebase
  n.active = Common::MsgPack::read_bool(d, len, off);   // 2: node_state
  if (count < 4) return true;
  n.per_transfer_kb = (uint32_t)Common::MsgPack::read_uint(d, len, off);  // 3
  if (count < 5) return true;
  n.per_sync = (uint32_t)Common::MsgPack::read_uint(d, len, off);         // 4
  if (count < 6) return true;
  int ac = Common::MsgPack::read_array_header(d, len, off);               // 5: [cost,flex,peer]
  if (ac >= 1) {
    n.stamp_cost = (uint16_t)Common::MsgPack::read_uint(d, len, off);
    for (int i = 1; i < ac; i++) Common::MsgPack::skip_element(d, len, off);
  }
  if (count < 7) return true;
  int mc = Common::MsgPack::read_map_header(d, len, off);                 // 6: metadata
  for (int i = 0; i < mc; i++) {
    bool kok = false;
    uint64_t key = Common::MsgPack::read_uint(d, len, off, &kok);
    if (kok && key == 0x01) n.name = Common::MsgPack::read_bin_or_str(d, len, off);  // PN_META_NAME
    else Common::MsgPack::skip_element(d, len, off);
  }
  return true;
}

class Handler : public RNS::AnnounceHandler {
public:
  // aspect_filter "lxmf.propagation" => Transport only delivers PN announces.
  Handler() : RNS::AnnounceHandler("lxmf.propagation") {}

  virtual void received_announce(const RNS::Bytes& destination_hash,
                                 const RNS::Identity& announced_identity,
                                 const RNS::Bytes& app_data) override {
    Node n;
    n.hash = destination_hash;
    n.last_seen_ms = millis();
    parse_app_data(app_data, n);

    auto& d = nodes();
    for (auto& e : d) {
      if (e.hash == destination_hash) {
        e.last_seen_ms    = n.last_seen_ms;
        e.active          = n.active;
        e.stamp_cost      = n.stamp_cost;
        e.per_transfer_kb = n.per_transfer_kb;
        e.per_sync        = n.per_sync;
        if (!n.name.empty()) e.name = n.name;
        return;
      }
    }
    d.push_front(n);
    while (d.size() > CAPACITY) d.pop_back();
  }
};

inline void setup() {
  static std::shared_ptr<Handler> h;
  if (h) return;
  h = std::make_shared<Handler>();
  RNS::Transport::register_announce_handler(h);
  NOTICE("LXMF::Propagation: registered lxmf.propagation announce handler");
}

}  // namespace Propagation
}  // namespace LXMF
