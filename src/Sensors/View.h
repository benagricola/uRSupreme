// On-device sensors view: full-screen pages for GPS, heading and
// environment, navigated with the two buttons. Built for the
// "standing outside watching the GPS acquire" case: everything the
// web app's sensor popover shows, without the laptop.
//
// Entry: power-key LONG press (the AXP2101's own ~1 s threshold) from
// anywhere except the messenger or firmware update. The short press
// keeps its messenger role untouched.
// Inside: user button short = next page; user button hold (700 ms to
// 5 s) or power-key short = exit. Presses past 5 s exit and fall
// through so pairing and the console stay reachable (same contract as
// the messenger). The display stays awake while the view is up - the
// whole point is watching it.
//
// Pages:
//   Gps      module, location power state (always on / acquiring /
//            retry countdown / self-cycling), sats seen/used, fix +
//            age + position + accuracy, clock-sync state, RF health
//   Heading  big magnetic heading + cardinal + needle (QMC6310)
//   System   environment (BME280) + battery
#pragma once

#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <stdint.h>
#include <stdio.h>

#include "../Common/OledText.h"
#include "../Clock/Manager.h"
#include "../Telemetry/Battery.h"
#include "Compass/QMC6310.h"
#include "Environment/BME280.h"
#include "Position/Gnss.h"
#include "Position/MaxM10.h"

namespace Sensors {
namespace View {

// Mirrors the messenger's gesture boundary: below this a user-button
// press is "next", above it (to the global 5 s tier) it is "back/exit".
inline constexpr unsigned long HOLD_THRESHOLD_MS = 700;

enum class Page : uint8_t { Hidden, Gps, Heading, System };

namespace _detail {
  inline Page& page_ref() { static Page p = Page::Hidden; return p; }

  // One row of Picopixel text from a printf format.
  inline void linef(GFXcanvas1& area, int16_t y, const char* fmt, ...) {
    char buf[48];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Common::OledText::line(area, y, buf);
  }

  // "m:ss" from a millisecond duration, clamped for display.
  inline void fmt_mmss(char* out, size_t n, uint32_t ms) {
    uint32_t s = ms / 1000UL;
    if (s > 5999) s = 5999;
    snprintf(out, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
  }

  // Compact age: "3s" / "2m" / "4h" / "never" (0 sentinel).
  inline void fmt_age(char* out, size_t n, uint32_t at_ms) {
    if (at_ms == 0) { snprintf(out, n, "never"); return; }
    const uint32_t d = millis() - at_ms;
    if      (d < 60000UL)    snprintf(out, n, "%lus", (unsigned long)(d / 1000UL));
    else if (d < 3600000UL)  snprintf(out, n, "%lum", (unsigned long)(d / 60000UL));
    else                     snprintf(out, n, "%luh", (unsigned long)(d / 3600000UL));
  }

  inline const char* cardinal(float deg) {
    static const char* names[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    int idx = (int)((deg + 22.5f) / 45.0f) % 8;
    if (idx < 0) idx += 8;
    return names[idx];
  }

  // All pages are laid out for the portrait full-panel canvas
  // (64 px wide, 128 px tall): one fact per 8 px Picopixel row, about
  // 13 characters across. On shorter canvases (the 64 px landscape
  // split) the lower rows are gated on height like the messenger does.
  inline void render_gps(GFXcanvas1& area) {
    using namespace Common;
    const Gnss::Fix f = Gnss::last_fix();
    const Gnss::AcqStatus a = Gnss::acq_status();
    const uint32_t now = millis();
    const int16_t h = area.height();

    linef(area, 7, "GPS %s", Gnss::module_name());
    area.drawFastHLine(0, 10, area.width(), 1);

    char t[16];
    switch (a.mode) {
      case Gnss::PowerMode::Off:
        OledText::line(area, 19, "Location off");
        break;
      case Gnss::PowerMode::AlwaysOn:
        OledText::line(area, 19, "Always on");
        break;
      default:  // Pulsed
        if (a.state == Gnss::PulseState::Acquiring) {
          fmt_mmss(t, sizeof(t), now - a.started_ms);
          linef(area, 19, "Acquiring %s", t);
        } else if (a.m10 == Gnss::M10Power::Psmoo) {
          linef(area, 19, "Auto %lum cycle",
                (unsigned long)(Gnss::power_config().interval_s / 60));
        } else if (a.next_attempt_ms != 0 && (int32_t)(a.next_attempt_ms - now) > 0) {
          fmt_mmss(t, sizeof(t), a.next_attempt_ms - now);
          linef(area, 19, "Retry in %s", t);
        } else {
          OledText::line(area, 19, "Waiting");
        }
    }

    linef(area, 27, "Sats %u seen", (unsigned)f.sats_visible);
    linef(area, 35, "     %u used", (unsigned)f.sats);

    char age[10];
    if (f.valid) {
      fmt_age(age, sizeof(age), a.last_fix_ms);
      linef(area, 45, "Fix %s ago", age);
      linef(area, 53, "%.5f", f.latitude_deg);
      linef(area, 61, "%.5f", f.longitude_deg);
      if (h > 72 && f.acc_valid) linef(area, 69, "Within %.0fm", f.hacc_m);
    } else if (a.ever_fixed) {
      fmt_age(age, sizeof(age), a.last_fix_ms);
      linef(area, 45, "Fix lost");
      linef(area, 53, "last %s ago", age);
    } else {
      OledText::line(area, 45, "No fix yet");
    }

    if (h > 90) {
      char cage[10];
      fmt_age(cage, sizeof(cage), a.last_clock_report_ms);
      area.drawFastHLine(0, 78, area.width(), 1);
      linef(area, 86, "Clock %s", cage);
      linef(area, 94, "via %s",
            Clock::Manager::source_name(Clock::Manager::current_source()));
      if (Gnss::module() == Gnss::Module::MAXM10) {
        const MaxM10::RfStatus rf = MaxM10::rf_status();
        linef(area, 102, "Gain %u%% jam %u",
              (unsigned)(rf.agc * 100UL / 8191UL), (unsigned)rf.cw_jam);
      }
    }
    OledText::hints(area, {"BOOT next", "hold exits"});
  }

  inline void render_heading(GFXcanvas1& area) {
    using namespace Common;
    const QMC6310::Reading r = QMC6310::last_reading();
    const int16_t h = area.height();
    linef(area, 7, "HEADING");
    area.drawFastHLine(0, 10, area.width(), 1);
    if (!r.valid) {
      OledText::line(area, 27, "No compass");
      OledText::line(area, 35, "reading yet");
    } else {
      // Big degrees in the classic font (2x), cardinal under it.
      char buf[16];
      snprintf(buf, sizeof(buf), "%3.0f", r.heading_deg);
      area.setFont(nullptr);
      area.setTextSize(2);
      area.setCursor(8, 16);
      area.print(buf);
      area.setTextSize(1);
      area.setFont(&Picopixel);
      linef(area, 41, "deg %s", cardinal(r.heading_deg));
      if (h > 90) {
        // Needle: circle centered below, line pointing toward north
        // relative to device-up.
        const int16_t cx = 32, cy = 70, rad = 18;
        area.drawCircle(cx, cy, rad, 1);
        const float th = r.heading_deg * 3.14159265f / 180.0f;
        area.drawLine(cx, cy,
                      cx + (int16_t)(rad * sinf(-th)),
                      cy - (int16_t)(rad * cosf(-th)), 1);
        char age[10];
        fmt_age(age, sizeof(age), r.taken_ms);
        linef(area, 100, "Read %s ago", age);
      }
    }
    OledText::hints(area, {"BOOT next", "hold exits"});
  }

  inline void render_system(GFXcanvas1& area) {
    using namespace Common;
    const BME280::Reading e = BME280::last_reading();
    const Telemetry::Battery::Snapshot b = Telemetry::Battery::current();
    const int16_t h = area.height();
    linef(area, 7, "SYSTEM");
    area.drawFastHLine(0, 10, area.width(), 1);
    if (e.valid) {
      linef(area, 19, "Temp %.1fC", e.temp_c);
      linef(area, 27, "Humid %.0f%%", e.humidity_pct);
      linef(area, 35, "Press %.0fhPa", e.pressure_pa / 100.0f);
    } else {
      OledText::line(area, 19, "No env data");
    }
    if (b.pmu_present && h > 60) {
      area.drawFastHLine(0, 44, area.width(), 1);
      if (b.percent >= 0) linef(area, 52, "Batt %d%%", b.percent);
      linef(area, 60, "%.2fV", b.voltage_v);
      linef(area, 68, "%s",
            b.state == Telemetry::Battery::State::Charging ? "Charging" :
            b.state == Telemetry::Battery::State::Discharging ? "On battery" : "Powered");
      if (b.vbus_present) linef(area, 76, "USB power in");
    }
    OledText::hints(area, {"BOOT next", "hold exits"});
  }
}

inline bool active() { return _detail::page_ref() != Page::Hidden; }

// Stable page label for the display diag endpoint.
inline const char* page_name() {
  switch (_detail::page_ref()) {
    case Page::Hidden:  return "hidden";
    case Page::Gps:     return "gps";
    case Page::Heading: return "heading";
    case Page::System:  return "system";
  }
  return "?";
}

inline void open() { _detail::page_ref() = Page::Gps; }
inline void exit_mode() { _detail::page_ref() = Page::Hidden; }

// User button inside the view: short = next page, hold = exit.
inline void on_user_button(unsigned long duration_ms) {
  if (duration_ms > HOLD_THRESHOLD_MS) { exit_mode(); return; }
  switch (_detail::page_ref()) {
    case Page::Gps:     _detail::page_ref() = Page::Heading; break;
    case Page::Heading: _detail::page_ref() = Page::System;  break;
    case Page::System:  _detail::page_ref() = Page::Gps;     break;
    default: break;
  }
}

// Power-key short press inside the view exits it.
inline void on_power_key() { exit_mode(); }

inline void render(GFXcanvas1& area) {
  area.fillRect(0, 0, area.width(), area.height(), 0 /*black*/);
  area.setFont(&Picopixel);
  area.setTextWrap(false);
  area.setTextColor(1 /*white*/);
  area.setTextSize(1);
  switch (_detail::page_ref()) {
    case Page::Gps:     _detail::render_gps(area);     break;
    case Page::Heading: _detail::render_heading(area); break;
    case Page::System:  _detail::render_system(area);  break;
    default: break;
  }
}

}  // namespace View
}  // namespace Sensors
