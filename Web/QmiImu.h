// QMI8658 6-axis IMU driver (accelerometer + gyroscope).
//
// On the T-Beam Supreme the QMI8658 hangs off the HSPI bus shared
// with the microSD slot (CS=34 for IMU, CS=47 for SD). We borrow the
// SPIClass instance from SDCard::ensure_shared_bus() so both devices
// drive the same bus through different chip-selects. Concurrent
// transactions are not expected — IMU pump runs from the main loop
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
//     for the popover — the magnetometer already gives heading.
//   * Interrupt-driven reads. The poll-on-interval pattern is plenty
//     for the system-popover use case.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <SPI.h>
#include <SensorQMI8658.hpp>
#include "../Boards.h"
#include "SDCard.h"   // for ensure_shared_bus

namespace Web {
namespace QmiImu {

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

namespace _detail {
  inline SensorQMI8658& sensor()     { static SensorQMI8658 s; return s; }
  inline bool&     present_ref()     { static bool v = false; return v; }
  inline Reading&  last_ref()        { static Reading r; return r; }
  inline uint32_t& interval_ms_ref() { static uint32_t v = 60000; return v; }
  inline bool&     enabled_ref()     { static bool v = true; return v; }
}

inline bool begin() {
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  // Share the SPI bus the SDCard module set up. begin() form here is
  // (SPIClass&, cs, mosi, miso, sck) — the trailing pin args are
  // ignored when the bus is already begin()'d, but SensorLib still
  // expects them for cs-pinMode init.
  SPIClass* bus = Web::SDCard::ensure_shared_bus();
  if (!bus) {
    NOTICE("QMI8658: shared SPI bus unavailable — not on a Supreme board");
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

inline void pump() {
  if (!_detail::present_ref()) return;
  if (!_detail::enabled_ref()) return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  if (last.taken_ms != 0 && (now - last.taken_ms) < _detail::interval_ms_ref()) return;

  IMUdata acc{};
  IMUdata gyr{};
  // getDataReady() is cheap; if neither domain has fresh data yet,
  // skip — we'll catch it on the next poll.
  if (_detail::sensor().getDataReady()) {
    _detail::sensor().getAccelerometer(acc.x, acc.y, acc.z);
    _detail::sensor().getGyroscope(gyr.x, gyr.y, gyr.z);
  } else {
    return;
  }
  Reading r;
  r.taken_ms    = now;
  r.accel_x_g   = acc.x;
  r.accel_y_g   = acc.y;
  r.accel_z_g   = acc.z;
  r.gyro_x_dps  = gyr.x;
  r.gyro_y_dps  = gyr.y;
  r.gyro_z_dps  = gyr.z;
  r.temp_c      = _detail::sensor().getTemperature_C();
  r.valid       = true;
  _detail::last_ref() = r;
}

inline const char* model_name() { return "QMI8658"; }
inline bool      present()       { return _detail::present_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
inline uint32_t  interval_ms()   { return _detail::interval_ms_ref(); }
inline void      set_interval_ms(uint32_t ms) { _detail::interval_ms_ref() = ms; }
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

}  // namespace QmiImu
}  // namespace Web
