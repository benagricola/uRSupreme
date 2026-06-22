// User-tunable power / idle-display behaviour, persisted to
// /lxmf/power.json and exposed by the web app's Power tab. Pure data plus
// JSON persistence: the consumers (Display blanking, the QMI8658 pickup
// detector, the charge-LED heartbeat) pull current() directly, so this
// header carries no driver dependencies and belongs in Common/.
//
// Fields:
//   blank_enabled       false = the idle screen never blanks (always on).
//   blank_timeout_s     idle delay before the home screen blanks.
//   wake_threshold_mg   gravity-vector deviation that counts as a pickup
//                       (the web app maps Low/Medium/High to this).
//   heartbeat_enabled   the idle "still alive" charge-LED blip.
//   gps_motion_retry_s  sustained-movement duration before the GPS backoff
//                       is reset (a probable relocation); 0 = never.

#pragma once

#include <stdint.h>
#include <vector>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "PsramAllocator.h"
#include <microStore/FileSystem.h>

namespace Common {
namespace PowerConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/power.json";

struct Config {
  bool     blank_enabled      = true;
  uint32_t blank_timeout_s    = 60;
  uint16_t wake_threshold_mg  = 120;
  bool     heartbeat_enabled  = true;
  uint32_t gps_motion_retry_s = 20;
};

inline Config& current() { static Config c; return c; }

// Clamp to sane ranges so a malformed file or API value can't wedge a
// behaviour (a 0 s blank timeout that blanks instantly, a sub-noise wake
// threshold that never settles).
inline void sanitize(Config& c) {
  if (c.blank_timeout_s < 5)             c.blank_timeout_s = 5;
  if (c.blank_timeout_s > 24UL * 3600)   c.blank_timeout_s = 24UL * 3600;
  if (c.wake_threshold_mg < 20)          c.wake_threshold_mg = 20;
  if (c.wake_threshold_mg > 1000)        c.wake_threshold_mg = 1000;
  if (c.gps_motion_retry_s > 24UL * 3600) c.gps_motion_retry_s = 24UL * 3600;
}

inline void load(microStore::FileSystem& fs) {
  Config& c = current();
  c = Config{};                       // defaults first
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  if (doc["blank_enabled"].is<bool>())          c.blank_enabled      = doc["blank_enabled"].as<bool>();
  if (doc["blank_timeout_s"].is<uint32_t>())    c.blank_timeout_s    = doc["blank_timeout_s"].as<uint32_t>();
  if (doc["wake_threshold_mg"].is<uint32_t>())  c.wake_threshold_mg  = (uint16_t)doc["wake_threshold_mg"].as<uint32_t>();
  if (doc["heartbeat_enabled"].is<bool>())      c.heartbeat_enabled  = doc["heartbeat_enabled"].as<bool>();
  if (doc["gps_motion_retry_s"].is<uint32_t>()) c.gps_motion_retry_s = doc["gps_motion_retry_s"].as<uint32_t>();
  sanitize(c);
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = current();
  Common::PsramJsonDocument doc;
  doc["blank_enabled"]      = c.blank_enabled;
  doc["blank_timeout_s"]    = c.blank_timeout_s;
  doc["wake_threshold_mg"]  = c.wake_threshold_mg;
  doc["heartbeat_enabled"]  = c.heartbeat_enabled;
  doc["gps_motion_retry_s"] = c.gps_motion_retry_s;
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

}  // namespace PowerConfig
}  // namespace Common
