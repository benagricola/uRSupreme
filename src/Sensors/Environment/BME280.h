// BME280 driver - temperature / humidity / pressure on the T-Beam
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
//   begin(wire, addr=0x77)  - probe + init. Returns true if a chip
//                             answered at the given address; auto-
//                             falls back to 0x76 if 0x77 missed.
//   pump()                  - called from the main loop. Re-reads
//                             the sensor at most once per
//                             interval_ms (default 60_000).
//   last_reading()          - cached struct {ts_ms, temp_c,
//                             humidity_pct, pressure_pa, valid}.
//   present()               - true once begin() succeeded.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BME280.h>

namespace Sensors {
namespace BME280 {

struct Reading {
  bool      valid          = false;
  uint32_t  taken_ms       = 0;    // millis() at the read
  float     temp_c         = 0.0f;
  float     humidity_pct   = 0.0f;
  float     pressure_pa    = 0.0f;
};

// Pressure-trend history for the climate screen's graph: one point every
// PRESS_HIST_MS, so PRESS_HIST_N points span a few hours. Sampled in
// pump() (which runs continuously) so the trend builds whether or not
// the screen is open.
inline constexpr int      PRESS_HIST_N  = 48;
inline constexpr uint32_t PRESS_HIST_MS = 5UL * 60 * 1000;  // 5 min -> ~4 h window

namespace _detail {
  inline Adafruit_BME280& sensor()      { static Adafruit_BME280 s; return s; }
  inline bool&     present_ref()        { static bool v = false; return v; }
  inline Reading&  last_ref()           { static Reading r; return r; }
  inline uint32_t& interval_ms_ref()    { static uint32_t v = 60000; return v; }
  inline uint32_t& live_until_ref()     { static uint32_t v = 0; return v; }
  inline uint8_t&  addr_ref()           { static uint8_t v = 0; return v; }
  inline bool&     enabled_ref()        { static bool v = true; return v; }
  struct PressHist { float p[PRESS_HIST_N] = {0}; int head = 0, count = 0; uint32_t last_ms = 0; };
  inline PressHist& press_hist_ref() { static PressHist h; return h; }
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

// Drive a periodic read. Cheap - only touches the bus once per
// interval_ms. Call from the main loop; gated on `enabled` so the
// user can stop monitoring entirely without unmounting the chip.
// Fast poll period while a live demand is active.
inline constexpr uint32_t LIVE_POLL_MS = 500;

inline void pump() {
  if (!_detail::present_ref())                  return;
  if (!_detail::enabled_ref())                  return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  // interval_ms == 0 means "boot-read only": take one reading on the
  // first pump() after boot, then never again until reboot. Without
  // this, the SPA's "At boot only" preset (which sends interval_s=0)
  // would cause the sensor to read every main-loop iteration because
  // `(now - taken_ms) < 0` is always false for uint32_t.
  const bool live = now < _detail::live_until_ref();
  const uint32_t eff_interval = live ? LIVE_POLL_MS : _detail::interval_ms_ref();
  if (!live && _detail::interval_ms_ref() == 0 && last.taken_ms != 0) return;
  if (last.taken_ms != 0 && (now - last.taken_ms) < eff_interval) return;

  Reading r;
  r.taken_ms     = now;
  r.temp_c       = _detail::sensor().readTemperature();
  r.humidity_pct = _detail::sensor().readHumidity();
  r.pressure_pa  = _detail::sensor().readPressure();
  // BME280 returns NAN for any field where the sensor refused; treat
  // the whole reading as invalid in that case rather than half-publish.
  r.valid = !isnan(r.temp_c) && !isnan(r.humidity_pct) && !isnan(r.pressure_pa);
  _detail::last_ref() = r;
  // Append to the pressure trend at most once per PRESS_HIST_MS.
  if (r.valid) {
    _detail::PressHist& h = _detail::press_hist_ref();
    if (h.last_ms == 0 || (now - h.last_ms) >= PRESS_HIST_MS) {
      h.p[h.head] = r.pressure_pa;
      h.head = (h.head + 1) % PRESS_HIST_N;
      if (h.count < PRESS_HIST_N) h.count++;
      h.last_ms = now;
    }
  }
}

// Fill `out` with the pressure history oldest-first; returns the count.
inline int pressure_history(float* out, int max) {
  const _detail::PressHist& h = _detail::press_hist_ref();
  const int n = h.count < max ? h.count : max;
  for (int i = 0; i < n; ++i) {
    const int idx = (h.head - h.count + i + 2 * PRESS_HIST_N) % PRESS_HIST_N;
    out[i] = h.p[idx];
  }
  return n;
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
// While live (a screen showing this sensor is open), poll fast
// instead of at the idle interval. The consumer renews the TTL.
inline void request_live(uint32_t ttl_ms = 1500) {
  _detail::live_until_ref() = millis() + ttl_ms;
}
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

} // namespace BME280
} // namespace Sensors
