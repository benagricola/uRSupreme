// Battery telemetry — voltage / percent / charge state plus a derived
// dV/dt slope over a sliding window.
//
// The slope (mV/min while discharging) is the load-bearing metric for
// power-optimisation A/B testing: it's a proxy for current draw on
// hardware that doesn't expose direct battery-current measurement.
//
// AXP2101 (T-Beam Supreme) does not expose a getBattDischargeCurrent()
// accessor — that method is AXP192-only. So on Supreme builds we
// infer relative current from voltage slope. AXP192 boards get the
// true current alongside (via the Sensors::AXP2101 wrapper which
// gracefully routes AXP192 discharge_ma() too).

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

#include "../Sensors/Battery/AXP2101.h"

namespace Telemetry {
namespace Battery {

inline constexpr uint32_t SAMPLE_PERIOD_MS  = 10000;      // 10 s
inline constexpr size_t   RING_CAP          = 30;         // → 5 min window
inline constexpr uint32_t MIN_SLOPE_SPAN_MS = 60 * 1000;  // need ≥ 1 min before reporting slope

enum class State : uint8_t {
  Unknown,
  Absent,
  Charging,
  Discharging,
  Charged,
};

struct Snapshot {
  bool   pmu_present       = false;
  float  voltage_v         = 0.0f;
  int    percent           = -1;          // -1 = unknown / no battery
  State  state             = State::Unknown;
  bool   vbus_present      = false;
  float  vbus_voltage_v    = 0.0f;
  bool   has_pmu_temp      = false;
  float  pmu_temp_c        = 0.0f;
  bool   has_discharge_ma  = false;       // AXP192 only
  float  discharge_ma      = 0.0f;
  bool   has_slope         = false;
  float  slope_mv_per_min  = 0.0f;        // negative while discharging
  uint32_t slope_window_ms = 0;
};

inline const char* state_name(State s) {
  switch (s) {
    case State::Charging:    return "charging";
    case State::Discharging: return "discharging";
    case State::Charged:     return "charged";
    case State::Absent:      return "absent";
    default:                 return "unknown";
  }
}

namespace _detail {
  struct Sample { uint32_t t_ms; float v; };
  inline Sample (&ring())[RING_CAP] { static Sample r[RING_CAP] = {}; return r; }
  inline size_t&   ring_count()     { static size_t   n = 0; return n; }
  inline size_t&   ring_head()      { static size_t   h = 0; return h; }
  inline uint32_t& last_sample_ms() { static uint32_t t = 0; return t; }

  inline void push_sample(uint32_t t_ms, float v) {
    auto& r = ring();
    r[ring_head()] = { t_ms, v };
    ring_head() = (ring_head() + 1) % RING_CAP;
    if (ring_count() < RING_CAP) ++ring_count();
  }

  // Least-squares slope dV/dt (mV per minute) over the ring. Returns
  // false if there are fewer than 3 samples or the span is < 1 min.
  inline bool slope_mv_per_min(float* out_slope, uint32_t* out_window_ms) {
    if (ring_count() < 3) return false;
    auto& r = ring();
    // Chronological order: oldest is at (head - count + RING_CAP) % RING_CAP.
    const size_t start = (ring_head() + RING_CAP - ring_count()) % RING_CAP;
    const uint32_t t0 = r[start].t_ms;
    double sx = 0, sy = 0, sxy = 0, sxx = 0;
    uint32_t t_last = t0;
    for (size_t i = 0; i < ring_count(); ++i) {
      const auto& s = r[(start + i) % RING_CAP];
      const double x = (double)(s.t_ms - t0) / 1000.0;   // seconds since first sample
      const double y = (double)s.v;
      sx  += x;   sy  += y;
      sxy += x*y; sxx += x*x;
      t_last = s.t_ms;
    }
    const uint32_t span = t_last - t0;
    if (span < MIN_SLOPE_SPAN_MS) return false;
    const double n = (double)ring_count();
    const double denom = n * sxx - sx * sx;
    if (denom <= 0.0) return false;
    const double slope_v_per_sec = (n * sxy - sx * sy) / denom;
    *out_slope     = (float)(slope_v_per_sec * 1000.0 * 60.0);
    *out_window_ms = span;
    return true;
  }
}

inline void tick() {
  if (!Sensors::AXP2101::present()) return;
  const uint32_t now = millis();
  if (now - _detail::last_sample_ms() < SAMPLE_PERIOD_MS
      && _detail::last_sample_ms() != 0) {
    return;
  }
  _detail::last_sample_ms() = now;
  const uint16_t mv = Sensors::AXP2101::voltage_mv();
  if (mv == 0) return;                 // no battery / read fail
  _detail::push_sample(now, (float)mv / 1000.0f);
}

inline Snapshot current() {
  Snapshot s;
  if (!Sensors::AXP2101::present()) return s;
  s.pmu_present      = true;
  s.voltage_v        = (float)Sensors::AXP2101::voltage_mv() / 1000.0f;
  s.vbus_present     = Sensors::AXP2101::vbus_in();
  s.vbus_voltage_v   = (float)Sensors::AXP2101::vbus_voltage_mv() / 1000.0f;
  const bool has_batt = Sensors::AXP2101::battery_connected();
  if (!has_batt) {
    s.state = State::Absent;
  } else if (Sensors::AXP2101::is_charging()) {
    s.state = State::Charging;
  } else if (Sensors::AXP2101::is_discharging()) {
    s.state = State::Discharging;
  } else {
    s.state = State::Charged;
  }
  // AXP2101's built-in percent reading is unreliable on shipped FW
  // (returns 0 on many units). Compute from voltage instead, same as
  // Power.h's measure_battery — caller can override with a calibrated
  // table later if needed.
  if (has_batt && s.voltage_v > 0.0f) {
    constexpr float V_MIN = 3.30f;
    constexpr float V_MAX = 4.20f;
    int pct = (int)(((s.voltage_v - V_MIN) / (V_MAX - V_MIN)) * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    s.percent = pct;
  }
  // PMU die temperature (AXP2101 only). The 18650 cell on these boards
  // has no thermistor wired to the PMU, so this is the AXP2101's *own*
  // die temp — useful for spotting heat during charge or hot-load.
  bool t_ok = false;
  const float t = Sensors::AXP2101::temperature_c(&t_ok);
  if (t_ok) {
    s.has_pmu_temp = true;
    s.pmu_temp_c   = t;
  }
  // AXP192-only: actual battery discharge current.
  bool d_ok = false;
  const float ma = Sensors::AXP2101::discharge_ma(&d_ok);
  if (d_ok) {
    s.has_discharge_ma = true;
    s.discharge_ma     = ma;
  }
  s.has_slope = _detail::slope_mv_per_min(&s.slope_mv_per_min,
                                          &s.slope_window_ms);
  return s;
}

}  // namespace Battery
}  // namespace Telemetry
