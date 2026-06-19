// BME280 driver - temperature / humidity / pressure on the T-Beam
// Supreme's user I2C bus (Wire, SDA=17, SCL=18, addr 0x77 fallback
// 0x76). Shares the bus with the OLED display and the QMC6310
// magnetometer; cheap to coexist since each device has its own
// address and each read is a few ms.
//
// Wraps Adafruit_BME280, which speaks our msgpack-free, no-config
// Arduino dialect. We pass a Wire pointer explicitly so we don't
// accidentally collide with Wire1 (PMU + RTC at 41/42).
//
// API surface kept narrow:
//   begin(wire, addr=0x77)  - probe + init. Returns true if a chip
//                             answered at the given address; auto-
//                             falls back to 0x76 if 0x77 missed.
//   pump()                  - called from the main loop. Polls fast
//                             while a live demand is active, else at the
//                             pressure-trend interval while that feature
//                             is on; read on demand otherwise.
//   read_now()              - one-shot read bypassing the cadence, for
//                             pack-time telemetry freshness.
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

// Pressure-trend history for the climate screen's graph: PRESS_HIST_N
// points, one per trend interval (set_trend), so the graph spans
// interval x PRESS_HIST_N. PRESS_HIST_MS is the default interval (a 5 min
// point gives a ~4 h window). The trend builds in the background whether
// or not the screen is open, while the feature is enabled.
inline constexpr int      PRESS_HIST_N  = 48;
inline constexpr uint32_t PRESS_HIST_MS = 5UL * 60 * 1000;  // default: 5 min -> ~4 h window

namespace _detail {
  inline Adafruit_BME280& sensor()      { static Adafruit_BME280 s; return s; }
  inline bool&     present_ref()        { static bool v = false; return v; }
  inline Reading&  last_ref()           { static Reading r; return r; }
  inline uint32_t& live_until_ref()     { static uint32_t v = 0; return v; }
  inline uint8_t&  addr_ref()           { static uint8_t v = 0; return v; }
  inline bool&     enabled_ref()        { static bool v = true; return v; }
  struct PressHist { float p[PRESS_HIST_N] = {0}; int head = 0, count = 0; uint32_t last_ms = 0; };
  inline PressHist& press_hist_ref() { static PressHist h; return h; }
  // Pressure-trend feature: when enabled, the sensor background-samples
  // at trend_interval_ms and appends one trend point per interval.
  // Disabled means no background sampling (read on demand / live only)
  // and no appends.
  inline bool&     trend_enabled_ref()     { static bool v = true; return v; }
  inline uint32_t& trend_interval_ms_ref() { static uint32_t v = PRESS_HIST_MS; return v; }

  // One read into the cache, bypassing any cadence gate. While the
  // pressure-trend feature is on, a point is appended at most once per
  // trend_interval_ms, so a fast live poll (or a pack-time read_now)
  // cannot over-fill the ring.
  inline void do_read(uint32_t now) {
    Reading r;
    r.taken_ms     = now;
    r.temp_c       = sensor().readTemperature();
    r.humidity_pct = sensor().readHumidity();
    r.pressure_pa  = sensor().readPressure();
    // BME280 returns NAN for any field where the sensor refused; treat
    // the whole reading as invalid in that case rather than half-publish.
    r.valid = !isnan(r.temp_c) && !isnan(r.humidity_pct) && !isnan(r.pressure_pa);
    last_ref() = r;
    if (r.valid && trend_enabled_ref()) {
      PressHist& h = press_hist_ref();
      if (h.last_ms == 0 || (now - h.last_ms) >= trend_interval_ms_ref()) {
        h.p[h.head] = r.pressure_pa;
        h.head = (h.head + 1) % PRESS_HIST_N;
        if (h.count < PRESS_HIST_N) h.count++;
        h.last_ms = now;
      }
    }
  }
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

// Fast poll period while a live demand is active (popover / OLED screen).
inline constexpr uint32_t LIVE_POLL_MS = 500;

// Drive the read cadence from the main loop. Cheap - one I2C read at most
// per period. Gated on `enabled` so the user can stop monitoring entirely
// without unmounting the chip. When nothing is live, the sensor
// background-samples only while the pressure-trend feature is on (at its
// interval); otherwise it is read on demand (Telemeter::pack -> read_now),
// not on a timer.
inline void pump() {
  if (!_detail::present_ref()) return;
  if (!_detail::enabled_ref()) return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  uint32_t eff_interval;
  if (now < _detail::live_until_ref()) {
    eff_interval = LIVE_POLL_MS;
  } else if (_detail::trend_enabled_ref()) {
    eff_interval = _detail::trend_interval_ms_ref();   // build the trend
  } else {
    return;   // no live demand and trend off -> read on demand only
  }
  if (last.taken_ms != 0 && (now - last.taken_ms) < eff_interval) return;
  _detail::do_read(now);
}

// Force a fresh read now, bypassing the cadence gate. Telemetry packs
// the cached reading, so a passive packer (collector, live-share grant,
// announce) calls this just before packing to send current data without
// a standing idle poll. A single I2C read, a few ms; no-op if the chip
// is absent or the sensor disabled.
inline void read_now() {
  if (!_detail::present_ref() || !_detail::enabled_ref()) return;
  _detail::do_read(millis());
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
// While live (a screen showing this sensor is open), poll fast
// instead of at the idle background cadence. The consumer renews the TTL.
inline void request_live(uint32_t ttl_ms = 1500) {
  _detail::live_until_ref() = millis() + ttl_ms;
}
// Whether a live-poll window is currently active (from any demander -
// the web popover or a device live screen both renew the same window).
inline bool live() { return millis() < _detail::live_until_ref(); }
inline uint32_t live_remaining_ms() {
  const uint32_t u = _detail::live_until_ref(), n = millis();
  return u > n ? u - n : 0;
}
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }
// Pressure-trend feature: enable + sample interval. The interval sets
// both the background sample cadence and the trend resolution, so the
// graph spans interval x PRESS_HIST_N. Off => no background sampling.
inline bool      trend_enabled()     { return _detail::trend_enabled_ref(); }
inline uint32_t  trend_interval_ms() { return _detail::trend_interval_ms_ref(); }
inline void      set_trend(bool on, uint32_t interval_ms) {
  _detail::trend_enabled_ref() = on;
  if (interval_ms > 0) _detail::trend_interval_ms_ref() = interval_ms;
}

} // namespace BME280
} // namespace Sensors
