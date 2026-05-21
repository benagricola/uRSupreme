// Hardware RTC driver for the PCF8563 (T-Beam Supreme).
//
// The PCF8563 sits on the PMU I2C bus on this hardware (Wire1,
// SDA=42 SCL=41 — NOT the user/sensor bus on 17/18), sharing the
// bus with the AXP2101 PMU at 0x34. The RTC's own address is 0x51.
// It has a coin-cell backup so its time survives ESP32 power-off
// and reboot. We use it as a cold-boot seed for Clock::Manager
// and write back to it whenever a higher-trust source (GPS, NTP,
// Browser) reports a new time. The RTC is NOT a user-configurable
// source — see TimeManager.h for the rationale.
//
// Wire format on the bus, addr 0x51:
//   reg 0x00: control_status_1 (bit 5 = STOP — must be 0 for clock to run)
//   reg 0x01: control_status_2
//   reg 0x02: VL_seconds   (bit 7 = VL: voltage-low flag; 0 = clock is valid)
//   reg 0x03: minutes      (BCD, 7 bits)
//   reg 0x04: hours        (BCD, 6 bits)
//   reg 0x05: days         (BCD, 6 bits)
//   reg 0x06: weekdays     (3 bits)
//   reg 0x07: century_months (bit 7 = century: 1 = 1900s, 0 = 2000s; BCD months 5 bits)
//   reg 0x08: years        (BCD, 8 bits)
//
// All BCD fields are encoded as 4-bit-decade upper nibble + 4-bit-unit
// lower nibble.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <time.h>
#include "../../Clock/Manager.h"

namespace Sensors {
namespace PCF8563 {

inline constexpr uint8_t I2C_ADDR = 0x51;

// Board pin defs aren't in scope here; the caller passes them in
// (init_and_seed() picks them up from Boards.h via the .ino).
struct Pins {
  int sda;
  int scl;
  uint32_t hz = 100000;
};

namespace _detail {
  inline bool& available_ref() { static bool v = false; return v; }
  inline TwoWire*& wire_ref()  { static TwoWire* v = nullptr; return v; }

  inline uint8_t bcd_to_bin(uint8_t v) {
    return ((v >> 4) & 0x0F) * 10 + (v & 0x0F);
  }
  inline uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
  }

  inline bool read_regs(uint8_t start, uint8_t* dst, size_t n) {
    TwoWire* w = wire_ref();
    if (!w) return false;
    w->beginTransmission(I2C_ADDR);
    w->write(start);
    if (w->endTransmission(false) != 0) return false;
    if (w->requestFrom((int)I2C_ADDR, (int)n) != (int)n) return false;
    for (size_t i = 0; i < n; ++i) dst[i] = w->read();
    return true;
  }

  inline bool write_regs(uint8_t start, const uint8_t* src, size_t n) {
    TwoWire* w = wire_ref();
    if (!w) return false;
    w->beginTransmission(I2C_ADDR);
    w->write(start);
    for (size_t i = 0; i < n; ++i) w->write(src[i]);
    return w->endTransmission(true) == 0;
  }
}

// Probe the bus for the PCF8563. Returns true if it ACKs.
inline bool probe() {
  TwoWire* w = _detail::wire_ref();
  if (!w) return false;
  w->beginTransmission(I2C_ADDR);
  return w->endTransmission(true) == 0;
}

// One-shot diagnostic scan of the entire 7-bit I2C address space.
// Logs every address that ACKs. Cheap (~115 transactions), only used
// at boot when probe(I2C_ADDR) fails so we can surface "what *is*
// on the bus" without flashing a separate scanner build.
inline void debug_bus_scan() {
  TwoWire* w = _detail::wire_ref();
  if (!w) return;
  NOTICE("RtcPCF8563: bus scan — probing 0x03..0x77");
  for (uint8_t a = 0x03; a < 0x78; ++a) {
    w->beginTransmission(a);
    if (w->endTransmission(true) == 0) {
      NOTICEF("RtcPCF8563:   0x%02x ACK", a);
    }
  }
  NOTICE("RtcPCF8563: bus scan done");
}

// Read the current RTC time as a Unix epoch (seconds). Returns 0 if
// the RTC reports VL (voltage-low — clock invalid) or the read
// fails. Does NOT call TimeManager.
inline double read_epoch() {
  uint8_t b[7] = {0};
  if (!_detail::read_regs(0x02, b, 7)) return 0.0;
  if (b[0] & 0x80) return 0.0;   // VL flag — chip says time is invalid
  struct tm t{};
  t.tm_sec   = _detail::bcd_to_bin(b[0] & 0x7F);
  t.tm_min   = _detail::bcd_to_bin(b[1] & 0x7F);
  t.tm_hour  = _detail::bcd_to_bin(b[2] & 0x3F);
  t.tm_mday  = _detail::bcd_to_bin(b[3] & 0x3F);
  const int century = (b[5] & 0x80) ? 1900 : 2000;
  t.tm_mon   = _detail::bcd_to_bin(b[5] & 0x1F) - 1;
  t.tm_year  = (int)(century - 1900) + (int)_detail::bcd_to_bin(b[6]);
  t.tm_isdst = 0;
  return (double)mktime(&t);
}

// Write a Unix-epoch time to the RTC. Caller is responsible for only
// invoking this with values they trust (GPS / NTP / Browser).
inline bool write_epoch(double epoch_seconds) {
  if (!_detail::available_ref()) return false;
  if (epoch_seconds < 1577836800.0 || epoch_seconds > 4102444800.0) return false;
  const time_t e = (time_t)epoch_seconds;
  struct tm t{};
  gmtime_r(&e, &t);
  uint8_t b[7];
  b[0] = _detail::bin_to_bcd((uint8_t)t.tm_sec)  & 0x7F;   // clear VL
  b[1] = _detail::bin_to_bcd((uint8_t)t.tm_min)  & 0x7F;
  b[2] = _detail::bin_to_bcd((uint8_t)t.tm_hour) & 0x3F;
  b[3] = _detail::bin_to_bcd((uint8_t)t.tm_mday) & 0x3F;
  b[4] = (uint8_t)(t.tm_wday & 0x07);
  const int year_full = t.tm_year + 1900;
  const uint8_t century_bit = (year_full < 2000) ? 0x80 : 0x00;
  b[5] = (uint8_t)(_detail::bin_to_bcd((uint8_t)(t.tm_mon + 1)) & 0x1F) | century_bit;
  b[6] = _detail::bin_to_bcd((uint8_t)(year_full % 100));
  return _detail::write_regs(0x02, b, sizeof(b));
}

// Initialise the bus, probe for the PCF8563, read the current time
// and seed the TimeManager. Idempotent. Returns true if a sensible
// epoch was read and applied.
inline bool init_and_seed(TwoWire& wire, const Pins& pins) {
  _detail::wire_ref() = &wire;
  wire.begin(pins.sda, pins.scl, pins.hz);
  if (!probe()) {
    NOTICE("RtcPCF8563: no chip at 0x51 — RTC features disabled");
    debug_bus_scan();
    _detail::available_ref() = false;
    return false;
  }
  _detail::available_ref() = true;
  // Make sure the clock is running (control_status_1, bit 5 = STOP).
  uint8_t cs1;
  if (_detail::read_regs(0x00, &cs1, 1) && (cs1 & 0x20)) {
    cs1 &= (uint8_t)~0x20;
    _detail::write_regs(0x00, &cs1, 1);
    NOTICE("RtcPCF8563: cleared STOP bit — clock now running");
  }
  const double epoch = read_epoch();
  if (epoch <= 0.0) {
    NOTICE("RtcPCF8563: present but no valid time stored (VL set) — waiting for a live source to seed it");
    return false;
  }
  Clock::Manager::seed_from_rtc(epoch);
  NOTICEF("RtcPCF8563: seeded TimeManager with epoch %.0f from on-board RTC", epoch);
  return true;
}

// Is the chip present and responsive?
inline bool available() { return _detail::available_ref(); }

// Diagnostic snapshot: raw register bytes + parsed state. Used by the
// /api/rtc endpoint so we can verify the chip is alive without
// dragging serial logs around.
struct DebugSnapshot {
  bool     present;
  bool     vl_set;             // bit 7 of reg 0x02 — clock-invalid flag
  uint8_t  regs[7];            // 0x02..0x08
  double   epoch;              // 0 if VL or read failed
};
inline DebugSnapshot debug_snapshot() {
  DebugSnapshot s{};
  s.present = _detail::available_ref();
  if (!s.present) return s;
  uint8_t b[7] = {0};
  if (!_detail::read_regs(0x02, b, 7)) return s;
  for (int i = 0; i < 7; ++i) s.regs[i] = b[i];
  s.vl_set = (b[0] & 0x80) != 0;
  if (!s.vl_set) {
    struct tm t{};
    t.tm_sec   = _detail::bcd_to_bin(b[0] & 0x7F);
    t.tm_min   = _detail::bcd_to_bin(b[1] & 0x7F);
    t.tm_hour  = _detail::bcd_to_bin(b[2] & 0x3F);
    t.tm_mday  = _detail::bcd_to_bin(b[3] & 0x3F);
    const int century = (b[5] & 0x80) ? 1900 : 2000;
    t.tm_mon   = _detail::bcd_to_bin(b[5] & 0x1F) - 1;
    t.tm_year  = (int)(century - 1900) + (int)_detail::bcd_to_bin(b[6]);
    t.tm_isdst = 0;
    s.epoch = (double)mktime(&t);
  }
  return s;
}

} // namespace PCF8563
} // namespace Sensors
