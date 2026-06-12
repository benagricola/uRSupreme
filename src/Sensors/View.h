// Sensors screens: GPS, heading and system as ScreenFramework pages.
// Built for the "standing outside watching the GPS acquire" case -
// everything the web app's sensor popover shows, without the laptop.
// The framework owns chrome and navigation (BOOT cycles screens, BOOT
// hold exits; these pages have no internal levels and no POWER
// actions yet); the bodies here only render data.
//
// Pages:
//   GPS      module, location power state (always on / acquiring /
//            retry countdown / self-cycling), sats seen/used, fix +
//            age + position + accuracy, clock-sync state, RF health
//   HEADING  big magnetic heading + cardinal + needle (QMC6310)
//   SYSTEM   environment (BME280) + battery
#pragma once

#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <stdint.h>
#include <stdio.h>

#include "../Common/OledText.h"
#include "../Clock/Manager.h"
#include "../Display/ScreenFramework.h"
#include "../Telemetry/Battery.h"
#include "Compass/QMC6310.h"
#include "Environment/BME280.h"
#include "Position/Gnss.h"
#include "Position/MaxM10.h"

namespace Sensors {
namespace View {

namespace _detail {
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

  // Shared hints: these pages are roots with no POWER actions yet.
  inline size_t root_hints(const char** out, size_t max) {
    if (max < 2) return 0;
    out[0] = "Tap BOOT: next";
    out[1] = "Hold BOOT: back";
    return 2;
  }

  // ---- GPS ----
  inline void gps_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_PIN;
    *title = "GPS";
  }
  inline bool gps_live() {
    return Gnss::acq_status().mode != Gnss::PowerMode::Off;
  }
  inline void gps_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const Gnss::Fix f = Gnss::last_fix();
    const Gnss::AcqStatus a = Gnss::acq_status();
    const uint32_t now = millis();
    int16_t y = (int16_t)(y_top + 6);

    linef(area, y, "%s", Gnss::module_name()); y += 10;

    char t[16];
    switch (a.mode) {
      case Gnss::PowerMode::Off:
        Common::OledText::line(area, y, "Location off");
        break;
      case Gnss::PowerMode::AlwaysOn:
        Common::OledText::line(area, y, "Always on");
        break;
      default:  // Pulsed
        if (a.state == Gnss::PulseState::Acquiring) {
          fmt_mmss(t, sizeof(t), now - a.started_ms);
          linef(area, y, "Acquiring %s", t);
        } else if (a.m10 == Gnss::M10Power::Psmoo) {
          linef(area, y, "Auto %lum cycle",
                (unsigned long)(Gnss::power_config().interval_s / 60));
        } else if (a.next_attempt_ms != 0 && (int32_t)(a.next_attempt_ms - now) > 0) {
          fmt_mmss(t, sizeof(t), a.next_attempt_ms - now);
          linef(area, y, "Retry in %s", t);
        } else {
          Common::OledText::line(area, y, "Waiting");
        }
    }
    y += 8;

    linef(area, y, "Sats %u seen", (unsigned)f.sats_visible); y += 8;
    linef(area, y, "     %u used", (unsigned)f.sats); y += 10;

    char age[10];
    if (f.valid) {
      fmt_age(age, sizeof(age), a.last_fix_ms);
      linef(area, y, "Fix %s ago", age); y += 8;
      linef(area, y, "%.5f", f.latitude_deg); y += 8;
      linef(area, y, "%.5f", f.longitude_deg); y += 8;
      if (f.acc_valid && y < (int16_t)(y_bottom - 8)) {
        linef(area, y, "Within %.0fm", f.hacc_m); y += 8;
      }
    } else if (a.ever_fixed) {
      fmt_age(age, sizeof(age), a.last_fix_ms);
      Common::OledText::line(area, y, "Fix lost"); y += 8;
      linef(area, y, "last %s ago", age); y += 8;
    } else {
      Common::OledText::line(area, y, "No fix yet"); y += 8;
    }

    if ((int16_t)(y_bottom - y) > 28) {
      char cage[10];
      fmt_age(cage, sizeof(cage), a.last_clock_report_ms);
      y += 2;
      area.drawFastHLine(0, y, area.width(), 1); y += 8;
      linef(area, y, "Clock %s", cage); y += 8;
      linef(area, y, "via %s",
            Clock::Manager::source_name(Clock::Manager::current_source()));
      y += 8;
      if (Gnss::module() == Gnss::Module::MAXM10 && (int16_t)(y_bottom - y) > 6) {
        const MaxM10::RfStatus rf = MaxM10::rf_status();
        linef(area, y, "Gain %u%% jam %u",
              (unsigned)(rf.agc * 100UL / 8191UL), (unsigned)rf.cw_jam);
      }
    }
  }

  // ---- HEADING ----
  inline void heading_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_COMPASS;
    *title = "HEADING";
  }
  inline void heading_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const QMC6310::Reading r = QMC6310::last_reading();
    if (!r.valid) {
      Common::OledText::line(area, (int16_t)(y_top + 14), "No compass");
      Common::OledText::line(area, (int16_t)(y_top + 22), "reading yet");
      return;
    }
    // Big degrees in the classic font (2x), cardinal under it.
    char buf[16];
    snprintf(buf, sizeof(buf), "%3.0f", r.heading_deg);
    area.setFont(nullptr);
    area.setTextSize(2);
    area.setCursor(8, (int16_t)(y_top + 3));
    area.print(buf);
    area.setTextSize(1);
    area.setFont(&Picopixel);
    linef(area, (int16_t)(y_top + 28), "deg %s", cardinal(r.heading_deg));
    if ((int16_t)(y_bottom - y_top) > 78) {
      // Needle: circle centered below, line pointing toward north
      // relative to device-up.
      const int16_t cx = 32, cy = (int16_t)(y_top + 57), rad = 18;
      area.drawCircle(cx, cy, rad, 1);
      const float th = r.heading_deg * 3.14159265f / 180.0f;
      area.drawLine(cx, cy,
                    cx + (int16_t)(rad * sinf(-th)),
                    cy - (int16_t)(rad * cosf(-th)), 1);
      char age[10];
      fmt_age(age, sizeof(age), r.taken_ms);
      linef(area, (int16_t)(cy + rad + 10), "Read %s ago", age);
    }
  }

  // ---- SYSTEM ----
  inline void system_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_BATTERY;
    *title = "SYSTEM";
  }
  inline void system_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const BME280::Reading e = BME280::last_reading();
    const Telemetry::Battery::Snapshot b = Telemetry::Battery::current();
    int16_t y = (int16_t)(y_top + 6);
    if (e.valid) {
      linef(area, y, "Temp %.1fC", e.temp_c); y += 8;
      linef(area, y, "Humid %.0f%%", e.humidity_pct); y += 8;
      linef(area, y, "Press %.0fhPa", e.pressure_pa / 100.0f); y += 8;
    } else {
      Common::OledText::line(area, y, "No env data"); y += 8;
    }
    if (b.pmu_present && (int16_t)(y_bottom - y) > 20) {
      y += 2;
      area.drawFastHLine(0, y, area.width(), 1); y += 8;
      if (b.percent >= 0) { linef(area, y, "Batt %d%%", b.percent); y += 8; }
      linef(area, y, "%.2fV", b.voltage_v); y += 8;
      linef(area, y, "%s",
            b.state == Telemetry::Battery::State::Charging ? "Charging" :
            b.state == Telemetry::Battery::State::Discharging ? "On battery" : "Powered");
      y += 8;
      if (b.vbus_present && y < y_bottom) linef(area, y, "USB power in");
    }
  }

  inline void noop() {}
  inline bool at_root() { return false; }   // no internal levels: back exits
  inline bool not_live() { return false; }
}

// The three registered pages, in BOOT-tap rotation order.
inline const Display::Screens::ScreenPage GPS_PAGE = {
  _detail::gps_header, _detail::root_hints, _detail::gps_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::gps_live, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage HEADING_PAGE = {
  _detail::heading_header, _detail::root_hints, _detail::heading_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::not_live, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage SYSTEM_PAGE = {
  _detail::system_header, _detail::root_hints, _detail::system_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::not_live, /*ttl_ms=*/0,
};

}  // namespace View
}  // namespace Sensors
