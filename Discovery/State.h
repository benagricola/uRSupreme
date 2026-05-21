// Master discovery state — /reticulum/discovery.json.
//
// Three knobs:
//   master_enabled:        global on/off for the announcer (default false)
//   default_interval_min:  how often each discoverable interface emits
//                          (default 360 min, matches upstream)
//   default_stamp_cost:    stamp PoW difficulty in leading zero bits
//                          (default 0 in v1 — stamp impl is a later
//                          commit, listeners with required_value=0
//                          accept zero-stamps)
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
#include "../Web/PsramAllocator.h"

extern microStore::FileSystem filesystem;

namespace Discovery {
namespace State {

inline constexpr const char* PERSIST_PATH = "/reticulum/discovery.json";

inline constexpr uint32_t DEFAULT_INTERVAL_MIN     = 360;   // 6 h, upstream default
inline constexpr uint32_t MIN_INTERVAL_MIN         = 5;     // upstream-enforced floor
inline constexpr uint32_t MAX_INTERVAL_MIN         = 1440;  // 24 h UI cap
inline constexpr uint32_t DEFAULT_STAMP_COST       = 0;     // PoW deferred — see Announce.h

struct Master {
  bool     enabled            = false;                  // privacy-by-design default
  uint32_t default_interval_min = DEFAULT_INTERVAL_MIN;
  uint32_t default_stamp_cost   = DEFAULT_STAMP_COST;
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
  Web::PsramJsonDocument doc;
  if (deserializeJson(doc, buf.data(), sz) != DeserializationError::Ok) {
    WARNINGF("Discovery::State: %s did not parse", PERSIST_PATH);
    return;
  }
  m.enabled              = doc["enabled"]              | false;
  m.default_interval_min = doc["default_interval_min"] | DEFAULT_INTERVAL_MIN;
  m.default_stamp_cost   = doc["default_stamp_cost"]   | DEFAULT_STAMP_COST;
  // Clamp interval to the documented range.
  if (m.default_interval_min < MIN_INTERVAL_MIN) m.default_interval_min = MIN_INTERVAL_MIN;
  if (m.default_interval_min > MAX_INTERVAL_MIN) m.default_interval_min = MAX_INTERVAL_MIN;
}

inline bool save() {
  if (!filesystem.isDirectory("/reticulum")) filesystem.mkdir("/reticulum");
  const Master& m = _detail::slot();
  Web::PsramJsonDocument doc;
  doc["enabled"]              = m.enabled;
  doc["default_interval_min"] = m.default_interval_min;
  doc["default_stamp_cost"]   = m.default_stamp_cost;
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

}  // namespace State
}  // namespace Discovery
