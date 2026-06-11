// Per-sensor enable + polling-interval persistence.
//
// Each Sensors::BME280 / Sensors::QMC6310 / Sensors::QMI8658 driver has its own
// in-RAM enable + interval_ms state with sensible defaults. This
// module persists those overrides to /lxmf/sensors.json so the user
// can disable a sensor or change its poll rate from the SPA and the
// setting survives reboots.
//
// Schema (JSON):
//   {
//     "environment":       {"enabled": true,  "interval_s": 60},
//     "magnetometer": {"enabled": true,  "interval_s": 60},
//     "imu":          {"enabled": false, "interval_s": 60},
//     "gps":          {"enabled": true,  "interval_s": 3600}
//   }
//
// GPS reuses Clock::Manager's GPS source config rather than this
// store - its enable/interval are bound to the time source priority
// list. Surfaced here read-only so the popover UI can show it
// alongside the other sensors.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <microStore/FileSystem.h>

#include "Environment/BME280.h"
#include "Compass/QMC6310.h"
#include "Motion/QMI8658.h"
#include "../Clock/Manager.h"

extern microStore::FileSystem filesystem;

namespace Sensors {
namespace SensorConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/sensors.json";

// Apply the JSON at /lxmf/sensors.json on top of the drivers'
// compiled defaults. Safe to call whether or not the file exists.
// GPS is intentionally not touched here - see TimeManager.
inline void load(microStore::FileSystem& fs) {
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;

  auto apply = [&](const char* key, void (*set_enable)(bool), void (*set_iv_ms)(uint32_t)) {
    JsonObjectConst o = doc[key].as<JsonObjectConst>();
    if (o.isNull()) return;
    if (o["enabled"].is<bool>()) set_enable(o["enabled"].as<bool>());
    if (o["interval_s"].is<uint32_t>()) {
      set_iv_ms(o["interval_s"].as<uint32_t>() * 1000UL);
    }
  };
  apply("environment",       Sensors::BME280::set_enabled,  Sensors::BME280::set_interval_ms);
  apply("magnetometer", Sensors::QMC6310::set_enabled,  Sensors::QMC6310::set_interval_ms);
  apply("imu",          Sensors::QMI8658::set_enabled,  Sensors::QMI8658::set_interval_ms);
}

inline void persist(microStore::FileSystem& fs) {
  Common::PsramJsonDocument doc;
  auto write = [&](const char* key, bool en, uint32_t iv_ms) {
    JsonObject o = doc[key].to<JsonObject>();
    o["enabled"]    = en;
    o["interval_s"] = iv_ms / 1000UL;
  };
  write("environment",       Sensors::BME280::enabled(),  Sensors::BME280::interval_ms());
  write("magnetometer", Sensors::QMC6310::enabled(),  Sensors::QMC6310::interval_ms());
  write("imu",          Sensors::QMI8658::enabled(),  Sensors::QMI8658::interval_ms());
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// Apply a single { enabled, interval_s } update to one sensor and
// persist the new full config. Key is one of "environment" / "magnetometer"
// / "imu" / "gps". Returns true if applied.
//
// GPS bridges through to TimeManager: GPS lives in the sensor
// popover (it *is* a sensor - fix age, position, etc.) but its
// time-side enable / poll interval are owned by TimeManager's GPS
// source config. Persisting the GPS update therefore writes through
// to /lxmf/time.json, not /lxmf/sensors.json. The SPA's view is the
// same; the storage just lands in the right backing file.
inline bool update_one(microStore::FileSystem& fs, const char* key,
                       bool enabled, uint32_t interval_s) {
  const uint32_t iv_ms = interval_s * 1000UL;
  if (strcmp(key, "environment") == 0) {
    Sensors::BME280::set_enabled(enabled);
    Sensors::BME280::set_interval_ms(iv_ms);
  } else if (strcmp(key, "magnetometer") == 0) {
    Sensors::QMC6310::set_enabled(enabled);
    Sensors::QMC6310::set_interval_ms(iv_ms);
  } else if (strcmp(key, "imu") == 0) {
    Sensors::QMI8658::set_enabled(enabled);
    Sensors::QMI8658::set_interval_ms(iv_ms);
  } else if (strcmp(key, "gps") == 0) {
    Clock::Manager::SourceConfig c =
        Clock::Manager::get_config(Clock::Manager::Source::GPS);
    c.enabled    = enabled;
    c.interval_s = interval_s;
    Clock::Manager::set_config(Clock::Manager::Source::GPS, c);
    Clock::Manager::persist_config(fs);
    return true;
  } else {
    return false;
  }
  persist(fs);
  return true;
}

}  // namespace SensorConfig
} // namespace Sensors
