// Sideband-format telemetry packer.
//
// Encodes the device's cached sensor readings as a packed Telemeter
// blob - the exact msgpack structure Sideband builds in
// Telemeter.packed() (Sideband sbapp/sideband/sense.py:153-162 @
// 863b925): a map of {sensor SID: packed value}, with SID_TIME always
// present as a plain integer epoch. The blob is carried in LXMF
// FIELD_TELEMETRY (LXMF/LXMF.py:9) and rendered by Sideband as
// battery / location / environment telemetry.
//
// Sensor IDs and per-sensor pack formats mirror sense.py exactly;
// each packer below cites its upstream counterpart. Only sensors the
// Supreme actually has are implemented:
//   SID_TIME        0x01  int epoch            (sense.py:330)
//   SID_LOCATION    0x02  7-element array      (sense.py:880)
//   SID_PRESSURE    0x03  float mbar           (sense.py:681)
//   SID_BATTERY     0x04  [percent, charging, temperature] (sense.py:571)
//   SID_TEMPERATURE 0x07  float celsius        (sense.py:1127)
//   SID_HUMIDITY    0x08  float percent        (sense.py:1197)
//
// All reads come from the drivers' cached snapshots (no bus traffic
// beyond the AXP2101 register reads Battery::current() already does
// for the web UI), so pack() is safe to call from the main loop.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "../Common/MsgPack.h"
#include "Battery.h"
#include "../Sensors/Position/L76K.h"
#include "../Sensors/Environment/BME280.h"

namespace Telemetry {
namespace Telemeter {

// Sensor IDs - sense.py:199-225.
inline constexpr uint8_t SID_TIME        = 0x01;
inline constexpr uint8_t SID_LOCATION    = 0x02;
inline constexpr uint8_t SID_PRESSURE    = 0x03;
inline constexpr uint8_t SID_BATTERY     = 0x04;
inline constexpr uint8_t SID_TEMPERATURE = 0x07;
inline constexpr uint8_t SID_HUMIDITY    = 0x08;

// Which sensor groups to include. A group is only emitted when its
// reading is actually valid, so these are upper bounds, not promises.
struct Include {
  bool battery     = true;
  bool location    = true;
  bool environment = true;   // BME280: pressure + temperature + humidity
};

// Worst-case packed size: map header + time(6) + battery(14) +
// location(42) + three float sensors(10 each). Callers can size
// buffers with this.
inline constexpr size_t MAX_PACKED = 96;

namespace _detail {

// Location array elements are struct.pack()-style big-endian ints
// wrapped in msgpack bin - sense.py:886-893.
inline size_t pack_be_bin(uint8_t* buf, size_t cap, uint32_t v, size_t width) {
  if (cap < 2 + width) return 0;
  buf[0] = 0xC4;                 // bin8
  buf[1] = (uint8_t)width;
  for (size_t i = 0; i < width; i++) {
    buf[2 + i] = (uint8_t)((v >> (8 * (width - 1 - i))) & 0xFF);
  }
  return 2 + width;
}

}  // namespace _detail

// Pack the telemeter map into `buf`. `now_epoch` is the wall-clock
// timestamp for SID_TIME (callers pass the same epoch source the LXMF
// message timestamp uses). Returns bytes written, 0 on overflow.
inline size_t pack(uint8_t* buf, size_t cap, const Include& inc, double now_epoch) {
  using namespace Common::MsgPack;

  const Telemetry::Battery::Snapshot batt = Telemetry::Battery::current();
  const bool have_battery = inc.battery && batt.pmu_present
                            && batt.state != Telemetry::Battery::State::Absent
                            && batt.percent >= 0;

  const Sensors::L76K::Fix fix = Sensors::L76K::last_fix();
  const bool have_location = inc.location && fix.valid;

  const Sensors::BME280::Reading env = Sensors::BME280::last_reading();
  const bool have_env = inc.environment && Sensors::BME280::present() && env.valid;

  size_t entries = 1;  // SID_TIME is unconditional (sense.py:155)
  if (have_battery)  entries += 1;
  if (have_location) entries += 1;
  if (have_env)      entries += 3;

  size_t pos = 0;
  size_t n = pack_map_header(buf, cap, entries);
  if (n == 0) return 0;
  pos += n;

  // SID_TIME: plain int epoch (sense.py:155, Time.pack sense.py:330-335).
  n = pack_uint8(&buf[pos], cap - pos, SID_TIME);
  if (n == 0) return 0; pos += n;
  n = pack_int(&buf[pos], cap - pos, (int64_t)now_epoch);
  if (n == 0) return 0; pos += n;

  if (have_battery) {
    // Battery.pack (sense.py:571-576): [charge_percent, charging,
    // temperature]. Percent is voltage-derived (integer); charging
    // mirrors plyer's isCharging semantics (true only while actively
    // charging, not when full on external power). Temperature is nil:
    // the cell has no thermistor and the AXP2101 die temp is not
    // battery temperature.
    n = pack_uint8(&buf[pos], cap - pos, SID_BATTERY);
    if (n == 0) return 0; pos += n;
    n = pack_array_header(&buf[pos], cap - pos, 3);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)batt.percent);
    if (n == 0) return 0; pos += n;
    n = pack_bool(&buf[pos], cap - pos,
                  batt.state == Telemetry::Battery::State::Charging);
    if (n == 0) return 0; pos += n;
    if (pos >= cap) return 0;
    buf[pos++] = 0xC0;  // temperature: nil
  }

  if (have_location) {
    // Location.pack (sense.py:880-897): [lat, lon, altitude, speed,
    // bearing, accuracy, last_update]. The first six are big-endian
    // struct-packed ints in msgpack bin: !i lat*1e6, !i lon*1e6,
    // !i altitude*1e2, !I speed*1e2, !i bearing*1e2, !H accuracy*1e2.
    // Speed is km/h on the wire (sense.py:856 converts from m/s);
    // the L76K reports knots. Accuracy is metres; the NMEA stream
    // carries no direct accuracy estimate, so approximate from HDOP
    // (HDOP x 5 m nominal user-range error), 10 m when HDOP is absent.
    const double speed_kmh = fix.speed_knots * 1.852;
    const double alt_m     = fix.altitude_valid ? fix.altitude_m : 0.0;
    double acc_m           = fix.hdop_valid ? (double)fix.hdop * 5.0 : 10.0;
    if (acc_m < 0.0)    acc_m = 0.0;
    if (acc_m > 655.35) acc_m = 655.35;   // !H ceiling
    const int64_t last_update =
        fix.unix_epoch > 0.0 ? (int64_t)fix.unix_epoch : (int64_t)now_epoch;

    n = pack_uint8(&buf[pos], cap - pos, SID_LOCATION);
    if (n == 0) return 0; pos += n;
    n = pack_array_header(&buf[pos], cap - pos, 7);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)(int32_t)lround(fix.latitude_deg * 1e6), 4);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)(int32_t)lround(fix.longitude_deg * 1e6), 4);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)(int32_t)lround(alt_m * 100.0), 4);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)lround(speed_kmh * 100.0), 4);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)(int32_t)lround(fix.heading_deg * 100.0), 4);
    if (n == 0) return 0; pos += n;
    n = _detail::pack_be_bin(&buf[pos], cap - pos,
                             (uint32_t)lround(acc_m * 100.0), 2);
    if (n == 0) return 0; pos += n;
    n = pack_int(&buf[pos], cap - pos, last_update);
    if (n == 0) return 0; pos += n;
  }

  if (have_env) {
    // Pressure.pack (sense.py:681-686): bare float, millibar. The
    // BME280 driver caches pascals.
    n = pack_uint8(&buf[pos], cap - pos, SID_PRESSURE);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)env.pressure_pa / 100.0);
    if (n == 0) return 0; pos += n;
    // Temperature.pack (sense.py:1127): bare float, celsius.
    n = pack_uint8(&buf[pos], cap - pos, SID_TEMPERATURE);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)env.temp_c);
    if (n == 0) return 0; pos += n;
    // Humidity.pack (sense.py:1197): bare float, relative percent.
    n = pack_uint8(&buf[pos], cap - pos, SID_HUMIDITY);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)env.humidity_pct);
    if (n == 0) return 0; pos += n;
  }

  return pos;
}

}  // namespace Telemeter
}  // namespace Telemetry
