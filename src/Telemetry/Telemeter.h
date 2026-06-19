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
//   SID_MAGNETIC_FIELD 0x09  [x, y, z] microtesla (sense.py:1262)
//
// pack() first reads the included I2C sensors on demand (BME280 /
// QMC6310, a few ms each) so passive packers send fresh data without a
// standing idle poll; battery is read live and location rides the GPS
// power schedule. The reads are bounded and main-loop safe.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

#include "../Common/MsgPack.h"
#include "Battery.h"
#include "../Sensors/Position/Gnss.h"
#include "../Sensors/Environment/BME280.h"
#include "../Sensors/Compass/QMC6310.h"

namespace Telemetry {
namespace Telemeter {

// Sensor IDs - sense.py:199-225.
inline constexpr uint8_t SID_TIME        = 0x01;
inline constexpr uint8_t SID_LOCATION    = 0x02;
inline constexpr uint8_t SID_PRESSURE    = 0x03;
inline constexpr uint8_t SID_BATTERY     = 0x04;
inline constexpr uint8_t SID_TEMPERATURE = 0x07;
inline constexpr uint8_t SID_HUMIDITY    = 0x08;
inline constexpr uint8_t SID_MAGNETIC_FIELD = 0x09;

// Which sensor groups to include. A group is only emitted when its
// reading is actually valid, so these are upper bounds, not promises.
struct Include {
  bool battery     = true;
  bool location    = true;
  bool environment = true;   // BME280: pressure + temperature + humidity
  bool magnetic    = false;  // QMC6310: 3-axis field, microtesla
};

// Worst-case packed size: map header + time(6) + battery(14) +
// location(42) + three float sensors(10 each) + magnetic(30).
// Callers can size buffers with this.
inline constexpr size_t MAX_PACKED = 128;

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

  // Read-on-demand (#7): refresh the included I2C sensors just before
  // packing so a passive packer (collector, live-share grant, announce)
  // sends current data without relying on a standing idle poll. Battery
  // is read live below; location rides the GPS power schedule and is not
  // force-read here. Each call is a no-op if the chip is absent/disabled.
  if (inc.environment) Sensors::BME280::read_now();
  if (inc.magnetic)    Sensors::QMC6310::read_now();

  const Telemetry::Battery::Snapshot batt = Telemetry::Battery::current();
  const bool have_battery = inc.battery && batt.pmu_present
                            && batt.state != Telemetry::Battery::State::Absent
                            && batt.percent >= 0;

  const Sensors::Gnss::Fix fix = Sensors::Gnss::last_fix();
  const bool have_location = inc.location && fix.valid;

  const Sensors::BME280::Reading env = Sensors::BME280::last_reading();
  const bool have_env = inc.environment && Sensors::BME280::present() && env.valid;

  const Sensors::QMC6310::Reading mag = Sensors::QMC6310::last_reading();
  const bool have_mag = inc.magnetic && mag.valid;

  size_t entries = 1;  // SID_TIME is unconditional (sense.py:155)
  if (have_battery)  entries += 1;
  if (have_location) entries += 1;
  if (have_env)      entries += 3;
  if (have_mag)      entries += 1;
  // A map with only SID_TIME carries no readings. Callers treat an
  // empty pack as "nothing to send" - without this, a location-only
  // pack with no GPS fix would still emit a useless timestamp blob.
  if (entries == 1) return 0;

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
    // NMEA reports knots. Accuracy is metres: the MAX-M10's real
    // estimate (UBX-NAV-PVT hAcc) when present, else the HDOP
    // approximation (HDOP x 5 m nominal user-range error), else a
    // 10 m nominal.
    const double speed_kmh = fix.speed_knots * 1.852;
    const double alt_m     = fix.altitude_valid ? fix.altitude_m : 0.0;
    double acc_m           = fix.acc_valid  ? (double)fix.hacc_m
                           : fix.hdop_valid ? (double)fix.hdop * 5.0
                           : 10.0;
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

  if (have_mag) {
    // MagneticField.pack (sense.py:1262-1266): [x, y, z] microtesla.
    n = pack_uint8(&buf[pos], cap - pos, SID_MAGNETIC_FIELD);
    if (n == 0) return 0; pos += n;
    n = pack_array_header(&buf[pos], cap - pos, 3);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)mag.x_uT);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)mag.y_uT);
    if (n == 0) return 0; pos += n;
    n = pack_float64(&buf[pos], cap - pos, (double)mag.z_uT);
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

// Decode a packed Telemeter blob into a JSON object for the web
// serializers - this is what lets the conversation render the actual
// readings on both the sender's and the receiver's side. Mirrors
// pack() for our own sensors and tolerates foreign telemeters
// (Sideband phones carry sensors we don't know; unknown SIDs are
// skipped). Only keys for sensors actually present are emitted:
//   time, bat_pct, bat_chg, lat, lon, alt_m, speed_kmh, bearing_deg,
//   acc_m, mbar, temp_c, hum_pct, mag_x, mag_y, mag_z
inline void decode_into(JsonObject out, const uint8_t* data, size_t len) {
  // Any msgpack number -> double. Python packs ints as fixint/intN
  // and floats as float64; our packs are float64 throughout.
  auto read_number = [&](size_t& off, double* out_v) -> bool {
    if (off >= len) return false;
    const uint8_t t = data[off];
    auto be = [&](size_t n) -> uint64_t {
      uint64_t v = 0;
      for (size_t b = 0; b < n; ++b) v = (v << 8) | data[off + 1 + b];
      return v;
    };
    if (t <= 0x7F)  { *out_v = (double)t; off += 1; return true; }
    if (t >= 0xE0)  { *out_v = (double)(int8_t)t; off += 1; return true; }
    switch (t) {
      case 0xCC: if (off + 2 > len) return false; *out_v = (double)be(1); off += 2; return true;
      case 0xCD: if (off + 3 > len) return false; *out_v = (double)be(2); off += 3; return true;
      case 0xCE: if (off + 5 > len) return false; *out_v = (double)be(4); off += 5; return true;
      case 0xCF: if (off + 9 > len) return false; *out_v = (double)be(8); off += 9; return true;
      case 0xD0: if (off + 2 > len) return false; *out_v = (double)(int8_t)be(1);  off += 2; return true;
      case 0xD1: if (off + 3 > len) return false; *out_v = (double)(int16_t)be(2); off += 3; return true;
      case 0xD2: if (off + 5 > len) return false; *out_v = (double)(int32_t)be(4); off += 5; return true;
      case 0xD3: if (off + 9 > len) return false; *out_v = (double)(int64_t)be(8); off += 9; return true;
      case 0xCA: {
        if (off + 5 > len) return false;
        uint32_t u = (uint32_t)be(4); float f; memcpy(&f, &u, 4);
        *out_v = (double)f; off += 5; return true;
      }
      case 0xCB: {
        if (off + 9 > len) return false;
        uint64_t u = be(8); double d; memcpy(&d, &u, 8);
        *out_v = d; off += 9; return true;
      }
    }
    return false;
  };
  // Big-endian struct-packed int in a bin (Location.pack convention).
  auto read_be_bin = [&](size_t& off, size_t want, int64_t* out_v) -> bool {
    if (off + 1 >= len || data[off] != 0xC4 || data[off + 1] != want) return false;
    off += 2;
    if (off + want > len) return false;
    uint64_t v = 0;
    for (size_t b = 0; b < want; ++b) v = (v << 8) | data[off + b];
    off += want;
    if (want == 4)      *out_v = (int32_t)v;
    else if (want == 2) *out_v = (uint16_t)v;
    else                *out_v = (int64_t)v;
    return true;
  };
  auto array_header = [&](size_t& off, size_t* n) -> bool {
    if (off >= len) return false;
    const uint8_t t = data[off];
    if ((t & 0xF0) == 0x90) { *n = t & 0x0F; off += 1; return true; }
    if (t == 0xDC && off + 2 < len) {
      *n = ((size_t)data[off + 1] << 8) | data[off + 2]; off += 3; return true;
    }
    return false;
  };

  size_t off = 0;
  if (off >= len) return;
  const uint8_t mt = data[off];
  size_t entries = 0;
  if ((mt & 0xF0) == 0x80) { entries = mt & 0x0F; off += 1; }
  else if (mt == 0xDE && off + 2 < len) {
    entries = ((size_t)data[off + 1] << 8) | data[off + 2]; off += 3;
  } else return;

  for (size_t i = 0; i < entries && off < len; ++i) {
    const uint8_t kt = data[off++];
    uint8_t sid = 0;
    if (kt <= 0x7F) sid = kt;
    else if (kt == 0xCC && off < len) sid = data[off++];
    else return;
    const size_t value_start = off;
    switch (sid) {
      case SID_TIME: {
        double v;
        if (!read_number(off, &v)) return;
        out["time"] = (int64_t)v;
        continue;
      }
      case SID_BATTERY: {
        // [charge_percent, charging, temperature] - percent may be an
        // int from phone telemeters; temperature is often nil.
        size_t n;
        if (!array_header(off, &n) || n < 2) break;
        double pct;
        if (!read_number(off, &pct)) return;
        out["bat_pct"] = pct;
        if (off < len && (data[off] == 0xC2 || data[off] == 0xC3)) {
          out["bat_chg"] = (data[off] == 0xC3);
        }
        break;
      }
      case SID_LOCATION: {
        size_t n;
        if (!array_header(off, &n) || n < 6) break;
        int64_t la, lo, alt, spd, brg, acc;
        if (read_be_bin(off, 4, &la) && read_be_bin(off, 4, &lo)
            && read_be_bin(off, 4, &alt) && read_be_bin(off, 4, &spd)
            && read_be_bin(off, 4, &brg) && read_be_bin(off, 2, &acc)) {
          const double dlat = (double)la / 1e6, dlon = (double)lo / 1e6;
          if (dlat >= -90.0 && dlat <= 90.0 && dlon >= -180.0 && dlon <= 180.0) {
            out["lat"]         = dlat;
            out["lon"]         = dlon;
            out["alt_m"]       = (double)alt / 100.0;
            out["speed_kmh"]   = (double)spd / 100.0;
            out["bearing_deg"] = (double)brg / 100.0;
            out["acc_m"]       = (double)acc / 100.0;
          }
        }
        break;
      }
      case SID_MAGNETIC_FIELD: {
        size_t n;
        if (!array_header(off, &n) || n < 3) break;
        double x, y, z;
        if (read_number(off, &x) && read_number(off, &y) && read_number(off, &z)) {
          out["mag_x"] = x;
          out["mag_y"] = y;
          out["mag_z"] = z;
        }
        break;
      }
      case SID_PRESSURE: {
        double v;
        if (read_number(off, &v)) out["mbar"] = v;
        break;
      }
      case SID_TEMPERATURE: {
        double v;
        if (read_number(off, &v)) out["temp_c"] = v;
        break;
      }
      case SID_HUMIDITY: {
        double v;
        if (read_number(off, &v)) out["hum_pct"] = v;
        break;
      }
      default:
        break;
    }
    // Re-skip the whole value from its start so partial reads above
    // can't desync the map walk.
    off = value_start;
    if (!Common::MsgPack::skip_element(data, len, off)) return;
  }
}

}  // namespace Telemeter
}  // namespace Telemetry
