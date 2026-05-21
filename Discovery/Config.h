// Discovery / interfaces config: /reticulum/interfaces.json.
//
// One JSON file keyed by interface name. Each entry carries the
// interface's type (lora / tcp_client / udp) + its discoverable
// flag + type-specific config (host/port for tcp_client, port +
// enabled for udp; lora has no per-row config here — its hardware
// params live in EEPROM and are read before the filesystem is
// mounted, so only the name + discoverable bit live in this file).
//
// Each user-creatable interface has its definition AND its
// discoverable bit in the same entry — deleting an entry removes
// both atomically. No stale flags to clean up after an interface
// is removed. LoRa is the special case: always-present, params
// elsewhere, so its entry just records the discoverable flag.
//
// Schema:
//   {
//     "LoRaInterface": { "type": "lora",       "discoverable": false },
//     "rmap":          { "type": "tcp_client", "host": "rmap.world",
//                        "port": 4242,         "discoverable": true  },
//     "udp0":          { "type": "udp",        "port": 4242,
//                        "enabled": false,     "discoverable": false }
//   }
//
// Master discovery state (master_enabled, default_interval_min,
// default_stamp_cost) lives in a SEPARATE file (/reticulum/discovery.json,
// owned by Discovery::State) so this file is purely the per-interface
// registry.

#pragma once

#include <Arduino.h>
#include <Log.h>
#include <Transport.h>
#include <ArduinoJson.h>
#include <map>
#include <string>
#include <vector>
#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"

extern microStore::FileSystem filesystem;

namespace Discovery {
namespace Config {

inline constexpr const char* PERSIST_PATH = "/reticulum/interfaces.json";

enum class Type : uint8_t {
  Unknown    = 0,
  Lora       = 1,
  TcpClient  = 2,
  Udp        = 3,
};

struct Entry {
  Type        type         = Type::Unknown;
  bool        discoverable = false;
  // tcp_client / udp:
  std::string host;
  int         port    = 0;
  // udp:
  bool        enabled = false;
};

namespace _detail {
  inline std::map<std::string, Entry>& entries_ref() {
    static std::map<std::string, Entry> m;
    return m;
  }
  inline bool& loaded_ref() { static bool v = false; return v; }

  inline const char* type_to_str(Type t) {
    switch (t) {
      case Type::Lora:      return "lora";
      case Type::TcpClient: return "tcp_client";
      case Type::Udp:       return "udp";
      default:              return "unknown";
    }
  }
  inline Type type_from_str(const char* s) {
    if (!s) return Type::Unknown;
    if (strcmp(s, "lora")       == 0) return Type::Lora;
    if (strcmp(s, "tcp_client") == 0) return Type::TcpClient;
    if (strcmp(s, "udp")        == 0) return Type::Udp;
    return Type::Unknown;
  }
}

// Reload the file from disk into the in-memory map. Safe to call
// before the file exists — leaves the map empty in that case.
// Idempotent on its own; callers can re-load to pick up out-of-band
// edits (e.g. after a CRUD endpoint writes).
inline void load() {
  auto& map = _detail::entries_ref();
  map.clear();
  _detail::loaded_ref() = true;
  if (!filesystem.exists(PERSIST_PATH)) return;
  const size_t sz = (size_t)filesystem.size(PERSIST_PATH);
  if (sz == 0) return;
  std::vector<uint8_t> buf(sz);
  microStore::File f = filesystem.open(PERSIST_PATH,
                                        microStore::File::ModeRead, false);
  if (!f) { WARNINGF("Discovery::Config: failed to open %s", PERSIST_PATH); return; }
  const size_t r = f.read(buf.data(), sz);
  f.close();
  if (r != sz) {
    WARNINGF("Discovery::Config: short read on %s (%u/%u)", PERSIST_PATH,
             (unsigned)r, (unsigned)sz);
    return;
  }
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, buf.data(), sz) != DeserializationError::Ok) {
    WARNINGF("Discovery::Config: %s did not parse as JSON — ignoring",
             PERSIST_PATH);
    return;
  }
  for (JsonPair kv : doc.as<JsonObject>()) {
    Entry e;
    e.type         = _detail::type_from_str(kv.value()["type"] | "");
    e.discoverable = kv.value()["discoverable"] | false;
    e.host         = (const char*)(kv.value()["host"] | "");
    e.port         = kv.value()["port"]    | 0;
    e.enabled      = kv.value()["enabled"] | false;
    map[std::string(kv.key().c_str())] = e;
  }
  NOTICEF("Discovery::Config: loaded %u interface entries from %s",
          (unsigned)map.size(), PERSIST_PATH);
}

// Write the in-memory map back to disk. Creates /reticulum/ if
// missing. Returns true on full write.
inline bool save() {
  if (!filesystem.isDirectory("/reticulum")) filesystem.mkdir("/reticulum");
  Common::PsramJsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  for (const auto& kv : _detail::entries_ref()) {
    JsonObject o = root[kv.first].to<JsonObject>();
    o["type"]         = _detail::type_to_str(kv.second.type);
    o["discoverable"] = kv.second.discoverable;
    if (kv.second.type == Type::TcpClient || kv.second.type == Type::Udp) {
      if (kv.second.type == Type::TcpClient) o["host"] = kv.second.host;
      o["port"] = kv.second.port;
    }
    if (kv.second.type == Type::Udp) {
      o["enabled"] = kv.second.enabled;
    }
  }
  std::string out;
  out.reserve(256);
  serializeJson(doc, out);
  const size_t w = filesystem.writeFile(PERSIST_PATH,
      reinterpret_cast<const uint8_t*>(out.data()), out.size());
  if (w != out.size()) {
    ERRORF("Discovery::Config: short write on %s (%u/%u)", PERSIST_PATH,
           (unsigned)w, (unsigned)out.size());
    return false;
  }
  return true;
}

// Look up an entry. Returns true if found (out_entry filled).
inline bool get(const std::string& name, Entry* out_entry = nullptr) {
  if (!_detail::loaded_ref()) load();
  auto it = _detail::entries_ref().find(name);
  if (it == _detail::entries_ref().end()) return false;
  if (out_entry) *out_entry = it->second;
  return true;
}

// Upsert by name. Persists immediately. Returns the save() result.
inline bool upsert(const std::string& name, const Entry& e) {
  if (!_detail::loaded_ref()) load();
  _detail::entries_ref()[name] = e;
  return save();
}

// Set or update just the discoverable flag for an existing entry,
// or create a minimal entry of the given type if none exists. The
// type argument is used only for the creation path; existing entries
// keep their type.
inline bool set_discoverable(const std::string& name, bool v,
                              Type create_type_if_missing = Type::Unknown) {
  if (!_detail::loaded_ref()) load();
  auto it = _detail::entries_ref().find(name);
  if (it != _detail::entries_ref().end()) {
    it->second.discoverable = v;
  } else {
    Entry e;
    e.type = create_type_if_missing;
    e.discoverable = v;
    _detail::entries_ref()[name] = e;
  }
  return save();
}

// Remove an entry by name. Returns true if removed.
inline bool remove(const std::string& name) {
  if (!_detail::loaded_ref()) load();
  auto it = _detail::entries_ref().find(name);
  if (it == _detail::entries_ref().end()) return false;
  _detail::entries_ref().erase(it);
  return save();
}

// Iterate the cached entries (read-only).
inline const std::map<std::string, Entry>& all() {
  if (!_detail::loaded_ref()) load();
  return _detail::entries_ref();
}

// Convenience: "is the interface named X currently set to discoverable?"
// Returns false when no entry exists for the name (privacy-by-design).
// This is the read path the upcoming Discovery::Announcer will hit
// when deciding which interfaces to emit announces on — it iterates
// RNS::Transport::get_interfaces() and consults this for each.
//
// We deliberately do NOT mirror the flag onto InterfaceImpl /
// Interface — that would invade upstream microReticulum to carry a
// flag whose only consumer is our application-layer announcer, and
// keeps two sources of truth in sync forever. Single source of truth:
// the JSON-backed map here.
inline bool is_discoverable(const std::string& name) {
  if (!_detail::loaded_ref()) load();
  const auto& map = _detail::entries_ref();
  auto it = map.find(name);
  return (it != map.end()) && it->second.discoverable;
}

// Log a one-line summary of which interfaces (by name) are currently
// configured as discoverable — useful for boot diagnostics. Doesn't
// mutate anything.
inline void log_summary() {
  if (!_detail::loaded_ref()) load();
  const auto& map = _detail::entries_ref();
  size_t count = 0;
  for (const auto& kv : map) if (kv.second.discoverable) ++count;
  NOTICEF("Discovery::Config: %u entries loaded, %u marked discoverable",
          (unsigned)map.size(), (unsigned)count);
}

}  // namespace Config
}  // namespace Discovery
