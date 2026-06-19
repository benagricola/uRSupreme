// Per-sensor enable persistence (+ the GPS location power interval).
//
// The I2C sensors (BME280 / QMC6310 / QMI8658) keep only an in-RAM
// enable flag now: their idle interval is no longer user-tunable. They
// are read on demand (Telemeter::pack -> read_now) and otherwise sample
// in the background only while a time-series feature needs it (the
// pressure trend, motion-wake). GPS is the exception - the receiver
// duty-cycles and cannot be read on demand, so it keeps a location
// interval. This module persists the overrides to /lxmf/sensors.json so
// they survive reboots.
//
// Schema (JSON):
//   {
//     "environment":  {"enabled": true,
//                      "trend": {"enabled": true, "interval_s": 300}},
//     "magnetometer": {"enabled": true},
//     "imu":          {"enabled": false, "motion_wake": {"enabled": true}},
//     "gps":          {"enabled": true,  "interval_s": 3600}
//   }
//
// The I2C sensors carry an enable flag plus, where they drive a feature,
// that feature's config: "trend" (BME280 pressure trend) is a time series
// with {enabled, interval_s}; "motion_wake" (QMI8658 motion -> GNSS
// backoff reset) is interrupt-driven, so just {enabled}. A bare interval_s
// on an I2C sensor in an older file is ignored (the removed idle-interval
// knob); GPS interval_s still applies.
//
// GPS's enabled + interval_s are the LOCATION policy (0 = always on,
// otherwise the pulsed location-update cadence) consumed by Sensors::Gnss
// for receiver power. The GPS entry in Clock::Manager's time.json is a
// different setting: how often a live fix may resync the clock. The two
// were one value historically; see load() for the migration.

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
// Magnetometer hard-iron calibration bounds, kept in their own file so a
// good compass calibration survives reboots and app reflashes.
inline constexpr const char* MAGCAL_PATH = "/lxmf/magcal.json";

// One-time migration for configs predating the location/clock split:
// the GPS interval used to live solely in Clock::Manager and did
// double duty as the power schedule. If sensors.json has no gps entry
// yet, adopt the old combined value when it looks like a power
// setting (>= 5 min was the old duty-cycle threshold); otherwise
// default to always-on so a cold receiver can actually acquire.
inline void migrate_gps_defaults() {
  // The smallest old combined interval that read as a power-saving duty
  // cycle rather than a clock-sync cadence; below it, adopt always-on.
  constexpr uint32_t OLD_DUTY_CYCLE_MIN_S = 5 * 60;
  const auto& c = Clock::Manager::get_config(Clock::Manager::Source::GPS);
  const uint32_t adopted = (c.interval_s >= OLD_DUTY_CYCLE_MIN_S) ? c.interval_s : 0;
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

  // The I2C sensors carry only an enable flag now; any interval_s in an
  // older file is ignored (the idle-interval knob was removed).
  auto apply_enable = [&](const char* key, void (*set_enable)(bool)) {
    JsonObjectConst o = doc[key].as<JsonObjectConst>();
    if (o.isNull()) return;
    if (o["enabled"].is<bool>()) set_enable(o["enabled"].as<bool>());
  };
  apply_enable("magnetometer", Sensors::QMC6310::set_enabled);
  // Environment + its pressure-trend feature.
  {
    JsonObjectConst o = doc["environment"].as<JsonObjectConst>();
    if (!o.isNull()) {
      if (o["enabled"].is<bool>()) Sensors::BME280::set_enabled(o["enabled"].as<bool>());
      JsonObjectConst t = o["trend"].as<JsonObjectConst>();
      if (!t.isNull()) {
        bool en = Sensors::BME280::trend_enabled();
        uint32_t iv_ms = Sensors::BME280::trend_interval_ms();
        if (t["enabled"].is<bool>())        en = t["enabled"].as<bool>();
        if (t["interval_s"].is<uint32_t>()) iv_ms = t["interval_s"].as<uint32_t>() * 1000UL;
        Sensors::BME280::set_trend(en, iv_ms);
      }
    }
  }
  // IMU + its motion-wake feature.
  {
    JsonObjectConst o = doc["imu"].as<JsonObjectConst>();
    if (!o.isNull()) {
      if (o["enabled"].is<bool>()) Sensors::QMI8658::set_enabled(o["enabled"].as<bool>());
      JsonObjectConst m = o["motion_wake"].as<JsonObjectConst>();
      if (!m.isNull() && m["enabled"].is<bool>()) {
        Sensors::QMI8658::set_motion_wake(m["enabled"].as<bool>());
      }
    }
  }
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
  // I2C sensors persist only their enable flag; GPS keeps its location
  // power interval (the receiver cannot be read on demand).
  auto write_enable = [&](const char* key, bool en) {
    doc[key].to<JsonObject>()["enabled"] = en;
  };
  write_enable("magnetometer", Sensors::QMC6310::enabled());
  {
    JsonObject o = doc["environment"].to<JsonObject>();
    o["enabled"] = Sensors::BME280::enabled();
    JsonObject t = o["trend"].to<JsonObject>();
    t["enabled"]    = Sensors::BME280::trend_enabled();
    t["interval_s"] = Sensors::BME280::trend_interval_ms() / 1000UL;
  }
  {
    JsonObject o = doc["imu"].to<JsonObject>();
    o["enabled"] = Sensors::QMI8658::enabled();
    o["motion_wake"].to<JsonObject>()["enabled"] = Sensors::QMI8658::motion_wake_enabled();
  }
  {
    JsonObject o = doc["gps"].to<JsonObject>();
    o["enabled"]    = Sensors::Gnss::power_config().enabled;
    o["interval_s"] = Sensors::Gnss::power_config().interval_s;
  }
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
// Restore the magnetometer calibration bounds saved by save_mag_cal.
// Call once at boot, after QMC6310::begin. Safe if the file is absent.
inline void load_mag_cal(microStore::FileSystem& fs) {
  if (!fs.exists(MAGCAL_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(MAGCAL_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  if (!doc["seeded"].as<bool>()) return;
  JsonArrayConst a = doc["b"].as<JsonArrayConst>();
  if (a.isNull() || a.size() != 6) return;
  float b[6];
  for (int i = 0; i < 6; ++i) b[i] = a[i].as<float>();
  Sensors::QMC6310::set_cal_bounds(b);
}

// Write the current magnetometer calibration bounds. Called from the
// main loop when QMC6310::take_cal_dirty() fires - that is once when a
// calibration completes or is reset, so it is a rare, small write.
inline void save_mag_cal(microStore::FileSystem& fs) {
  Common::PsramJsonDocument doc;
  const bool seeded = Sensors::QMC6310::cal_seeded();
  doc["seeded"] = seeded;
  if (seeded) {
    float b[6];
    Sensors::QMC6310::get_cal_bounds(b);
    JsonArray a = doc["b"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) a.add(b[i]);
  }
  String out;
  serializeJson(doc, out);
  fs.writeFile(MAGCAL_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// One optional feature update carried alongside a sensor's enable flag:
// the BME280 pressure trend or the QMI8658 motion-wake. `present` false
// leaves the feature untouched.
struct FeatureUpdate {
  bool     present    = false;
  bool     enabled    = true;
  uint32_t interval_s = 0;
};

inline bool update_one(microStore::FileSystem& fs, const char* key,
                       bool enabled, uint32_t interval_s,
                       const FeatureUpdate& feat = FeatureUpdate{}) {
  if (strcmp(key, "environment") == 0) {
    Sensors::BME280::set_enabled(enabled);
    if (feat.present) Sensors::BME280::set_trend(feat.enabled, feat.interval_s * 1000UL);
  } else if (strcmp(key, "magnetometer") == 0) {
    Sensors::QMC6310::set_enabled(enabled);
  } else if (strcmp(key, "imu") == 0) {
    Sensors::QMI8658::set_enabled(enabled);
    if (feat.present) Sensors::QMI8658::set_motion_wake(feat.enabled);
  } else if (strcmp(key, "gps") == 0) {
    // GPS keeps its location interval (the receiver duty-cycles and
    // cannot be read on demand); the I2C sensors ignore interval_s.
    Sensors::Gnss::set_power_config(enabled, interval_s);
  } else {
    return false;
  }
  persist(fs);
  return true;
}

}  // namespace SensorConfig
} // namespace Sensors
