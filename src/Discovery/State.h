// Master discovery state — /reticulum/discovery.json.
//
// Three knobs:
//   master_enabled:        global on/off for the announcer (default false)
//   default_interval_min:  how often each discoverable interface emits
//                          (default 360 min, matches upstream)
//   default_stamp_cost:    stamp PoW difficulty in leading zero bits.
//                          Default 14 matches the upstream listener's
//                          required_value, so downstream RNS listeners and other RNS
//                          nodes following the reference impl accept
//                          our announces out of the box. Set to 0 to
//                          disable the PoW entirely (worker emits a
//                          zero-filled stamp; stricter listeners drop).
//
// Lives separately from interfaces.json (which is the per-interface
// registry) so the master toggle has its own clean read/write surface
// without forcing a full registry rewrite when the user just wants
// to flip the global enable.

#pragma once

#include <Arduino.h>
#include <Log.h>
#include <ArduinoJson.h>
#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"

extern microStore::FileSystem filesystem;

namespace Discovery {
namespace State {

inline constexpr const char* PERSIST_PATH = "/reticulum/discovery.json";

inline constexpr uint32_t DEFAULT_INTERVAL_MIN     = 360;   // 6 h, upstream default
inline constexpr uint32_t MIN_INTERVAL_MIN         = 5;     // upstream-enforced floor
inline constexpr uint32_t MAX_INTERVAL_MIN         = 1440;  // 24 h UI cap
inline constexpr uint32_t DEFAULT_STAMP_COST       = 14;    // upstream listener's required_value

// Cap on the user-set advertisement label. The on-air announce
// app_data carries this as the interface's `name` field. Sideband-
// shape consumers (downstream RNS listeners, other RNS nodes) treat it as a
// human-readable label, so anything in the 32-64 char range is
// reasonable; 64 covers most "MyNode in the garden shed" use.
inline constexpr size_t MAX_ADVERTISED_NAME_BYTES = 64;

struct Master {
  bool        enabled            = false;                  // privacy-by-design default
  uint32_t    default_interval_min = DEFAULT_INTERVAL_MIN;
  uint32_t    default_stamp_cost   = DEFAULT_STAMP_COST;
  // User-set label included in every announce. Empty string means
  // "use the interface name" — the default fallback the Announcer
  // applies when this is unset.
  std::string advertised_name;
};

namespace _detail {
  inline Master& slot() { static Master m; return m; }
  inline bool& loaded_ref() { static bool v = false; return v; }
}

inline void load() {
  Master& m = _detail::slot();
  m = Master{};                                        // reset to defaults
  _detail::loaded_ref() = true;
  if (!filesystem.exists(PERSIST_PATH)) return;
  const size_t sz = (size_t)filesystem.size(PERSIST_PATH);
  if (sz == 0) return;
  std::vector<uint8_t> buf(sz);
  microStore::File f = filesystem.open(PERSIST_PATH,
                                        microStore::File::ModeRead, false);
  if (!f) { WARNINGF("Discovery::State: failed to open %s", PERSIST_PATH); return; }
  const size_t r = f.read(buf.data(), sz);
  f.close();
  if (r != sz) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, buf.data(), sz) != DeserializationError::Ok) {
    WARNINGF("Discovery::State: %s did not parse", PERSIST_PATH);
    return;
  }
  m.enabled              = doc["enabled"]              | false;
  m.default_interval_min = doc["default_interval_min"] | DEFAULT_INTERVAL_MIN;
  m.default_stamp_cost   = doc["default_stamp_cost"]   | DEFAULT_STAMP_COST;
  m.advertised_name      = (const char*)(doc["advertised_name"] | "");
  if (m.advertised_name.size() > MAX_ADVERTISED_NAME_BYTES) {
    m.advertised_name.resize(MAX_ADVERTISED_NAME_BYTES);
  }
  // Clamp interval to the documented range.
  if (m.default_interval_min < MIN_INTERVAL_MIN) m.default_interval_min = MIN_INTERVAL_MIN;
  if (m.default_interval_min > MAX_INTERVAL_MIN) m.default_interval_min = MAX_INTERVAL_MIN;
}

inline bool save() {
  if (!filesystem.isDirectory("/reticulum")) filesystem.mkdir("/reticulum");
  const Master& m = _detail::slot();
  Common::PsramJsonDocument doc;
  doc["enabled"]              = m.enabled;
  doc["default_interval_min"] = m.default_interval_min;
  doc["default_stamp_cost"]   = m.default_stamp_cost;
  doc["advertised_name"]      = m.advertised_name;
  std::string out;
  serializeJson(doc, out);
  const size_t w = filesystem.writeFile(PERSIST_PATH,
      reinterpret_cast<const uint8_t*>(out.data()), out.size());
  return w == out.size();
}

inline const Master& current() {
  if (!_detail::loaded_ref()) load();
  return _detail::slot();
}

inline bool set_enabled(bool v) {
  if (!_detail::loaded_ref()) load();
  _detail::slot().enabled = v;
  return save();
}
inline bool set_default_interval_min(uint32_t m) {
  if (!_detail::loaded_ref()) load();
  if (m < MIN_INTERVAL_MIN) m = MIN_INTERVAL_MIN;
  if (m > MAX_INTERVAL_MIN) m = MAX_INTERVAL_MIN;
  _detail::slot().default_interval_min = m;
  return save();
}
inline bool set_default_stamp_cost(uint32_t c) {
  if (!_detail::loaded_ref()) load();
  _detail::slot().default_stamp_cost = c;
  return save();
}
inline bool set_advertised_name(const std::string& n) {
  if (!_detail::loaded_ref()) load();
  std::string clipped = n;
  if (clipped.size() > MAX_ADVERTISED_NAME_BYTES) clipped.resize(MAX_ADVERTISED_NAME_BYTES);
  _detail::slot().advertised_name = clipped;
  return save();
}

}  // namespace State
}  // namespace Discovery
