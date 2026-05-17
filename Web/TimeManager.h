// TimeManager — unified wall-clock state for the firmware.
//
// LXMF needs a real Unix epoch for the `ts` field on every outbound
// message (peers display message times from it). Reticulum itself is
// time-agnostic but the firmware also uses real time for "X ago"
// labels in the SPA, log timestamps, certificate expiry, etc.
//
// Sources, in default priority order (most trusted first):
//   1. GPS         — UTC from a fix; works without internet
//   2. NTP         — when WiFi-STA has an internet route
//   3. Browser     — POST /api/time from the SPA, populated with the
//                    user's wall clock
//   4. RNS peer    — LXMF::calibrate_time(); the remote's `ts` field
//
// Each source is independently enable-able from the SPA settings UI
// (auth-gated) and the priority order is user-reorderable. When a
// source reports time, the manager adopts it only if its configured
// priority is at least as high as the current source's. Some sources
// carry a per-source `interval_s` setting (GPS poll, NTP refresh)
// that the source's driver reads.
//
// Hardware RTC is *not* a user-configurable source. It's read once at
// cold boot to seed the wall clock with a coarse value that survives
// power-off (PCF8563 with coin-cell backup on the T-Beam Supreme),
// and written through on each successful report from a higher-trust
// source. The RTC handler lives in Web/RtcPCF8563.h and calls
// `seed_from_rtc()` once during setup.
//
// Storage: source config is persisted to `/lxmf/time.json` via
// microStore's filesystem adapter — same pattern as identity meta
// files and `/lxmf/transport.json`. The wall-clock offset itself is
// in-memory only (reseeded from RTC at boot).

#pragma once

#include <stdint.h>
#include <string.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <microStore/FileSystem.h>

namespace Web {
namespace TimeManager {

// User-configurable sources occupy indices 1..4 (GPS, NTP, Browser,
// RNS). RTC is a display-only label for the cold-boot seed —
// reported via source_name() but never exposed in the user's source
// list and not accepted by source_from_name().
enum class Source : uint8_t {
  None    = 0,
  GPS     = 1,
  NTP     = 2,
  Browser = 3,
  RNS     = 4,
  RTC     = 5,
};

constexpr uint8_t SOURCE_COUNT = 5;  // user-visible: None..RNS

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
  // Note: "rtc" deliberately not accepted — it's a display label only,
  // not a user-selectable input source.
  return Source::None;
}

// Per-source config. `interval_s` is interpreted by the source's
// driver — for GPS, how often to consume an NMEA RMC fix and report
// it; for NTP, how often to refresh against pool.ntp.org. Ignored
// for sources where polling doesn't apply (Browser, RNS).
struct SourceConfig {
  bool     enabled;
  uint8_t  priority;    // 0 = highest priority; 255 = ignored.
  uint32_t interval_s;  // 0 = source-driver default; otherwise seconds.
};

inline SourceConfig default_config(Source s) {
  switch (s) {
    case Source::GPS:     return { true,  0, 3600 };  // hourly GPS time reports
    case Source::NTP:     return { true,  1, 3600 };  // hourly NTP refresh
    case Source::Browser: return { true,  2, 0    };  // event-driven
    case Source::RNS:     return { true,  3, 0    };  // event-driven
    case Source::None:    return { false, 255, 0  };
  }
  return { false, 255, 0 };
}

namespace _detail {
  inline SourceConfig& cfg_ref(Source s) {
    static SourceConfig c[SOURCE_COUNT] = {
      default_config(Source::None),
      default_config(Source::GPS),
      default_config(Source::NTP),
      default_config(Source::Browser),
      default_config(Source::RNS),
    };
    const uint8_t idx = (uint8_t)s;
    // Source::RTC (and any future non-user source) is out of array
    // bounds — collapse to the "None" slot which carries the disabled
    // default. RTC is never user-configured via cfg_ref anyway.
    if (idx >= SOURCE_COUNT) return c[0];
    return c[idx];
  }
  // Wall-clock offset: epoch_seconds at the moment millis() was 0.
  // now_epoch() = offset + millis()/1000.
  inline int64_t& offset_seconds_ref()   { static int64_t v = 0; return v; }
  inline Source&  current_source_ref()   { static Source v = Source::None; return v; }
  inline uint8_t& current_priority_ref() { static uint8_t v = 255; return v; }
  // RTC-seed hook — populated by the firmware-side RTC driver if the
  // PCF8563 reads a sensible time at boot. Used for /api/info to
  // surface "we have an RTC value but no live source" state.
  inline bool& rtc_seed_applied_ref()    { static bool v = false; return v; }
  // Post-adopt callback. Fires after a user-visible source's report
  // is accepted (passed priority + sentinel checks). The RTC driver
  // registers here to write the live time through to PCF8563 so the
  // calibration survives reboots.
  using on_adopt_fn = void (*)(Source, double /*epoch*/);
  inline on_adopt_fn& on_adopt_ref()     { static on_adopt_fn fn = nullptr; return fn; }
  // Separate hook for emit-style observers (the WebSocket push, the
  // SPA's clock pill, anyone who wants to know "the clock just moved
  // / its source just changed"). Distinct from on_adopt_ref so the
  // RTC-write slot can't be clobbered by a publisher.
  using on_change_fn = void (*)(Source, double /*epoch*/);
  inline on_change_fn& on_change_ref() { static on_change_fn fn = nullptr; return fn; }
}

// Register a post-adopt callback. Only one slot — RtcPCF8563 takes
// it on the firmware's behalf. Pass nullptr to unhook.
inline void set_on_adopt(_detail::on_adopt_fn fn) {
  _detail::on_adopt_ref() = fn;
}

// Register an observer for time changes. Separate from on_adopt so the
// RTC write-through hook can coexist with an event publisher.
inline void set_on_change(_detail::on_change_fn fn) {
  _detail::on_change_ref() = fn;
}

// Reported time-source state. Returns 0.0 if no source has set time.
inline double now_epoch() {
  if (_detail::current_source_ref() == Source::None
      && !_detail::rtc_seed_applied_ref()) return 0.0;
  return (double)_detail::offset_seconds_ref() + (double)millis() / 1000.0;
}
inline bool   is_calibrated() {
  return _detail::current_source_ref() != Source::None
         || _detail::rtc_seed_applied_ref();
}
inline Source current_source() { return _detail::current_source_ref(); }

// Internal helper that bypasses source-priority gating. Used by the
// RTC seed path at boot — RTC isn't a user-visible source so its
// values shouldn't compete in the priority list, but the wall clock
// still needs a starting point if no live source reports. Marks
// `rtc_seed_applied` so is_calibrated() returns true even before a
// real source reports.
inline void seed_from_rtc(double epoch_seconds) {
  // Sanity-check: refuse anything before 2020 or past 2100.
  if (epoch_seconds < 1577836800.0 || epoch_seconds > 4102444800.0) return;
  _detail::offset_seconds_ref() = (int64_t)epoch_seconds - (int64_t)(millis() / 1000UL);
  _detail::rtc_seed_applied_ref() = true;
  // Label the current source as RTC so /api/time shows where the
  // clock came from. Priority stays at 255 (max) so any real source
  // report still wins via the report_time priority check.
  _detail::current_source_ref()   = Source::RTC;
  _detail::current_priority_ref() = 255;
  if (_detail::on_change_ref()) {
    _detail::on_change_ref()(Source::RTC, epoch_seconds);
  }
}

// A user-visible source reports a time. Adopted iff the source is
// enabled AND its priority beats (or equals) the current source's,
// and the value passes the sanity-check sentinel. Returns true if
// adopted.
inline bool report_time(Source src, double epoch_seconds) {
  if (src == Source::None) return false;
  SourceConfig& cfg = _detail::cfg_ref(src);
  if (!cfg.enabled) return false;
  if (epoch_seconds < 1577836800.0 || epoch_seconds > 4102444800.0) return false;
  // Refuse to be overridden by a lower-priority source. A re-report
  // from the same priority does refresh (e.g. NTP re-syncing).
  if (_detail::current_source_ref() != Source::None
      && cfg.priority > _detail::current_priority_ref()) return false;

  _detail::offset_seconds_ref()   = (int64_t)epoch_seconds - (int64_t)(millis() / 1000UL);
  _detail::current_source_ref()   = src;
  _detail::current_priority_ref() = cfg.priority;
  // Fire the post-adopt hook (RTC write-through, etc). Exceptions
  // here must not break the caller — the callback is fire-and-forget.
  if (_detail::on_adopt_ref()) {
    _detail::on_adopt_ref()(src, epoch_seconds);
  }
  if (_detail::on_change_ref()) {
    _detail::on_change_ref()(src, epoch_seconds);
  }
  return true;
}

inline const SourceConfig& get_config(Source s) { return _detail::cfg_ref(s); }
inline void set_config(Source s, const SourceConfig& cfg) {
  if (s == Source::None) return;
  _detail::cfg_ref(s) = cfg;
}

// JSON persistence, matching the per-identity meta.json / transport.json
// pattern. Path is /lxmf/time.json. Schema:
//   { sources: { gps: {enabled, priority, interval_s}, ntp: {...}, … } }
// Caller passes the filesystem because microStore::FileSystem is
// non-default-constructible — it owns its adapter (Posix/FlashFS/SD).
inline constexpr const char* CONFIG_PATH = "/lxmf/time.json";

inline void load_config(microStore::FileSystem& fs) {
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  JsonObjectConst sources = doc["sources"].as<JsonObjectConst>();
  for (uint8_t i = 1; i < SOURCE_COUNT; ++i) {
    const Source s = (Source)i;
    JsonVariantConst v = sources[source_name(s)];
    if (v.isNull()) continue;
    SourceConfig c = _detail::cfg_ref(s);
    if (v["enabled"].is<bool>())     c.enabled    = v["enabled"].as<bool>();
    if (v["priority"].is<int>())     c.priority   = (uint8_t)v["priority"].as<int>();
    if (v["interval_s"].is<long>())  c.interval_s = (uint32_t)v["interval_s"].as<long>();
    _detail::cfg_ref(s) = c;
  }
}

inline void persist_config(microStore::FileSystem& fs) {
  JsonDocument doc;
  JsonObject sources = doc["sources"].to<JsonObject>();
  for (uint8_t i = 1; i < SOURCE_COUNT; ++i) {
    const Source s = (Source)i;
    const SourceConfig& c = _detail::cfg_ref(s);
    JsonObject o = sources[source_name(s)].to<JsonObject>();
    o["enabled"]    = c.enabled;
    o["priority"]   = c.priority;
    o["interval_s"] = c.interval_s;
  }
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

}  // namespace TimeManager
}  // namespace Web
