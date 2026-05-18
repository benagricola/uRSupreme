// BME280 driver — temperature / humidity / pressure on the T-Beam
// Supreme's user I2C bus (Wire, SDA=17, SCL=18, addr 0x77 fallback
// 0x76). Shares the bus with the OLED display and the QMC6310
// magnetometer; cheap to coexist since each device has its own
// address and we only poll on a configurable interval.
//
// Wraps Adafruit_BME280, which speaks our msgpack-free, no-config
// Arduino dialect. We pass a Wire pointer explicitly so we don't
// accidentally collide with Wire1 (PMU + RTC at 41/42).
//
// API surface kept narrow:
//   begin(wire, addr=0x77)  — probe + init. Returns true if a chip
//                             answered at the given address; auto-
//                             falls back to 0x76 if 0x77 missed.
//   pump()                  — called from the main loop. Re-reads
//                             the sensor at most once per
//                             interval_ms (default 60_000).
//   last_reading()          — cached struct {ts_ms, temp_c,
//                             humidity_pct, pressure_pa, valid}.
//   present()               — true once begin() succeeded.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>

namespace Web {
namespace Bme280 {

struct Reading {
  bool      valid          = false;
  uint32_t  taken_ms       = 0;    // millis() at the read
  float     temp_c         = 0.0f;
  float     humidity_pct   = 0.0f;
  float     pressure_pa    = 0.0f;
};

namespace _detail {
  inline Adafruit_BME280& sensor()      { static Adafruit_BME280 s; return s; }
  inline bool&     present_ref()        { static bool v = false; return v; }
  inline Reading&  last_ref()           { static Reading r; return r; }
  inline uint32_t& interval_ms_ref()    { static uint32_t v = 60000; return v; }
  inline uint8_t&  addr_ref()           { static uint8_t v = 0; return v; }
  inline bool&     enabled_ref()        { static bool v = true; return v; }
}

inline bool begin(TwoWire& wire, uint8_t primary_addr = 0x77) {
  uint8_t tried[2] = { primary_addr, (uint8_t)(primary_addr == 0x77 ? 0x76 : 0x77) };
  for (uint8_t addr : tried) {
    if (_detail::sensor().begin(addr, &wire)) {
      _detail::present_ref() = true;
      _detail::addr_ref()    = addr;
      // Forecast-mode style sampling: oversample everything modestly,
      // 1 Hz max sample rate is plenty for ambient telemetry.
      _detail::sensor().setSampling(
          Adafruit_BME280::MODE_NORMAL,
          Adafruit_BME280::SAMPLING_X2,   // temp
          Adafruit_BME280::SAMPLING_X16,  // pressure
          Adafruit_BME280::SAMPLING_X1,   // humidity
          Adafruit_BME280::FILTER_X16,
          Adafruit_BME280::STANDBY_MS_1000);
      NOTICEF("BME280: found at 0x%02x", addr);
      return true;
    }
  }
  NOTICE("BME280: not detected on Wire (probed 0x77 + 0x76)");
  _detail::present_ref() = false;
  return false;
}

// Drive a periodic read. Cheap — only touches the bus once per
// interval_ms. Call from the main loop; gated on `enabled` so the
// user can stop monitoring entirely without unmounting the chip.
inline void pump() {
  if (!_detail::present_ref())                  return;
  if (!_detail::enabled_ref())                  return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  if (last.taken_ms != 0 && (now - last.taken_ms) < _detail::interval_ms_ref()) return;

  Reading r;
  r.taken_ms     = now;
  r.temp_c       = _detail::sensor().readTemperature();
  r.humidity_pct = _detail::sensor().readHumidity();
  r.pressure_pa  = _detail::sensor().readPressure();
  // BME280 returns NAN for any field where the sensor refused; treat
  // the whole reading as invalid in that case rather than half-publish.
  r.valid = !isnan(r.temp_c) && !isnan(r.humidity_pct) && !isnan(r.pressure_pa);
  _detail::last_ref() = r;
}

// Chip identity surfaced as a string so the SPA can display "Last read
// 12s ago (BME280)" without hard-coding the driver name. The Adafruit
// BME280 driver doesn't distinguish BMP/BME variants reliably; assume
// BME280 for now and swap when we add support for a different chip.
inline const char* model_name() { return "BME280"; }
inline bool      present()       { return _detail::present_ref(); }
inline uint8_t   address()       { return _detail::addr_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
inline uint32_t  interval_ms()   { return _detail::interval_ms_ref(); }
inline void      set_interval_ms(uint32_t ms) { _detail::interval_ms_ref() = ms; }
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

}  // namespace Bme280
}  // namespace Web
