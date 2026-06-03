// NTP time source driver.
//
// Uses ESP-IDF's bundled SNTP client (lwip/apps/sntp.c) to fetch UTC
// epoch from pool.ntp.org whenever the device is on WiFi STA with
// internet access. Adoption follows TimeManager's priority rules —
// the GPS source still wins by default, but NTP overrides Browser
// and RNS unless the user reordered them.
//
// Responsibilities:
//   * begin(): configure + start the SNTP client. Non-blocking; the
//     actual sync happens asynchronously when WiFi comes up.
//   * pump(): called from the main loop. When sntp_get_sync_status()
//     reports COMPLETED, read the system clock, sanity-check it, and
//     pass to TimeManager::report_time(Source::NTP, epoch). Respects
//     the configured interval: 0 → "at boot" (report once, never
//     refresh); >0 → seconds between forced re-syncs via
//     sntp_restart().
//
// The SNTP system itself calls settimeofday() automatically when an
// answer arrives, so the device's internal clock is also set as a
// side effect — useful for any code that calls time(NULL) directly.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
extern "C" {
  #include "esp_sntp.h"
}

#include "Manager.h"

namespace Clock {
namespace Ntp {

namespace _detail {
  inline bool&     started_ref()         { static bool v = false; return v; }
  // millis() at the last successful adoption. 0 = never adopted.
  inline uint32_t& last_adopt_ms_ref()   { static uint32_t v = 0; return v; }
  // Last sync_status snapshot. Used to detect transitions
  // RESET/IN_PROGRESS → COMPLETED so we don't re-adopt every loop.
  inline sntp_sync_status_t& last_status_ref() {
    static sntp_sync_status_t v = SNTP_SYNC_STATUS_RESET;
    return v;
  }
}

// One-shot init: configure pool.ntp.org and start SNTP. Safe to call
// before WiFi is up — SNTP will sit idle until DHCP/DNS becomes
// available. Subsequent calls are no-ops.
inline void begin() {
  if (_detail::started_ref()) return;
  // SNTP_OPMODE_POLL is a plain int macro in lwip's sntp.h; cast to
  // satisfy the IDF wrapper's enum-typed signature under C++.
  esp_sntp_setoperatingmode(static_cast<esp_sntp_operatingmode_t>(SNTP_OPMODE_POLL));
  esp_sntp_setservername(0, (char*)"pool.ntp.org");
  esp_sntp_init();
  _detail::started_ref() = true;
  NOTICE("NTP: SNTP client initialised — will sync against pool.ntp.org when WiFi STA has internet");
}

// Called from the main loop. Cheap on most ticks: checks sntp status
// and reports to TimeManager on COMPLETED transitions.
inline void pump() {
  if (!_detail::started_ref()) return;

  const auto& cfg = Clock::Manager::get_config(Clock::Manager::Source::NTP);
  if (!cfg.enabled) return;

  // No WiFi → SNTP can't make progress. Don't burn CPU.
  if (WiFi.status() != WL_CONNECTED) return;

  const auto status = sntp_get_sync_status();
  const auto& last  = _detail::last_status_ref();

  // Transition into COMPLETED → adopt.
  if (status == SNTP_SYNC_STATUS_COMPLETED && last != SNTP_SYNC_STATUS_COMPLETED) {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    const double epoch = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
    // Sentinel check. SNTP shouldn't hand back garbage but guard
    // anyway so a misconfigured server can't bump us back to 1970.
    if (epoch >= 1577836800.0 && epoch <= 4102444800.0) {
      if (Clock::Manager::report_time(Clock::Manager::Source::NTP, epoch)) {
        _detail::last_adopt_ms_ref() = millis();
        NOTICEF("NTP: adopted epoch %.0f from pool.ntp.org", epoch);
      }
    }
  }
  _detail::last_status_ref() = status;

  // Periodic re-sync. interval_s = 0 → "at boot" only: never repoll
  // (matches the SPA's interval dropdown semantics).
  if (cfg.interval_s > 0 && _detail::last_adopt_ms_ref() != 0) {
    const uint32_t elapsed = millis() - _detail::last_adopt_ms_ref();
    if (elapsed >= cfg.interval_s * 1000UL) {
      sntp_restart();
      // Force the next COMPLETED transition to be observed by
      // clearing our cached status — sntp_restart resets the lwip
      // side to RESET, but a fast LAN could land COMPLETED before
      // our next pump() and skip the transition guard above.
      _detail::last_status_ref() = SNTP_SYNC_STATUS_RESET;
      _detail::last_adopt_ms_ref() = 0;  // re-arm
      NOTICEF("NTP: refresh requested after %us interval", (unsigned)cfg.interval_s);
    }
  }
}

inline bool is_running()        { return _detail::started_ref(); }
inline uint32_t last_adopt_ms() { return _detail::last_adopt_ms_ref(); }

}  // namespace Ntp
} // namespace Clock
