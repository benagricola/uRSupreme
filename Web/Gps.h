// GPS driver — minimal NMEA-0183 parser for the L76K-class module on
// the T-Beam Supreme (UART1 on GPIO 8/9 at 9600 baud).
//
// Responsibilities:
//   * Power-enable the GPS module (GPS_EN pin) at boot.
//   * Open the UART at 9600 8N1.
//   * Stream-parse NMEA sentences, validating the XOR checksum.
//   * On valid $..RMC frames: extract UTC time + date + lat/long +
//     speed + heading. Cache the most recent fix in a `Fix` struct.
//   * When the fix's status flag is A (Active) and the user-
//     configured GPS time-source poll interval has elapsed, call
//     `Web::TimeManager::report_time(Source::GPS, epoch)`.
//   * Expose the last fix via `last_fix()` so the SPA can read
//     position over the existing HTTP surface.
//   * Power-cycle the module via the AXP2101's ALDO4 rail when the
//     poll interval is long enough that pulsed-mode operation saves
//     meaningful current.
//
// Power management:
//   GPS time-source enabled  + interval_s <  PULSE_THRESHOLD_S → always on
//   GPS time-source enabled  + interval_s >= PULSE_THRESHOLD_S → pulsed
//                              (power up, acquire fix, report, power down)
//   GPS time-source disabled                                    → powered off
//   Position tracking follows the same gating as time reports —
//   if the user disables GPS, the chip is off and position is stale.
//
// The parser is line-based — calls into `pump()` from loopTask drain
// up to a few hundred bytes per pass, accumulate one line in a
// fixed-size buffer, dispatch when '\n' arrives or the buffer fills.
// No dynamic allocation in the hot path.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <time.h>
#include <XPowersLib.h>
#include "TimeManager.h"

extern XPowersLibInterface* PMU;

namespace Web {
namespace Gps {

struct Pins {
  int rx;          // GPS_RX_PIN on the Supreme: data FROM the L76K
  int tx;          // GPS_TX_PIN on the Supreme: data TO the L76K
  int en;          // GPS_EN_PIN; -1 if no enable line
  uint32_t baud = 9600;
};

struct Fix {
  bool     valid           = false;  // RMC status flag = 'A'
  double   latitude_deg    = 0.0;    // signed decimal degrees
  double   longitude_deg   = 0.0;    // signed decimal degrees
  double   speed_knots     = 0.0;
  double   heading_deg     = 0.0;
  double   unix_epoch      = 0.0;    // UTC seconds since 1970
  uint32_t fix_received_ms = 0;      // millis() when last RMC parsed
  uint32_t last_byte_ms    = 0;      // millis() last time the UART produced anything
};

// If the user's GPS poll interval is at or above this, run the chip
// in pulsed mode: power on, hunt for a fix, report, power off. Below
// it, keep the chip continuously powered — frequent cycling would
// shred the cold-start budget.
inline constexpr uint32_t PULSE_THRESHOLD_S    = 5 * 60;     // 5 min
inline constexpr uint32_t PULSE_ACQUIRE_TIMEOUT_MS = 120000; // give up after 2 min hunting
inline constexpr uint32_t PULSE_RETRY_BACKOFF_MS   = 60000;  // try again 60 s later on timeout

enum class PowerMode : uint8_t { Off, AlwaysOn, Pulsed };
enum class PulseState : uint8_t { Idle, Acquiring };

namespace _detail {
  inline HardwareSerial*& serial_ref() { static HardwareSerial* v = nullptr; return v; }
  inline Pins&           pins_ref()    { static Pins p{ -1, -1, -1, 9600 }; return p; }
  inline Fix&            fix_ref()     { static Fix f{}; return f; }
  // Line buffer + checksum-accumulator state. NMEA sentence max is
  // 82 bytes including $ and \r\n; we round up.
  inline char (&line_buf_ref())[96]   { static char buf[96]; return buf; }
  inline size_t&         line_len_ref() { static size_t v = 0; return v; }
  // Last time we reported a time to TimeManager (millis()). 0 = never.
  inline uint32_t&       last_report_ms_ref() { static uint32_t v = 0; return v; }
  // Power-mode state.
  inline bool&           hw_powered_ref()     { static bool v = false; return v; }
  inline PulseState&     pulse_state_ref()    { static PulseState v = PulseState::Idle; return v; }
  inline uint32_t&       pulse_started_ms_ref(){ static uint32_t v = 0; return v; }

  // XOR-checksum the chars between '$' and '*' exclusive. The
  // sentence may or may not include the leading '$'.
  inline bool checksum_ok(const char* line, size_t len) {
    const char* star = (const char*)memchr(line, '*', len);
    if (!star) return false;
    if (star + 3 > line + len) return false;
    const char* p = line;
    if (*p == '$') ++p;
    uint8_t cs = 0;
    while (p < star) { cs ^= (uint8_t)*p++; }
    // Hex compare.
    auto fromhex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      return -1;
    };
    const int h = fromhex(star[1]);
    const int l = fromhex(star[2]);
    if (h < 0 || l < 0) return false;
    return cs == ((h << 4) | l);
  }

  // Split a NMEA payload (no leading $ or trailing *cs\r\n) at
  // commas into up to N fields. Returns the actual count.
  inline size_t split_fields(char* p, size_t len, char* fields[], size_t max_fields) {
    size_t n = 0;
    fields[n++] = p;
    for (size_t i = 0; i < len && n < max_fields; ++i) {
      if (p[i] == ',') {
        p[i] = '\0';
        fields[n++] = &p[i + 1];
      }
    }
    return n;
  }

  // NMEA "ddmm.mmmm" / "dddmm.mmmm" → decimal degrees.
  // `degree_digits` is 2 (lat) or 3 (lon).
  inline double parse_coord(const char* s, int degree_digits) {
    if (!s || !*s) return 0.0;
    char buf[16];
    size_t n = strnlen(s, sizeof(buf) - 1);
    if (n < (size_t)(degree_digits + 1)) return 0.0;
    // First `degree_digits` chars are the integer degrees.
    int deg = 0;
    for (int i = 0; i < degree_digits; ++i) {
      const char c = s[i];
      if (c < '0' || c > '9') return 0.0;
      deg = deg * 10 + (c - '0');
    }
    const double minutes = atof(s + degree_digits);
    return (double)deg + minutes / 60.0;
  }

  // "hhmmss.sss" → seconds-of-day. "ddmmyy" → date components.
  // Together → unix epoch (UTC) via mktime.
  inline double parse_rmc_datetime(const char* time_str, const char* date_str) {
    if (!time_str || strlen(time_str) < 6) return 0.0;
    if (!date_str || strlen(date_str) != 6) return 0.0;
    char t[12];
    strncpy(t, time_str, sizeof(t) - 1);
    t[sizeof(t) - 1] = '\0';
    auto digits2 = [](const char* p) {
      return (p[0] - '0') * 10 + (p[1] - '0');
    };
    struct tm tt{};
    tt.tm_year  = (2000 + digits2(date_str + 4)) - 1900;
    tt.tm_mon   = digits2(date_str + 2) - 1;
    tt.tm_mday  = digits2(date_str);
    tt.tm_hour  = digits2(t);
    tt.tm_min   = digits2(t + 2);
    tt.tm_sec   = digits2(t + 4);
    tt.tm_isdst = 0;
    return (double)mktime(&tt);
  }

  inline void handle_line(char* line, size_t len) {
    // Strip optional leading $ and trailing \r.
    char* p = line;
    if (len && *p == '$') { ++p; --len; }
    while (len && (p[len - 1] == '\r' || p[len - 1] == '\n')) --len;
    if (!checksum_ok(p - 1, len + 1) && !checksum_ok(p, len)) return;
    // Only care about RMC sentences. Talker IDs vary by constellation
    // (GP, GN, GL, GA, BD); match suffix.
    if (len < 5) return;
    if (p[2] != 'R' || p[3] != 'M' || p[4] != 'C') return;
    // Cut at the '*' before checksum so split_fields doesn't include it.
    char* star = (char*)memchr(p, '*', len);
    if (star) { *star = '\0'; len = (size_t)(star - p); }
    // Drop the talker prefix + "RMC" + comma. Fields[0..] are the
    // RMC body.
    char* body = (char*)memchr(p, ',', len);
    if (!body) return;
    ++body;
    char* fields[16] = {nullptr};
    const size_t nfields = split_fields(body, len - (body - p), fields, 16);
    if (nfields < 10) return;
    Fix& f = fix_ref();
    f.valid = (fields[1] && fields[1][0] == 'A');
    if (!f.valid) {
      f.fix_received_ms = millis();
      return;
    }
    const double lat = parse_coord(fields[2], 2);
    const double lon = parse_coord(fields[4], 3);
    f.latitude_deg  = (fields[3] && fields[3][0] == 'S') ? -lat : lat;
    f.longitude_deg = (fields[5] && fields[5][0] == 'W') ? -lon : lon;
    f.speed_knots   = fields[6] ? atof(fields[6]) : 0.0;
    f.heading_deg   = fields[7] ? atof(fields[7]) : 0.0;
    f.unix_epoch    = parse_rmc_datetime(fields[0], fields[8]);
    f.fix_received_ms = millis();

    // Time reporting — respect the user's GPS interval.
    // interval_s = 0  → "at boot" only: report once, never repoll
    // interval_s > 0  → seconds between repolls
    if (f.unix_epoch > 0.0) {
      const auto& cfg = Web::TimeManager::get_config(Web::TimeManager::Source::GPS);
      const uint32_t now = millis();
      const bool first_time = (last_report_ms_ref() == 0);
      const bool may_repoll = (cfg.interval_s > 0)
          && ((now - last_report_ms_ref()) >= cfg.interval_s * 1000UL);
      if (first_time || may_repoll) {
        if (Web::TimeManager::report_time(
              Web::TimeManager::Source::GPS, f.unix_epoch)) {
          last_report_ms_ref() = now;
        }
      }
    }
  }
}

// Boot init. Pin 7 (GPS_EN) is held HIGH to power the module; pin 9
// is RX (data from GPS), pin 8 is TX (commands to GPS), 9600 8N1.
inline void begin(HardwareSerial& serial, const Pins& pins) {
  _detail::serial_ref() = &serial;
  _detail::pins_ref()   = pins;
  if (pins.en >= 0) {
    pinMode(pins.en, OUTPUT);
    digitalWrite(pins.en, HIGH);   // power the L76K
    delay(20);
  }
  serial.begin(pins.baud, SERIAL_8N1, pins.rx, pins.tx);
  // Power.h has already enabled ALDO4 at PMU init; mark the rail as
  // tracked so power_off() knows it's safe to cut it.
  _detail::hw_powered_ref() = true;
  NOTICEF("GPS: UART up on rx=%d tx=%d en=%d baud=%lu",
          pins.rx, pins.tx, pins.en, (unsigned long)pins.baud);
}

// Bring the GPS chip back up — ALDO4 on, GPS_EN high. Idempotent.
inline void power_on() {
  if (PMU && !PMU->isPowerChannelEnable(XPOWERS_ALDO4)) {
    PMU->setPowerChannelVoltage(XPOWERS_ALDO4, 3300);
    PMU->enablePowerOutput(XPOWERS_ALDO4);
  }
  const int en = _detail::pins_ref().en;
  if (en >= 0) digitalWrite(en, HIGH);
  _detail::hw_powered_ref() = true;
}

// Cut power to the GPS chip. ALDO4 off, GPS_EN low. Drops the in-
// flight NMEA parser state so partial lines don't bleed into the
// next power-on. Idempotent.
inline void power_off() {
  if (PMU) PMU->disablePowerOutput(XPOWERS_ALDO4);
  const int en = _detail::pins_ref().en;
  if (en >= 0) digitalWrite(en, LOW);
  _detail::hw_powered_ref()      = false;
  _detail::line_len_ref()        = 0;
  _detail::fix_ref().last_byte_ms = 0;
}

inline bool is_powered() { return _detail::hw_powered_ref(); }
inline PulseState pulse_state() { return _detail::pulse_state_ref(); }

// Decide the current target power mode from the user's GPS time-
// source config. Source enable-disable is the master switch; the
// interval picks always-on vs pulsed.
inline PowerMode target_mode() {
  const auto& cfg = Web::TimeManager::get_config(Web::TimeManager::Source::GPS);
  if (!cfg.enabled)                              return PowerMode::Off;
  if (cfg.interval_s < PULSE_THRESHOLD_S)        return PowerMode::AlwaysOn;
  return PowerMode::Pulsed;
}

// Drain whatever's in the UART buffer and feed it to the line parser.
// Also drives the power-mode state machine so the chip cycles on
// only when a poll is due. Call from the main loop; cheap when
// nothing's pending.
inline void pump() {
  HardwareSerial* s = _detail::serial_ref();
  if (!s) return;
  const PowerMode mode = target_mode();
  const uint32_t  now  = millis();
  Fix&            f    = _detail::fix_ref();
  const auto&     cfg  = Web::TimeManager::get_config(Web::TimeManager::Source::GPS);

  // ---- power-mode transitions ----
  if (mode == PowerMode::Off) {
    if (_detail::hw_powered_ref()) power_off();
    _detail::pulse_state_ref() = PulseState::Idle;
    return;                                         // no bytes will come; skip drain
  }
  if (mode == PowerMode::AlwaysOn) {
    if (!_detail::hw_powered_ref()) power_on();
    _detail::pulse_state_ref() = PulseState::Idle;
  }
  if (mode == PowerMode::Pulsed) {
    const uint32_t last_rep = _detail::last_report_ms_ref();
    const uint32_t interval_ms = (uint32_t)cfg.interval_s * 1000UL;
    const bool first_time = (last_rep == 0);
    const bool due        = first_time || ((now - last_rep) >= interval_ms);
    if (_detail::pulse_state_ref() == PulseState::Idle) {
      if (due) {
        power_on();
        _detail::pulse_state_ref()      = PulseState::Acquiring;
        _detail::pulse_started_ms_ref() = now;
        NOTICEF("GPS: pulse start (interval=%lus, last=%lums ago)",
                (unsigned long)cfg.interval_s,
                (unsigned long)(first_time ? 0 : (now - last_rep)));
      } else {
        // Idle between polls — make sure power is off.
        if (_detail::hw_powered_ref()) power_off();
      }
    } else /* Acquiring */ {
      const bool acquired = (last_rep >= _detail::pulse_started_ms_ref())
                         && (last_rep != 0);
      const bool timed_out = (now - _detail::pulse_started_ms_ref())
                              > PULSE_ACQUIRE_TIMEOUT_MS;
      if (acquired) {
        // Got a fix + reported time; back to sleep.
        NOTICEF("GPS: pulse acquired in %lums", (unsigned long)(now - _detail::pulse_started_ms_ref()));
        power_off();
        _detail::pulse_state_ref() = PulseState::Idle;
      } else if (timed_out) {
        // No fix within the window. Power down and back off slightly
        // so we don't immediately retry; next poll attempt happens
        // PULSE_RETRY_BACKOFF_MS from now.
        WARNINGF("GPS: pulse timed out after %lums; backing off %lums",
                 (unsigned long)PULSE_ACQUIRE_TIMEOUT_MS,
                 (unsigned long)PULSE_RETRY_BACKOFF_MS);
        power_off();
        _detail::pulse_state_ref() = PulseState::Idle;
        // Set last_report_ms so the next due-check fires PULSE_RETRY_BACKOFF_MS
        // from now rather than interval_s from "never".
        if (interval_ms > PULSE_RETRY_BACKOFF_MS) {
          _detail::last_report_ms_ref() = now - (interval_ms - PULSE_RETRY_BACKOFF_MS);
        } else {
          _detail::last_report_ms_ref() = now;
        }
      }
    }
  }

  // ---- drain UART (only meaningful if powered) ----
  if (!_detail::hw_powered_ref()) return;
  while (s->available()) {
    const int c = s->read();
    if (c < 0) break;
    f.last_byte_ms = millis();
    auto& buf  = _detail::line_buf_ref();
    auto& blen = _detail::line_len_ref();
    if (c == '\n') {
      buf[blen] = '\0';
      _detail::handle_line(buf, blen);
      blen = 0;
    }
    else if (blen + 1 < sizeof(buf)) {
      buf[blen++] = (char)c;
    }
    else {
      // Overrun — drop the line, resync on next \n.
      blen = 0;
    }
  }
}

// Read access for /api/gps. Caller gets a copy of the current fix.
inline Fix last_fix() { return _detail::fix_ref(); }

inline bool has_serial() { return _detail::serial_ref() != nullptr; }

}  // namespace Gps
}  // namespace Web
