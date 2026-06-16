// QMI8658 6-axis IMU driver (accelerometer + gyroscope).
//
// On the T-Beam Supreme the QMI8658 hangs off the HSPI bus shared
// with the microSD slot (CS=34 for IMU, CS=47 for SD). We borrow the
// SPIClass instance from SDCard::ensure_shared_bus() so both devices
// drive the same bus through different chip-selects. Concurrent
// transactions are not expected - IMU pump runs from the main loop
// once per interval, SD writes happen synchronously from LXMF persist
// callbacks; both serialise on the FreeRTOS scheduler.
//
// We use Lewis He's SensorLib QMI8658 driver (lewisxhe/SensorLib).
//
// What we expose:
//   * raw accelerometer (m/s²) and gyroscope (°/s) per axis
//   * chip temperature
// What we don't expose yet:
//   * Madgwick / Mahony fusion for absolute orientation. Not needed
//     for the popover - the magnetometer already gives heading.
//   * Interrupt-driven reads. The poll-on-interval pattern is plenty
//     for the system-popover use case.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <SPI.h>
#include <SensorQMI8658.hpp>
#include "../../Boards.h"
#include "../../Storage/SDCard.h"  // for ensure_shared_bus
#include "../Position/Gnss.h"      // for Gnss::reset_backoff on motion events

namespace Sensors {
namespace QMI8658 {

struct Reading {
  bool      valid       = false;
  uint32_t  taken_ms    = 0;
  float     accel_x_g   = 0.0f;
  float     accel_y_g   = 0.0f;
  float     accel_z_g   = 0.0f;
  float     gyro_x_dps  = 0.0f;
  float     gyro_y_dps  = 0.0f;
  float     gyro_z_dps  = 0.0f;
  float     temp_c      = 0.0f;
};

// Motion-detection thresholds. The IMU runs at a long-ish poll
// interval (default 60 s), so any "motion" we surface here is the
// difference between two snapshots one interval apart. We compare
// gravity-removed accel magnitude - gyro is noisier and the GPS-reset
// use case cares about translation, not rotation. The hysteresis is
// deliberately loose: the goal is to spot "the device was picked up
// and walked somewhere" not "someone breathed on it". The default
// chooses a value about 5x bench noise on the QMI8658.
inline constexpr float    MOTION_ACCEL_DELTA_G = 0.15f;
// Don't fire reset_backoff() more than once per N reads (motion will
// often persist for several reads while someone walks); keep the
// signal sparse so it doesn't flap the GPS retry cadence.
inline constexpr uint32_t MOTION_REPORT_COOLDOWN_MS = 30UL * 1000UL;

namespace _detail {
  inline SensorQMI8658& sensor()     { static SensorQMI8658 s; return s; }
  inline bool&     present_ref()     { static bool v = false; return v; }
  inline Reading&  last_ref()        { static Reading r; return r; }
  inline uint32_t& interval_ms_ref() { static uint32_t v = 60000; return v; }
  inline uint32_t& live_until_ref()  { static uint32_t v = 0; return v; }
  inline bool&     enabled_ref()     { static bool v = true; return v; }
  // millis() when we last fired a motion notification. 0 = never.
  inline uint32_t& last_motion_ms_ref() { static uint32_t v = 0; return v; }
}

inline bool begin() {
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  // Share the SPI bus the SDCard module set up. begin() form here is
  // (SPIClass&, cs, mosi, miso, sck) - the trailing pin args are
  // ignored when the bus is already begin()'d, but SensorLib still
  // expects them for cs-pinMode init.
  SPIClass* bus = Storage::SDCard::ensure_shared_bus();
  if (!bus) {
    NOTICE("QMI8658: shared SPI bus unavailable - not on a Supreme board");
    return false;
  }
  // SensorLib's SPI begin signature: (cs, mosi, miso, sck, spi_ref).
  // The lib calls spi.begin() internally but ours is already up via
  // SDCard's ensure_shared_bus(); calling begin() again is a no-op
  // on ESP32.
  if (!_detail::sensor().begin(IMU_CS, SD_MOSI, SD_MISO, SD_CLK, *bus)) {
    NOTICEF("QMI8658: not detected (cs=%d on HSPI)", IMU_CS);
    _detail::present_ref() = false;
    return false;
  }
  _detail::present_ref() = true;
  NOTICEF("QMI8658: chip id 0x%02x", _detail::sensor().getChipID());

  // ±4g / ±256 dps at high ODR. Plenty of headroom for "is the device
  // being held or moved" telemetry. configAccelerometer/Gyroscope's
  // 3rd arg is the LPF mode; older example code had a stale 4th
  // bool that no longer exists in this SensorLib version.
  _detail::sensor().configAccelerometer(
      SensorQMI8658::ACC_RANGE_4G,
      SensorQMI8658::ACC_ODR_1000Hz,
      SensorQMI8658::LPF_MODE_0);
  _detail::sensor().configGyroscope(
      SensorQMI8658::GYR_RANGE_256DPS,
      SensorQMI8658::GYR_ODR_896_8Hz,
      SensorQMI8658::LPF_MODE_3);
  _detail::sensor().enableAccelerometer();
  _detail::sensor().enableGyroscope();
  return true;
#else
  return false;
#endif
}

// Fast poll period while a live demand is active. 10 Hz keeps the gyro
// fresh for the compass complementary filter.
inline constexpr uint32_t LIVE_POLL_MS = 100;

inline void pump() {
  if (!_detail::present_ref()) return;
  if (!_detail::enabled_ref()) return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  const bool live = now < _detail::live_until_ref();
  const uint32_t eff_interval = live ? LIVE_POLL_MS : _detail::interval_ms_ref();
  // interval_ms == 0 → "boot-read only" (single read, then idle until
  // reboot). See Bme280.h pump() for the same handling.
  if (!live && _detail::interval_ms_ref() == 0 && last.taken_ms != 0) return;
  if (last.taken_ms != 0 && (now - last.taken_ms) < eff_interval) return;

  IMUdata acc{};
  IMUdata gyr{};
  float temp_c = 0.0f;
  {
    // The IMU shares the HSPI bus with the SD card. Hold the bus mutex
    // across this poll's SPI reads so they can't interleave with an
    // attachment-upload SD write running on the AsyncTCP web task, which
    // would corrupt the SD transfer. See Storage::SDCard::BusGuard.
    Storage::SDCard::BusGuard _bg;
    // getDataReady() is cheap; if neither domain has fresh data yet,
    // skip - we'll catch it on the next poll.
    if (!_detail::sensor().getDataReady()) return;
    _detail::sensor().getAccelerometer(acc.x, acc.y, acc.z);
    _detail::sensor().getGyroscope(gyr.x, gyr.y, gyr.z);
    temp_c = _detail::sensor().getTemperature_C();
  }
  Reading r;
  r.taken_ms    = now;
  r.accel_x_g   = acc.x;
  r.accel_y_g   = acc.y;
  r.accel_z_g   = acc.z;
  r.gyro_x_dps  = gyr.x;
  r.gyro_y_dps  = gyr.y;
  r.gyro_z_dps  = gyr.z;
  r.temp_c      = temp_c;
  r.valid       = true;
  // Motion detection - compare gravity-removed magnitude against
  // the previous snapshot's. Skip if there's no previous (first
  // valid reading), and respect the cooldown so a long walk doesn't
  // fire dozens of reset_backoff() pings.
  if (last.valid) {
    const float prev_mag = sqrtf(last.accel_x_g * last.accel_x_g
                               + last.accel_y_g * last.accel_y_g
                               + last.accel_z_g * last.accel_z_g);
    const float curr_mag = sqrtf(r.accel_x_g * r.accel_x_g
                               + r.accel_y_g * r.accel_y_g
                               + r.accel_z_g * r.accel_z_g);
    const float delta    = fabsf(curr_mag - prev_mag);
    auto& cooldown = _detail::last_motion_ms_ref();
    const bool cooled = (cooldown == 0) || (now - cooldown >= MOTION_REPORT_COOLDOWN_MS);
    if (delta > MOTION_ACCEL_DELTA_G && cooled) {
      cooldown = now;
      // Drop the GPS exponential-backoff counter: motion implies the
      // device may have a new sky view, so retry sooner rather than
      // sit out the (potentially 30-min) backoff window.
      Sensors::Gnss::reset_backoff();
    }
  }
  _detail::last_ref() = r;
}

inline const char* model_name() { return "QMI8658"; }
inline bool      present()       { return _detail::present_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
inline uint32_t  interval_ms()   { return _detail::interval_ms_ref(); }
inline void      set_interval_ms(uint32_t ms) { _detail::interval_ms_ref() = ms; }
// While live (a screen/popover showing the IMU is open), poll fast.
inline void request_live(uint32_t ttl_ms = 1500) { _detail::live_until_ref() = millis() + ttl_ms; }
// Whether a live-poll window is currently active (web popover or a
// device live screen renew the same window).
inline bool live() { return millis() < _detail::live_until_ref(); }
inline uint32_t live_remaining_ms() {
  const uint32_t u = _detail::live_until_ref(), n = millis();
  return u > n ? u - n : 0;
}
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

} // namespace QMI8658
} // namespace Sensors
