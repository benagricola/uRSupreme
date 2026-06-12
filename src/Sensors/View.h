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
//   ENVIRONMENT  temperature + humidity + pressure (BME280)
//                (battery/CPU move to a future SYSTEM screen)
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
    static float    rot_deg   = 0.0f;     // longitude spin
    static float    tilt_deg  = 0.0f;     // latitude tilt (centres the fix)
    static bool     was_fixed = false;
    static uint32_t fixed_ms  = 0;        // when the current fix landed

    const int16_t cx = (int16_t)(area.width() / 2);
    const int16_t cy = (int16_t)(y_top + PLANET_R + 2);
    draw_stars(area, y_top, y_bottom, cx, cy, now);

    if (f.valid) {
      if (!was_fixed) { was_fixed = true; fixed_ms = now; }
      // Ease both rotations so the device sits at the centre of the
      // disc: longitude spin -> device lon, latitude tilt -> device
      // lat. Centred both ways, not just at the front meridian.
      float dlon = (float)f.longitude_deg - rot_deg;
      while (dlon > 180.0f)  dlon -= 360.0f;
      while (dlon < -180.0f) dlon += 360.0f;
      rot_deg  += dlon * 0.15f;
      tilt_deg += ((float)f.latitude_deg - tilt_deg) * 0.15f;
    } else {
      was_fixed = false;
      rot_deg += 1.4f;                    // free spin while searching
      tilt_deg += (0.0f - tilt_deg) * 0.1f;   // settle back to north-up
    }
    if (rot_deg >= 360.0f) rot_deg -= 360.0f;
    if (rot_deg < 0.0f)    rot_deg += 360.0f;
    const int roti  = (int)lrintf(rot_deg);
    const int tilti = (int)lrintf(tilt_deg);

    render_globe(area, cx, cy, roti, tilti);

    // Orbiting satellites: one per satellite in view, capped at 4, each
    // on its own phase. A proper tilted 3D ring - the satellite is
    // hidden only when genuinely behind the sphere (depth < 0 AND its
    // screen point falls on the disc); it stays visible all the way
    // round the sides, where the ring is wider than the planet.
    const int n_orbit = f.sats_visible >= 4 ? 4 : f.sats_visible;
    const float orbit_r = PLANET_R + 7;
    const float orbit_tilt_s = 0.45f, orbit_tilt_c = 0.89f;   // ~27 deg
    for (int k = 0; k < n_orbit; ++k) {
      const float oa = (now % 5000) * (2.0f * 3.14159265f / 5000.0f)
                       + k * (2.0f * 3.14159265f / n_orbit);
      const float wx = orbit_r * cosf(oa);
      const float wy = orbit_r * sinf(oa) * orbit_tilt_s;   // vertical tilt
      const float wz = orbit_r * sinf(oa) * orbit_tilt_c;   // depth
      const int16_t sx = (int16_t)(cx + wx);
      const int16_t sy = (int16_t)(cy - wy);
      const bool over_disc = (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)
                             < PLANET_R * PLANET_R;
      if (wz < 0.0f && over_disc) continue;                 // behind the globe
      if (sx < 3 || sx > area.width() - 4) continue;
      draw_satellite(area, sx, sy);
    }

    // Marker: only when fixed and front-facing.
    if (f.valid) {
      int16_t mx, my;
      if (project(f.latitude_deg, f.longitude_deg, roti, tilti, cx, cy, mx, my)) {
        const int pulse = 2 + (int)((now - fixed_ms) / 60);
        draw_marker(area, mx, my, /*locked=*/true, pulse < 14 ? pulse : -1);
      }
    }

    // Text rows below the globe.
    char buf[24];
    const int16_t ty = (int16_t)(cy + PLANET_R + 6);
    if (f.valid) {
      snprintf(buf, sizeof(buf), "%u seen  %u used",
               (unsigned)f.sats_visible, (unsigned)f.sats);
      Common::OledText::line(area, ty, buf);
      snprintf(buf, sizeof(buf), "%.4f %.4f", f.latitude_deg, f.longitude_deg);
      area.setFont(nullptr);
      area.setCursor(2, (int16_t)(ty + 8));
      area.print(buf);
      area.setFont(&Picopixel);
    } else {
      Common::OledText::line(area, ty, "Locating");
      snprintf(buf, sizeof(buf), "%u sats seen", (unsigned)f.sats_visible);
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

  // ---- ENVIRONMENT ----
  // Battery / CPU move to a separate SYSTEM screen later; this screen
  // is the environment sensors only. The battery block is kept
  // commented below as a record of its layout for that future screen.
  inline void environment_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_THERMO;
    *title = "ENVIRONMENT";
  }
  inline void environment_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const BME280::Reading e = BME280::last_reading();
    char buf[20];
    int16_t y = (int16_t)(y_top + 3);

    if (e.valid) {
      // Temperature in the big font from the left edge (the header
      // already carries the thermometer glyph), with a degree-C unit
      // that fits to its right.
      area.setFont(nullptr);
      area.setTextSize(2);
      snprintf(buf, sizeof(buf), "%.1f", e.temp_c);
      area.setCursor(2, y); area.print(buf);
      area.setTextSize(1);
      area.setFont(&Picopixel);
      int16_t ux = (int16_t)(2 + (int)strlen(buf) * 12 + 1);
      area.drawCircle(ux, (int16_t)(y + 1), 1, 1);
      Common::OledText::line_at(area, (int16_t)(ux + 3), (int16_t)(y + 5), "C");
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
    (void)y_bottom;

    // --- Battery block (moves to a future SYSTEM screen) ---------------
    // const Telemetry::Battery::Snapshot b = Telemetry::Battery::current();
    // if (b.pmu_present && (int16_t)(y_bottom - y) > 26) {
    //   y += 2;
    //   area.drawFastHLine(0, y, area.width(), 1);
    //   y += 4;
    //   const int16_t bw = 40, bh = 12;
    //   area.drawRect(2, y, bw, bh, 1);
    //   area.fillRect((int16_t)(2 + bw), (int16_t)(y + 3), 3, (int16_t)(bh - 6), 1);
    //   if (b.percent >= 0) {
    //     area.fillRect(4, (int16_t)(y + 2),
    //                   (int16_t)((bw - 4) * b.percent / 100), (int16_t)(bh - 4), 1);
    //     char pbuf[8]; snprintf(pbuf, sizeof(pbuf), "%d%%", b.percent);
    //     Common::OledText::line_at(area, 48, (int16_t)(y + 9), pbuf);
    //   }
    //   y += (int16_t)(bh + 8);
    //   char vbuf[20];
    //   snprintf(vbuf, sizeof(vbuf), "%.2fV %s", b.voltage_v,
    //            b.state == Telemetry::Battery::State::Charging ? "charging" :
    //            b.state == Telemetry::Battery::State::Discharging ? "on battery" : "powered");
    //   Common::OledText::line(area, y, vbuf);
    // }
  }

  inline void noop() {}
  inline bool at_root() { return false; }   // no internal levels: back exits
  inline bool not_live() { return false; }
  inline bool always_live() { return true; }
  inline void demand_compass() { QMC6310::request_live(); }
  inline void demand_env()     { BME280::request_live(); }
}

// The three registered pages, in BOOT-tap rotation order.
inline const Display::Screens::ScreenPage GPS_PAGE = {
  _detail::gps_header, _detail::root_hints, _detail::gps_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::gps_live, /*live_demand=*/nullptr, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage HEADING_PAGE = {
  _detail::heading_header, _detail::root_hints, _detail::heading_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::always_live, _detail::demand_compass, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage ENVIRONMENT_PAGE = {
  _detail::environment_header, _detail::root_hints, _detail::environment_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::always_live, _detail::demand_env, /*ttl_ms=*/0,
};

}  // namespace View
}  // namespace Sensors
