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
//   * raw accelerometer (g) and gyroscope (deg/s) per axis
//   * chip temperature
//   * motion-wake: the hardware any-motion engine, routed to the IMU INT
//     pin (GPIO33), dropping the GNSS acquisition backoff on movement
// What we don't expose yet:
//   * Madgwick / Mahony fusion for absolute orientation. Not needed
//     for the popover - the magnetometer already gives heading.
//   * Interrupt-driven data reads. Accel/gyro data is polled while a
//     screen demands it; only motion-wake takes the INT.

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

// Motion-wake feature. The IMU's only standing consumer is motion-wake:
// a movement event drops the GNSS acquisition backoff so a device that
// was moved re-acquires sooner. This is an event, not a time series, so
// there is no interval - it is interrupt-driven. The QMI8658's hardware
// any-motion engine drives the IMU INT line (GPIO33); the ISR sets a flag
// and pump() then reads the status register once to confirm and clear the
// event. The line is a "jump transition", not a stable level (so it is
// wired CHANGE and read via the status latch, per LilyGo's
// QMI8658_MotionDetectionExample). The magnetometer's tilt/gyro fusion
// gets its fast samples from the live demand the heading screen/popover
// renews; outside both, the IMU does not poll.
//
// Per-axis any-motion slope threshold (mg). High-pass filtered, so
// gravity is removed and this is the change that counts as movement.
inline constexpr float    MOTION_ANY_THR_MG = 100.0f;
// Don't fire reset_backoff() more than once per cooldown (a walk emits
// many events); keep the signal sparse so it doesn't flap the GPS retry
// cadence.
inline constexpr uint32_t MOTION_REPORT_COOLDOWN_MS = 30UL * 1000UL;

namespace _detail {
  inline SensorQMI8658& sensor()     { static SensorQMI8658 s; return s; }
  inline bool&     present_ref()     { static bool v = false; return v; }
  inline Reading&  last_ref()        { static Reading r; return r; }
  inline uint32_t& live_until_ref()  { static uint32_t v = 0; return v; }
  inline bool&     enabled_ref()     { static bool v = true; return v; }
  // millis() when we last fired a motion notification. 0 = never.
  inline uint32_t& last_motion_ms_ref() { static uint32_t v = 0; return v; }
  // Motion-wake feature: enable flag, and whether the any-motion engine +
  // INT configured at begin() (servicing no-ops if not).
  inline bool&     motion_wake_enabled_ref()     { static bool v = true; return v; }
  inline bool&     motion_ready_ref()            { static bool v = false; return v; }
  // Set by the IMU INT ISR on an any-motion edge; cleared in pump() after
  // the status read. volatile - written in ISR context, read on the loop.
  inline volatile bool& motion_irq_flag_ref()    { static volatile bool v = false; return v; }
  // ISR: flag only (no SPI here); pump() does the bus-guarded status read.
  inline void IRAM_ATTR on_motion_irq() { motion_irq_flag_ref() = true; }
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
  // Hardware any-motion engine for motion-wake. configMotion toggles the
  // accel/gyro off and back on while it writes the engine registers, so
  // it runs after they are enabled. The event is routed to the IMU's INT1
  // pin (wired to GPIO33 / IMU_INT); we attach a CHANGE interrupt - the
  // any-motion line is a "jump transition", not a stable level - and the
  // ISR just flags it for pump(). Any-motion only (no-motion / significant
  // unused); slope thresholds are MOTION_ANY_THR_MG over a 1-sample
  // window. Mirrors SensorLib's QMI8658_MotionDetectionExample.
  const uint8_t motion_ctrl = SensorQMI8658::ANY_MOTION_EN_X
                            | SensorQMI8658::ANY_MOTION_EN_Y
                            | SensorQMI8658::ANY_MOTION_EN_Z;
  _detail::sensor().configMotion(
      motion_ctrl,
      MOTION_ANY_THR_MG, MOTION_ANY_THR_MG, MOTION_ANY_THR_MG, /*AnyMotionWindow=*/1,
      /*NoMotion x/y/z thr=*/0.0f, 0.0f, 0.0f, /*NoMotionWindow=*/1,
      /*SigMotionWaitWindow=*/1, /*SigMotionConfirmWindow=*/1);
  _detail::sensor().enableMotionDetect(SensorQMI8658::INTERRUPT_PIN_1);
  pinMode(IMU_INT, INPUT);
  attachInterrupt(digitalPinToInterrupt(IMU_INT), _detail::on_motion_irq, CHANGE);
  _detail::motion_ready_ref() = true;
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

  // (a) Motion-wake: the IMU INT ISR flags an any-motion edge. Service it
  // here (off the ISR): read the status register once to confirm + clear
  // the latch, then drop the GNSS backoff (cooldown-limited) so a moved
  // device re-acquires sooner. The status read clears the event whether or
  // not the feature acts, so a stuck line cannot wedge the ISR.
  if (_detail::motion_ready_ref() && _detail::motion_irq_flag_ref()) {
    _detail::motion_irq_flag_ref() = false;
    Storage::SDCard::BusGuard _bg{Storage::SDCard::TryLock{}};
    if (_bg.ok()) {
      const uint8_t st = (uint8_t)_detail::sensor().getStatusRegister();
      if (_detail::motion_wake_enabled_ref() && (st & SensorQMI8658::EVENT_ANY_MOTION)) {
        auto& cooldown = _detail::last_motion_ms_ref();
        const bool cooled = (cooldown == 0) || (now - cooldown >= MOTION_REPORT_COOLDOWN_MS);
        if (cooled) {
          cooldown = now;
          // Motion implies a possibly new sky view; retry GPS sooner
          // rather than sit out the (up to 30-min) backoff window.
          Sensors::Gnss::reset_backoff();
        }
      }
    } else {
      // Bus busy: re-flag so we read + clear the latch on a later pass.
      _detail::motion_irq_flag_ref() = true;
    }
  }

  // (b) Accel/gyro/temp data read for the magnetometer's tilt/gyro fusion
  // and the Motion popover. Needed only while a live demand is active (the
  // compass renews it); the IMU has no packed telemetry and no background
  // data consumer, so it is otherwise idle.
  if (now >= _detail::live_until_ref()) return;
  const auto& last = _detail::last_ref();
  if (last.taken_ms != 0 && (now - last.taken_ms) < LIVE_POLL_MS) return;

  IMUdata acc{};
  IMUdata gyr{};
  float temp_c = 0.0f;
  {
    // The IMU shares the HSPI bus with the SD card. Hold the bus mutex
    // across the SPI reads so they can't interleave with an SD write
    // (upload on the web task, or the off-loop receive writer), which
    // would corrupt the transfer. Non-blocking TryLock: the off-loop
    // writer can hold the bus in tight bursts, and blocking here would
    // stall the loop and starve LoRa; this poll is best-effort, so if the
    // bus is busy we just skip this sample.
    Storage::SDCard::BusGuard _bg{Storage::SDCard::TryLock{}};
    if (!_bg.ok()) return;
    // getDataReady() is cheap; if there's no fresh data yet, skip - we'll
    // catch it on the next poll.
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
  _detail::last_ref() = r;
}

inline const char* model_name() { return "QMI8658"; }
inline bool      present()       { return _detail::present_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
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
// Motion-wake feature: enable flag only (it is interrupt-driven, not a
// timed poll, so there is no interval). Off => an any-motion event is
// still read and cleared but does not reset the GNSS backoff.
inline bool      motion_wake_enabled()     { return _detail::motion_wake_enabled_ref(); }
inline void      set_motion_wake(bool on)  { _detail::motion_wake_enabled_ref() = on; }

} // namespace QMI8658
} // namespace Sensors
