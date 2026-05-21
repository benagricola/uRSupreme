// QMC6310 3-axis magnetometer driver.
//
// Lives on the T-Beam Supreme's user/sensor I2C bus (Wire, SDA=17,
// SCL=18) at 0x1C (QMC6310U variant) or 0x3C (QMC6310N). Shares the
// bus with BME280 (#120 stage 2) and the OLED. Each device has its
// own address so coexistence is automatic.
//
// We use Lewis He's SensorLib wrapper (lewisxhe/SensorLib) — the
// same library LilyGo's factory firmware uses, so the calibration
// + configuration paths are well-trodden.
//
// API mirrors Bme280/Gps: begin() probes both addresses, pump()
// reads at most once per interval_ms (default 60 s), last_reading()
// returns the cached snapshot. polar (heading) is computed in the
// library from the raw XYZ vector.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMC6310.hpp>

namespace Sensors {
namespace QMC6310 {

struct Reading {
  bool      valid       = false;
  uint32_t  taken_ms    = 0;
  float     x_uT        = 0.0f;
  float     y_uT        = 0.0f;
  float     z_uT        = 0.0f;
  float     heading_deg = 0.0f;   // 0 = magnetic north, 0..360
};

namespace _detail {
  inline SensorQMC6310& sensor()     { static SensorQMC6310 s; return s; }
  inline bool&     present_ref()     { static bool v = false; return v; }
  inline Reading&  last_ref()        { static Reading r; return r; }
  inline uint32_t& interval_ms_ref() { static uint32_t v = 60000; return v; }
  inline uint8_t&  addr_ref()        { static uint8_t v = 0; return v; }
  inline bool&     enabled_ref()     { static bool v = true; return v; }
}

inline bool begin(TwoWire& wire) {
  // SensorLib's QMC6310 only knows one address constant (0x1C); the
  // QMC6310N is at 0x3C. Probe both via init(). The sda/scl args are
  // -1 so SensorLib's wire.begin(sda,scl) call reuses the existing
  // bus we set up in the .ino. (ESP32 Wire.begin is idempotent.)
  const uint8_t addrs[] = { 0x1C, 0x3C };
  for (uint8_t a : addrs) {
    if (_detail::sensor().init(wire, -1, -1, a)) {
      _detail::present_ref() = true;
      _detail::addr_ref()    = a;
      _detail::sensor().configMagnetometer(
          SensorQMC6310::MODE_CONTINUOUS,
          SensorQMC6310::RANGE_8G,
          SensorQMC6310::DATARATE_10HZ,
          SensorQMC6310::OSR_8,
          SensorQMC6310::DSR_1);
      // Declination defaults to 0 — fine for relative readings; the
      // user can override per-location in a follow-up if needed.
      _detail::sensor().setDeclination(0.0f);
      NOTICEF("QMC6310: found at 0x%02x", a);
      return true;
    }
  }
  NOTICE("QMC6310: not detected on Wire (probed 0x1C + 0x3C)");
  _detail::present_ref() = false;
  return false;
}

inline void pump() {
  if (!_detail::present_ref()) return;
  if (!_detail::enabled_ref()) return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  // interval_ms == 0 → "boot-read only" (single read, then idle until
  // reboot). See Bme280.h pump() for the same handling.
  if (_detail::interval_ms_ref() == 0 && last.taken_ms != 0) return;
  if (last.taken_ms != 0 && (now - last.taken_ms) < _detail::interval_ms_ref()) return;

  Polar p;
  if (!_detail::sensor().readPolar(p)) return;   // data not ready yet
  Reading r;
  r.taken_ms    = now;
  r.heading_deg = p.polar;
  r.x_uT        = _detail::sensor().getX();
  r.y_uT        = _detail::sensor().getY();
  r.z_uT        = _detail::sensor().getZ();
  r.valid       = !isnan(r.x_uT) && !isnan(r.y_uT) && !isnan(r.z_uT);
  _detail::last_ref() = r;
}

inline const char* model_name() { return "QMC6310"; }
inline bool      present()       { return _detail::present_ref(); }
inline uint8_t   address()       { return _detail::addr_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
inline uint32_t  interval_ms()   { return _detail::interval_ms_ref(); }
inline void      set_interval_ms(uint32_t ms) { _detail::interval_ms_ref() = ms; }
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

} // namespace QMC6310
} // namespace Sensors
