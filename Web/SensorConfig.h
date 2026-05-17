// Per-sensor enable + polling-interval persistence.
//
// Each Web::Bme280 / Web::QmcMag / Web::QmiImu driver has its own
// in-RAM enable + interval_ms state with sensible defaults. This
// module persists those overrides to /lxmf/sensors.json so the user
// can disable a sensor or change its poll rate from the SPA and the
// setting survives reboots.
//
// Schema (JSON):
//   {
//     "bme280":       {"enabled": true,  "interval_s": 60},
//     "magnetometer": {"enabled": true,  "interval_s": 60},
//     "imu":          {"enabled": false, "interval_s": 60},
//     "gps":          {"enabled": true,  "interval_s": 3600}
//   }
//
// GPS reuses Web::TimeManager's GPS source config rather than this
// store — its enable/interval are bound to the time source priority
// list. Surfaced here read-only so the popover UI can show it
// alongside the other sensors. (#131)

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <microStore/FileSystem.h>

#include "Bme280.h"
#include "QmcMag.h"
#include "QmiImu.h"
#include "TimeManager.h"

extern microStore::FileSystem filesystem;

namespace Web {
namespace SensorConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/sensors.json";

// Apply the JSON at /lxmf/sensors.json on top of the drivers'
// compiled defaults. Safe to call whether or not the file exists.
// GPS is intentionally not touched here — see TimeManager.
inline void load(microStore::FileSystem& fs) {
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;

  auto apply = [&](const char* key, void (*set_enable)(bool), void (*set_iv_ms)(uint32_t)) {
    JsonObjectConst o = doc[key].as<JsonObjectConst>();
    if (o.isNull()) return;
    if (o["enabled"].is<bool>()) set_enable(o["enabled"].as<bool>());
    if (o["interval_s"].is<uint32_t>()) {
      set_iv_ms(o["interval_s"].as<uint32_t>() * 1000UL);
    }
  };
  apply("bme280",       Web::Bme280::set_enabled,  Web::Bme280::set_interval_ms);
  apply("magnetometer", Web::QmcMag::set_enabled,  Web::QmcMag::set_interval_ms);
  apply("imu",          Web::QmiImu::set_enabled,  Web::QmiImu::set_interval_ms);
}

inline void persist(microStore::FileSystem& fs) {
  JsonDocument doc;
  auto write = [&](const char* key, bool en, uint32_t iv_ms) {
    JsonObject o = doc[key].to<JsonObject>();
    o["enabled"]    = en;
    o["interval_s"] = iv_ms / 1000UL;
  };
  write("bme280",       Web::Bme280::enabled(),  Web::Bme280::interval_ms());
  write("magnetometer", Web::QmcMag::enabled(),  Web::QmcMag::interval_ms());
  write("imu",          Web::QmiImu::enabled(),  Web::QmiImu::interval_ms());
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// Apply a single { enabled, interval_s } update to one sensor and
// persist the new full config. Key is one of "bme280" / "magnetometer"
// / "imu". Returns true if applied.
inline bool update_one(microStore::FileSystem& fs, const char* key,
                       bool enabled, uint32_t interval_s) {
  const uint32_t iv_ms = interval_s * 1000UL;
  if (strcmp(key, "bme280") == 0) {
    Web::Bme280::set_enabled(enabled);
    Web::Bme280::set_interval_ms(iv_ms);
  } else if (strcmp(key, "magnetometer") == 0) {
    Web::QmcMag::set_enabled(enabled);
    Web::QmcMag::set_interval_ms(iv_ms);
  } else if (strcmp(key, "imu") == 0) {
    Web::QmiImu::set_enabled(enabled);
    Web::QmiImu::set_interval_ms(iv_ms);
  } else {
    return false;
  }
  persist(fs);
  return true;
}

}  // namespace SensorConfig
}  // namespace Web
