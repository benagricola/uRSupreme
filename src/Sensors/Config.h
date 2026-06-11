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
// GPS owns a real entry here like every other sensor: enabled +
// interval_s are the LOCATION policy (0 = always on, otherwise the
// pulsed location-update cadence) consumed by Sensors::Gnss for
// receiver power. The GPS entry in Clock::Manager's time.json is a
// different setting: how often a live fix may resync the clock. The
// two were one value historically; see load() for the migration.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <microStore/FileSystem.h>

#include "Environment/BME280.h"
#include "Compass/QMC6310.h"
#include "Motion/QMI8658.h"
#include "Position/Gnss.h"
#include "../Clock/Manager.h"

extern microStore::FileSystem filesystem;

namespace Sensors {
namespace SensorConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/sensors.json";

// One-time migration for configs predating the location/clock split:
// the GPS interval used to live solely in Clock::Manager and did
// double duty as the power schedule. If sensors.json has no gps entry
// yet, adopt the old combined value when it looks like a power
// setting (>= 5 min was the old duty-cycle threshold); otherwise
// default to always-on so a cold receiver can actually acquire.
inline void migrate_gps_defaults() {
  const auto& c = Clock::Manager::get_config(Clock::Manager::Source::GPS);
  const uint32_t adopted = (c.interval_s >= 300) ? c.interval_s : 0;
  Sensors::Gnss::set_power_config(c.enabled, adopted);
}

// Apply the JSON at /lxmf/sensors.json on top of the drivers'
// compiled defaults. Safe to call whether or not the file exists.
inline void load(microStore::FileSystem& fs) {
  migrate_gps_defaults();
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
  {
    JsonObjectConst o = doc["gps"].as<JsonObjectConst>();
    if (!o.isNull()) {
      bool en = Sensors::Gnss::power_config().enabled;
      uint32_t iv = Sensors::Gnss::power_config().interval_s;
      if (o["enabled"].is<bool>())        en = o["enabled"].as<bool>();
      if (o["interval_s"].is<uint32_t>()) iv = o["interval_s"].as<uint32_t>();
      Sensors::Gnss::set_power_config(en, iv);
    }
  }
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
  write("gps",          Sensors::Gnss::power_config().enabled,
                        Sensors::Gnss::power_config().interval_s * 1000UL);
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
// GPS persists here like every other sensor; the update reaches
// Sensors::Gnss immediately (power mode follows on its next pump).
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
    Sensors::Gnss::set_power_config(enabled, interval_s);
  } else {
    return false;
  }
  persist(fs);
  return true;
}

}  // namespace SensorConfig
} // namespace Sensors
