// TimeManager — unified wall-clock state for the firmware.
//
// LXMF needs a real Unix epoch for the `ts` field on every outbound
// message (peers display message times from it). Reticulum itself is
// time-agnostic but the firmware also uses real time for "X ago"
// labels in the SPA, log timestamps, certificate expiry, etc.
//
// Sources reported here, in default priority order (most trusted first):
//   1. GPS         — UTC from a fix; works without internet
//   2. NTP         — when WiFi-STA has an internet route
//   3. Browser     — POST /api/time from the SPA, populated with the
//                    user's wall clock
//   4. RNS peer    — LXMF::calibrate_time(); the remote's `ts` field
//   5. Hardware RTC— AXP2101 RTC, only read once at cold boot to seed
//
// Each source is independently enable-able from the SPA settings UI
// (auth-gated). When a source reports time, the manager adopts it
// only if its configured priority is at least as high as the current
// source's. This lets users disable GPS / NTP without losing the
// Browser-set time, and vice versa.
//
// Implementation note: the time offset is stored as Unix-epoch
// seconds at the moment `millis()` was zero. now_epoch() = offset +
// millis()/1000. millis() rolls over every ~49 days, but the offset
// is refreshed on every source report so we never actually hit the
// wrap window unless the device sits uncalibrated for 49 days
// straight.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <EEPROM.h>
#include <string.h>
#include "../ROM.h"

namespace Web {
namespace TimeManager {

// Source identifiers. Persisted to EEPROM as raw uint8_t — do not
// reorder without bumping a schema version.
enum class Source : uint8_t {
  None    = 0,
  GPS     = 1,
  NTP     = 2,
  Browser = 3,
  RNS     = 4,
  RTC     = 5,
};

constexpr uint8_t SOURCE_COUNT = 6;

inline const char* source_name(Source s) {
  switch (s) {
    case Source::GPS:     return "gps";
    case Source::NTP:     return "ntp";
    case Source::Browser: return "browser";
    case Source::RNS:     return "rns";
    case Source::RTC:     return "rtc";
    case Source::None:    return "none";
  }
  return "unknown";
}

inline Source source_from_name(const char* name) {
  if (!name) return Source::None;
  if (strcmp(name, "gps")     == 0) return Source::GPS;
  if (strcmp(name, "ntp")     == 0) return Source::NTP;
  if (strcmp(name, "browser") == 0) return Source::Browser;
  if (strcmp(name, "rns")     == 0) return Source::RNS;
  if (strcmp(name, "rtc")     == 0) return Source::RTC;
  return Source::None;
}

struct SourceConfig {
  bool    enabled;
  uint8_t priority;  // 0 = highest priority; 255 = ignored. Sources at
                     // equal priority are tie-broken by Source enum
                     // ordinal (GPS < NTP < Browser < RNS < RTC).
};

// Default priority + enable flags. Mirrors the user-stated preference
// (GPS, NTP, Browser, RNS) and adds RTC as a low-priority cold-boot
// fallback (always enabled but only reported once at boot).
inline SourceConfig default_config(Source s) {
  switch (s) {
    case Source::GPS:     return { true,  0 };
    case Source::NTP:     return { true,  1 };
    case Source::Browser: return { true,  2 };
    case Source::RNS:     return { true,  3 };
    case Source::RTC:     return { true,  4 };
    case Source::None:    return { false, 255 };
  }
  return { false, 255 };
}

namespace _detail {
  inline SourceConfig& cfg_ref(Source s) {
    static SourceConfig c[SOURCE_COUNT] = {
      default_config(Source::None),
      default_config(Source::GPS),
      default_config(Source::NTP),
      default_config(Source::Browser),
      default_config(Source::RNS),
      default_config(Source::RTC),
    };
    return c[(uint8_t)s];
  }
  // Current wall-clock state.
  inline int64_t& offset_seconds_ref() { static int64_t v = 0; return v; }
  inline Source&  current_source_ref() { static Source v = Source::None; return v; }
  inline uint8_t& current_priority_ref() { static uint8_t v = 255; return v; }
}

// Returns whatever time has been set, or 0.0 if nothing has reported.
// Callers that need a calibrated-only time should gate on is_calibrated().
inline double now_epoch() {
  if (_detail::current_source_ref() == Source::None) return 0.0;
  return (double)_detail::offset_seconds_ref() + (double)millis() / 1000.0;
}

inline bool is_calibrated() {
  return _detail::current_source_ref() != Source::None;
}

inline Source current_source() {
  return _detail::current_source_ref();
}

// A source reports a time. Manager adopts it if the source is enabled
// AND its priority is at least as high as the current source's (lower
// numeric value = higher priority). Returns true if adopted.
inline bool report_time(Source src, double epoch_seconds) {
  if (src == Source::None) return false;
  SourceConfig& cfg = _detail::cfg_ref(src);
  if (!cfg.enabled) return false;
  // Sentinel-check the reported time: anything before 2020-01-01 or
  // after 2100-01-01 is clearly garbage.
  static constexpr double MIN_EPOCH = 1577836800.0;  // 2020-01-01T00:00Z
  static constexpr double MAX_EPOCH = 4102444800.0;  // 2100-01-01T00:00Z
  if (epoch_seconds < MIN_EPOCH || epoch_seconds > MAX_EPOCH) return false;
  // Refuse to be overridden by a lower-priority source. A re-report
  // from the same priority does refresh (e.g. NTP re-syncing).
  if (cfg.priority > _detail::current_priority_ref()
      && _detail::current_source_ref() != Source::None) return false;

  const int64_t new_offset = (int64_t)epoch_seconds - (int64_t)(millis() / 1000UL);
  _detail::offset_seconds_ref()  = new_offset;
  _detail::current_source_ref()  = src;
  _detail::current_priority_ref()= cfg.priority;
  return true;
}

inline const SourceConfig& get_config(Source s) {
  return _detail::cfg_ref(s);
}

inline void set_config(Source s, const SourceConfig& cfg) {
  if (s == Source::None) return;
  _detail::cfg_ref(s) = cfg;
}

// EEPROM layout for source-config persistence. One byte per source for
// enable, one byte for priority. Anchored at the caller-supplied
// eeprom_base address (which is `eeprom_addr(ADDR_CONF_TIME_SRC)` —
// resolved by the firmware where Config.h is in scope). Magic byte on
// the first slot doubles as the "config is valid" sentinel — 0xCB
// means "loaded from EEPROM", anything else means "use defaults".
//
// Decoupled from the eeprom_addr() macro here because the macro is
// defined in Config.h which can't be included from a header file
// without causing duplicate-symbol errors (Config.h declares vars at
// translation-unit scope). Callers pass the resolved address in. (#113)
inline constexpr uint8_t TIME_SRC_MAGIC = 0xCB;

inline void load_config(int eeprom_base) {
  const uint8_t magic = EEPROM.read(eeprom_base);
  if (magic != TIME_SRC_MAGIC) return;
  for (uint8_t i = 1; i < SOURCE_COUNT; ++i) {
    const uint8_t en  = EEPROM.read(eeprom_base + 1 + (i - 1) * 2);
    const uint8_t pri = EEPROM.read(eeprom_base + 1 + (i - 1) * 2 + 1);
    _detail::cfg_ref((Source)i).enabled  = (en != 0);
    _detail::cfg_ref((Source)i).priority = pri;
  }
}

inline void persist_config(int eeprom_base) {
  EEPROM.write(eeprom_base, TIME_SRC_MAGIC);
  for (uint8_t i = 1; i < SOURCE_COUNT; ++i) {
    const SourceConfig& c = _detail::cfg_ref((Source)i);
    EEPROM.write(eeprom_base + 1 + (i - 1) * 2,     c.enabled ? 1 : 0);
    EEPROM.write(eeprom_base + 1 + (i - 1) * 2 + 1, c.priority);
  }
  EEPROM.commit();
}

}  // namespace TimeManager
}  // namespace Web
