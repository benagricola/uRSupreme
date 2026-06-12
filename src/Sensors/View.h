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
#include "PlanetView.h"

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
  // Signal-strength bars: one bar per ~2 satellites in view, capped
  // at five, lit while in view. Drawn from (x,y) growing rightwards.
  inline void signal_bars(GFXcanvas1& area, int16_t x, int16_t base_y,
                          uint8_t sats_visible) {
    const int lit = sats_visible >= 10 ? 5 : (sats_visible + 1) / 2;
    for (int i = 0; i < 5; ++i) {
      const int16_t bh = (int16_t)(4 + i * 3);
      const int16_t bx = (int16_t)(x + i * 8);
      const int16_t by = (int16_t)(base_y - bh);
      if (i < lit) area.fillRect(bx, by, 6, bh, 1);
      else         area.drawRect(bx, by, 6, bh, 1);
    }
  }

  // The GPS screen adapts to its state: searching makes the signal
  // meter the hero (what you watch on the balcony), a fix makes the
  // coordinates the hero. The clock-sync detail is intentionally
  // absent - it is not what someone holding the device wants here.
  // The GPS screen is a dithered globe with the device's position
  // marked on its surface (PlanetView). While searching it spins and
  // the marker is hidden; on a fix it eases the device longitude to
  // the front, drops a pin with a pulse, and holds there with the
  // satellite orbiting for liveness.
  inline void gps_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    using namespace PlanetView;
    const Gnss::Fix f = Gnss::last_fix();
    const Gnss::AcqStatus a = Gnss::acq_status();
    const uint32_t now = millis();

    // Animation state, persisted across frames.
    static float    rot_deg   = 0.0f;     // current globe longitude rotation
    static bool     was_fixed = false;
    static uint32_t fixed_ms  = 0;        // when the current fix landed

    const int16_t cx = (int16_t)(area.width() / 2);
    const int16_t cy = (int16_t)(y_top + PLANET_R + 2);
    draw_stars(area, y_top, y_bottom, cx, cy, now);

    if (f.valid) {
      if (!was_fixed) { was_fixed = true; fixed_ms = now; }
      // Ease the rotation so the device longitude faces front (front =
      // lon - rot == 0, i.e. rot -> device lon).
      float target = (float)f.longitude_deg;
      float diff = target - rot_deg;
      while (diff > 180.0f)  diff -= 360.0f;
      while (diff < -180.0f) diff += 360.0f;
      rot_deg += diff * 0.15f;            // critically-damped-ish ease
    } else {
      was_fixed = false;
      rot_deg += 1.4f;                    // free spin while searching
    }
    if (rot_deg >= 360.0f) rot_deg -= 360.0f;
    if (rot_deg < 0.0f)    rot_deg += 360.0f;
    const int roti = (int)lrintf(rot_deg);

    render_globe(area, cx, cy, roti);

    // Satellite on a constrained orbit (stays in the body, ducks
    // behind the globe). Phase from millis so it animates frame to
    // frame regardless of redraw cadence.
    const float oa = (now % 4000) * (2.0f * 3.14159265f / 4000.0f);
    const int16_t ox = (int16_t)(cx + (PLANET_R + 6) * cosf(oa));
    const int16_t oy = (int16_t)(cy + (PLANET_R - 4) * 0.6f * sinf(oa));
    const bool behind = (cosf(oa) < 0.0f) && (abs(oy - cy) < PLANET_R);
    if (!behind) {
      int16_t sx = ox; if (sx < 3) sx = 3; if (sx > area.width() - 4) sx = (int16_t)(area.width() - 4);
      draw_satellite(area, sx, oy);
    }

    // Marker: only when fixed and front-facing.
    if (f.valid) {
      int16_t mx, my;
      if (project(f.latitude_deg, f.longitude_deg, roti, cx, cy, mx, my)) {
        const int pulse = 2 + (int)((now - fixed_ms) / 60);
        draw_marker(area, mx, my, /*locked=*/true, pulse < 14 ? pulse : -1);
      }
    }

    // Text rows below the globe.
    char buf[24];
    const int16_t ty = (int16_t)(cy + PLANET_R + 6);
    if (f.valid) {
      Common::OledText::line(area, ty, "FIX");
      snprintf(buf, sizeof(buf), "%.4f %.4f", f.latitude_deg, f.longitude_deg);
      area.setFont(nullptr);
      area.setCursor(2, (int16_t)(ty + 8));
      area.print(buf);
      area.setFont(&Picopixel);
    } else {
      Common::OledText::line(area, ty, "Locating");
      snprintf(buf, sizeof(buf), "%u sats", (unsigned)f.sats_visible);
      Common::OledText::line(area, (int16_t)(ty + 8), buf);
    }
    // Quiet power-mode row at the very bottom.
    const char* mode_str = "Location off";
    if (a.mode == Gnss::PowerMode::AlwaysOn) mode_str = "Always on";
    else if (a.mode == Gnss::PowerMode::Pulsed) {
      static char mbuf[20];
      snprintf(mbuf, sizeof(mbuf), "Auto, %lum cycle",
               (unsigned long)(Gnss::power_config().interval_s / 60));
      mode_str = mbuf;
    }
    Common::OledText::line(area, (int16_t)(y_bottom - 4), mode_str);
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
      const int16_t cx = 32, cy = (int16_t)(y_top + 57), rad = 20;
      area.drawCircle(cx, cy, rad, 1);
      // N marker outside the rim.
      area.setCursor((int16_t)(cx - 1), (int16_t)(cy - rad - 3));
      area.print("N");
      // Filled needle: triangle from two base points perpendicular to
      // the north direction, pointing where north is from device-up.
      const float th = r.heading_deg * 3.14159265f / 180.0f;
      const float nx = sinf(-th), ny = -cosf(-th);
      const int16_t tipx = (int16_t)(cx + (rad - 3) * nx);
      const int16_t tipy = (int16_t)(cy + (rad - 3) * ny);
      const int16_t blx  = (int16_t)(cx - 4 * ny), bly = (int16_t)(cy + 4 * nx);
      const int16_t brx  = (int16_t)(cx + 4 * ny), bry = (int16_t)(cy - 4 * nx);
      area.fillTriangle(tipx, tipy, blx, bly, brx, bry, 1);
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
    char buf[20];
    int16_t y = (int16_t)(y_top + 3);

    if (e.valid) {
      // Temperature: thermometer glyph + the value in the big font.
      area.drawBitmap(2, y, Display::Screens::GLYPH_THERMO, 8, 8, 1);
      area.setFont(nullptr);
      area.setTextSize(2);
      snprintf(buf, sizeof(buf), "%.1f", e.temp_c);
      area.setCursor(14, y); area.print(buf);
      area.setTextSize(1);
      area.setFont(&Picopixel);
      y += 20;
      // Humidity bar: labelled gauge, fill = percent.
      snprintf(buf, sizeof(buf), "RH %.0f%%", e.humidity_pct);
      Common::OledText::line(area, (int16_t)(y + 6), buf);
      area.drawRect(2, (int16_t)(y + 9), 60, 5, 1);
      area.fillRect(2, (int16_t)(y + 9),
                    (int16_t)(60.0f * e.humidity_pct / 100.0f), 5, 1);
      y += 18;
      snprintf(buf, sizeof(buf), "%.0f hPa", e.pressure_pa / 100.0f);
      Common::OledText::line(area, (int16_t)(y + 4), buf);
      y += 12;
    } else {
      Common::OledText::line(area, (int16_t)(y + 6), "No env data");
      y += 14;
    }

    if (b.pmu_present && (int16_t)(y_bottom - y) > 26) {
      y += 2;
      area.drawFastHLine(0, y, area.width(), 1);
      y += 4;
      // Battery gauge: outline + nub, fill proportional to percent.
      const int16_t bw = 40, bh = 12;
      area.drawRect(2, y, bw, bh, 1);
      area.fillRect((int16_t)(2 + bw), (int16_t)(y + 3), 3, (int16_t)(bh - 6), 1);
      if (b.percent >= 0) {
        area.fillRect(4, (int16_t)(y + 2),
                      (int16_t)((bw - 4) * b.percent / 100), (int16_t)(bh - 4), 1);
        snprintf(buf, sizeof(buf), "%d%%", b.percent);
        // Past the gauge body (x=2..45) with clearance.
        Common::OledText::line_at(area, 48, (int16_t)(y + 9), buf);
      }
      y += (int16_t)(bh + 8);
      snprintf(buf, sizeof(buf), "%.2fV %s", b.voltage_v,
               b.state == Telemetry::Battery::State::Charging ? "charging" :
               b.state == Telemetry::Battery::State::Discharging ? "on battery" : "powered");
      Common::OledText::line(area, y, buf);
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
