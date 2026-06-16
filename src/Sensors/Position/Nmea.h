// NMEA-0183 sentence parsing - the protocol both GNSS modules speak.
//
// Pure functions over one line of text: no I/O, no power management,
// no clock coupling. Gnss.h owns the UART and the policy; this file
// only turns sentences into Fix updates and classifies what it saw so
// the caller can react (RMC carries the epoch the time-sync policy
// wants; TXT carries the module banners the identification probe
// wants).
//
// Handled sentences, any talker prefix (GP/GN/GL/GA/BD):
//   RMC - validity, lat/lon, speed, heading, UTC date+time
//   GGA - altitude + satellite count (and a redundant position we
//         ignore), gated on fix quality
//   GSV - satellites in view + per-satellite signal strength. Works
//         WITHOUT a fix, so indoors the UI can still show "N visible,
//         signal X dB" instead of nothing. Arrives per constellation
//         (GPGSV, GLGSV, ...), aggregated across talkers here.
//   TXT - free-text announcements (power-on banners, probe replies)

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

namespace Sensors {
namespace Nmea {

struct Fix {
  bool     valid           = false;  // RMC status flag = 'A'
  double   latitude_deg    = 0.0;    // signed decimal degrees
  double   longitude_deg   = 0.0;    // signed decimal degrees
  double   altitude_m      = 0.0;    // Mean-sea-level altitude in metres (from GGA).
                                     // Updated independently from lat/lon - GGA arrives
                                     // separately from RMC each second.
  bool     altitude_valid  = false;  // True once we've seen at least one GGA with
                                     // fix_quality >= 1 since boot (or since last fix lost).
  uint8_t  sats            = 0;      // Satellites in use (GGA field 6, or UBX numSV)
  float    hdop            = 0.0f;   // Horizontal dilution of precision (GGA field 7)
  bool     hdop_valid      = false;  // True once a fixed GGA carried an HDOP value
  uint8_t  sats_visible    = 0;      // Satellites in view, summed across constellations (GSV)
  uint8_t  sats_tracked    = 0;      // Satellites actually heard (GSV C/N0 > 0), summed
  uint8_t  best_snr_db     = 0;      // Strongest C/N0 seen in the current GSV cycle
  // Accuracy estimates. Only the MAX-M10 fills these (UBX-NAV-PVT
  // hAcc/vAcc); the L76K's NMEA stream carries no direct accuracy.
  float    hacc_m          = 0.0f;
  float    vacc_m          = 0.0f;
  bool     acc_valid       = false;
  double   speed_knots     = 0.0;
  double   heading_deg     = 0.0;
  double   unix_epoch      = 0.0;    // UTC seconds since 1970
  uint32_t fix_received_ms = 0;      // millis() when last RMC parsed (valid OR not)
  uint32_t last_valid_fix_ms = 0;    // millis() of the last *valid* fix; 0 = never fixed
  uint32_t last_byte_ms    = 0;      // millis() last time the UART produced anything
};

enum class Sentence : uint8_t { None, RmcValid, RmcInvalid, Gga, Gsv, Txt, Other };

namespace _detail {
  // GSV aggregation across constellations. Each talker (GP, GL, GA,
  // GB, ...) reports its own satellites-in-view total; the UI wants
  // the sum. Small fixed table keyed by the two talker chars; SNR max
  // resets when a cycle goes quiet so a stale strong reading cannot
  // linger.
  inline constexpr size_t  GSV_TALKERS = 8;
  inline constexpr uint32_t GSV_CYCLE_MS = 5000;
  struct GsvState {
    char    talker[GSV_TALKERS][2] = {};
    uint8_t in_view[GSV_TALKERS]   = {};
    uint8_t tracked[GSV_TALKERS]   = {};   // sats heard (C/N0 > 0) per talker
    uint8_t best_snr = 0;
    uint32_t last_ms = 0;
  };
  inline GsvState& gsv() { static GsvState g; return g; }
}

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

// Parse one line in place (the buffer is mutated by field splitting)
// and apply it to `f`. Returns what the line was. For TXT sentences,
// `out_txt` (optional) points at the sentence body inside the buffer
// - valid until the buffer is reused.
inline Sentence parse_line(char* line, size_t len, Fix& f, const char** out_txt = nullptr) {
  if (out_txt) *out_txt = nullptr;
  char* p = line;
  if (len && *p == '$') { ++p; --len; }
  while (len && (p[len - 1] == '\r' || p[len - 1] == '\n')) --len;
  if (!checksum_ok(p - 1, len + 1) && !checksum_ok(p, len)) return Sentence::None;
  if (len < 5) return Sentence::None;
  const bool is_rmc = (p[2] == 'R' && p[3] == 'M' && p[4] == 'C');
  const bool is_gga = (p[2] == 'G' && p[3] == 'G' && p[4] == 'A');
  const bool is_gsv = (p[2] == 'G' && p[3] == 'S' && p[4] == 'V');
  const bool is_txt = (p[2] == 'T' && p[3] == 'X' && p[4] == 'T');
  if (!is_rmc && !is_gga && !is_gsv && !is_txt) return Sentence::Other;
  // Cut at the '*' before checksum so split_fields doesn't include it.
  char* star = (char*)memchr(p, '*', len);
  if (star) { *star = '\0'; len = (size_t)(star - p); }
  char* body = (char*)memchr(p, ',', len);
  if (!body) return Sentence::Other;
  ++body;
  // GSV carries up to 4 quads after 3 header fields = 19 fields.
  char* fields[20] = {nullptr};
  const size_t nfields = split_fields(body, len - (body - p), fields, 20);

  if (is_gsv) {
    // $xxGSV,totalMsgs,msgNum,satsInView,(prn,elev,azim,snr)x0..4
    if (nfields < 3) return Sentence::Gsv;
    auto& g = _detail::gsv();
    const uint32_t now = millis();
    if (now - g.last_ms > _detail::GSV_CYCLE_MS) {
      // New reporting cycle after silence - drop stale aggregates.
      memset(g.in_view, 0, sizeof(g.in_view));
      memset(g.tracked, 0, sizeof(g.tracked));
      memset(g.talker, 0, sizeof(g.talker));
      g.best_snr = 0;
    }
    g.last_ms = now;
    // Find/assign this talker's slot and record its in-view total.
    int slot = -1;
    for (size_t i = 0; i < _detail::GSV_TALKERS; ++i) {
      const bool empty = (g.talker[i][0] == 0);
      if (empty || (g.talker[i][0] == p[0] && g.talker[i][1] == p[1])) {
        g.talker[i][0] = p[0];
        g.talker[i][1] = p[1];
        g.in_view[i]   = (uint8_t)atoi(fields[2]);
        slot = (int)i;
        break;
      }
    }
    // msgNum 1 begins this constellation's GSV set; reset its heard count
    // so the multi-sentence set accumulates without double-counting (the
    // 5 s cycle window spans several 1 Hz bursts, so a naive sum would
    // multiply).
    if (slot >= 0 && fields[1] && atoi(fields[1]) == 1) g.tracked[slot] = 0;
    // Each quad's field 4 is C/N0, empty when a satellite is searched but
    // not yet tracked. Count the ones actually heard and keep the max.
    for (size_t q = 3; q + 3 < nfields; q += 4) {
      if (fields[q + 3] && fields[q + 3][0] != '\0') {
        const int snr = atoi(fields[q + 3]);
        if (snr > 0 && snr <= 99) {
          if (slot >= 0 && g.tracked[slot] < 255) g.tracked[slot]++;
          if ((uint8_t)snr > g.best_snr) g.best_snr = (uint8_t)snr;
        }
      }
    }
    uint16_t total = 0, heard = 0;
    for (size_t i = 0; i < _detail::GSV_TALKERS; ++i) {
      total += g.in_view[i];
      heard += g.tracked[i];
    }
    f.sats_visible = (uint8_t)(total > 255 ? 255 : total);
    f.sats_tracked = (uint8_t)(heard > 255 ? 255 : heard);
    f.best_snr_db  = g.best_snr;
    return Sentence::Gsv;
  }
  if (is_txt) {
    // $GPTXT,xx,yy,zz,text - the free text is field [3].
    if (out_txt && nfields >= 4) *out_txt = fields[3];
    return Sentence::Txt;
  }
  if (is_gga) {
    // GGA body fields:
    //   [0] UTC time, [1] lat ddmm.mmmm, [2] N/S, [3] lon dddmm.mmmm,
    //   [4] E/W, [5] fix quality (0=no fix, 1=GPS, 2=DGPS, ...),
    //   [6] num sats, [7] HDOP, [8] altitude, [9] altitude units (M),
    //   [10] geoid separation, [11] geoid units, ...
    // Use altitude only when fix_quality >= 1.
    if (nfields < 10) return Sentence::Gga;
    const int fq = fields[5] ? atoi(fields[5]) : 0;
    if (fq <= 0) {
      f.altitude_valid = false;
      f.hdop_valid     = false;
      return Sentence::Gga;
    }
    if (fields[6] && fields[6][0] != '\0') {
      f.sats = (uint8_t)atoi(fields[6]);
    }
    if (fields[7] && fields[7][0] != '\0') {
      f.hdop       = (float)atof(fields[7]);
      f.hdop_valid = true;
    }
    if (fields[8] && fields[8][0] != '\0') {
      f.altitude_m     = atof(fields[8]);
      f.altitude_valid = true;
    }
    return Sentence::Gga;
  }
  // RMC.
  if (nfields < 10) return Sentence::RmcInvalid;
  f.valid = (fields[1] && fields[1][0] == 'A');
  if (!f.valid) {
    f.fix_received_ms = millis();
    return Sentence::RmcInvalid;
  }
  const double lat = parse_coord(fields[2], 2);
  const double lon = parse_coord(fields[4], 3);
  f.latitude_deg  = (fields[3] && fields[3][0] == 'S') ? -lat : lat;
  f.longitude_deg = (fields[5] && fields[5][0] == 'W') ? -lon : lon;
  f.speed_knots   = fields[6] ? atof(fields[6]) : 0.0;
  f.heading_deg   = fields[7] ? atof(fields[7]) : 0.0;
  f.unix_epoch    = parse_rmc_datetime(fields[0], fields[8]);
  f.fix_received_ms = millis();
  f.last_valid_fix_ms = millis();   // reached only on a valid fix
  return Sentence::RmcValid;
}

}  // namespace Nmea
}  // namespace Sensors
