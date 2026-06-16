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
#include <stdint.h>
#include <stdio.h>

#include "../Common/OledText.h"
#include "../Clock/Manager.h"
#include "../Display/ScreenFramework.h"
#include "../Telemetry/Battery.h"
#include "Compass/QMC6310.h"
#include "Motion/QMI8658.h"
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
  // The GPS screen has two levels: the globe (the hero) with a single
  // status line, and a detail page with the numbers (sats, signal,
  // accuracy, RF health, schedules). POWER toggles between them; at the
  // globe, BOOT-hold exits; leaving the screen resets to the globe.
  inline bool& gps_detail_ref() { static bool v = false; return v; }
  inline void gps_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_PIN;
    *title = gps_detail_ref() ? "GPS INFO" : "GPS";
  }
  inline size_t gps_hints(const char** out, size_t max) {
    if (gps_detail_ref()) {
      if (max < 2) return 0;
      out[0] = "Hold POWER: globe";
      out[1] = "Hold BOOT: back";
      return 2;
    }
    if (max < 3) return root_hints(out, max);
    out[0] = "Tap BOOT: next";
    out[1] = "Hold BOOT: back";
    out[2] = "Hold POWER: info";
    return 3;
  }
  inline void gps_on_select() { gps_detail_ref() = !gps_detail_ref(); }
  inline bool gps_on_back() {
    if (gps_detail_ref()) { gps_detail_ref() = false; return true; }
    return false;
  }
  inline void gps_on_exit() { gps_detail_ref() = false; }
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

  // Globe level: the dithered Earth is the hero with one status line
  // beneath it - a position on a fix, otherwise "Locating" or the power
  // state. The numbers move to the detail level (POWER). While searching
  // it spins; on a fix it eases the device lat+lon to centre, drops a
  // pulsing pin, and the satellites in view orbit the rim.
  inline void gps_globe_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    using namespace PlanetView;
    const Gnss::Fix f = Gnss::last_fix();
    const uint32_t now = millis();

    // Animation tuning, per rendered frame (not per real-time unit, so
    // the feel tracks the frame rate the page renders at).
    constexpr float    GLOBE_EASE          = 0.15f;  // ease spin+tilt toward the fix
    constexpr float    SEARCH_SPIN_DEG     = 1.4f;   // free longitude spin while searching
    constexpr float    TILT_SETTLE         = 0.1f;   // ease tilt back to north-up
    constexpr int      SAT_PER_ORBIT       = 4;      // satellites per orbital ring
    constexpr int      SAT_ORBIT_MAX       = 8;      // two rings, so up to 8 shown
    constexpr uint32_t SAT_ORBIT_PERIOD_MS = 5000;   // one full orbit

    // Animation state, persisted across frames.
    static float    rot_deg   = 0.0f;     // longitude spin
    static float    tilt_deg  = 0.0f;     // latitude tilt (centres the fix)
    static bool     was_fixed = false;
    static uint32_t fixed_ms  = 0;        // when the current fix landed

    const int16_t cx = (int16_t)(area.width() / 2);
    const int16_t cy = (int16_t)((y_top + y_bottom) / 2 - 6);
    draw_stars(area, y_top, y_bottom, cx, cy, now);

    if (f.valid) {
      if (!was_fixed) { was_fixed = true; fixed_ms = now; }
      // Ease both rotations so the device sits at the centre of the
      // disc: longitude spin -> device lon, latitude tilt -> device
      // lat. Centred both ways, not just at the front meridian.
      float dlon = (float)f.longitude_deg - rot_deg;
      while (dlon > 180.0f)  dlon -= 360.0f;
      while (dlon < -180.0f) dlon += 360.0f;
      rot_deg  += dlon * GLOBE_EASE;
      tilt_deg += ((float)f.latitude_deg - tilt_deg) * GLOBE_EASE;
    } else {
      was_fixed = false;
      rot_deg += SEARCH_SPIN_DEG;        // free spin while searching
      tilt_deg += (0.0f - tilt_deg) * TILT_SETTLE;   // settle back to north-up
    }
    if (rot_deg >= 360.0f) rot_deg -= 360.0f;
    if (rot_deg < 0.0f)    rot_deg += 360.0f;
    const int roti  = (int)lrintf(rot_deg);
    const int tilti = (int)lrintf(tilt_deg);

    render_globe(area, cx, cy, roti, tilti);

    // Orbiting satellites: the ones actually being heard (signal
    // present), spread across two latitude bands - one over the northern
    // hemisphere, one over the southern - in the same orbital plane.
    // SAT_PER_ORBIT on each, SAT_ORBIT_MAX total. Each band is a tilted
    // ellipse offset up or down; a satellite is hidden only when behind
    // the sphere (depth < 0 AND its screen point is on the disc), so the
    // bands read as rings circling the globe.
    const float orbit_r = PLANET_R + 7;
    const float lat_cos  = 0.82f;            // band radius factor (~35 deg latitude)
    const float band_tilt_s = 0.30f;         // vertical squash of each band ellipse
    const float band_tilt_c = 0.95f;         // depth component (hide-behind)
    const float band_v = orbit_r * 0.42f;    // north/south offset from the centre
    auto draw_band = [&](int count, float center_y, float phase0) {
      for (int k = 0; k < count; ++k) {
        const float oa = (now % SAT_ORBIT_PERIOD_MS)
                         * (2.0f * 3.14159265f / (float)SAT_ORBIT_PERIOD_MS)
                         + phase0 + k * (2.0f * 3.14159265f / count);
        const float wx = orbit_r * lat_cos * cosf(oa);
        const float wy = orbit_r * lat_cos * sinf(oa) * band_tilt_s;
        const float wz = orbit_r * lat_cos * sinf(oa) * band_tilt_c;   // depth
        const int16_t sx = (int16_t)(cx + wx);
        const int16_t sy = (int16_t)(center_y - wy);
        const bool over_disc = (sx - cx) * (sx - cx) + (sy - cy) * (sy - cy)
                               < PLANET_R * PLANET_R;
        if (wz < 0.0f && over_disc) continue;                 // behind the globe
        if (sx < 2 || sx > area.width() - 3) continue;
        draw_satellite(area, sx, sy);
      }
    };
    const int n_total = f.sats_tracked > SAT_ORBIT_MAX ? SAT_ORBIT_MAX : f.sats_tracked;
    const int n1 = n_total > SAT_PER_ORBIT ? SAT_PER_ORBIT : n_total;
    const int n2 = n_total > SAT_PER_ORBIT ? n_total - SAT_PER_ORBIT : 0;
    draw_band(n1, (float)cy - band_v, 0.0f);    // northern band
    draw_band(n2, (float)cy + band_v, 0.6f);    // southern band, staggered

    // Marker: only when fixed and front-facing.
    if (f.valid) {
      int16_t mx, my;
      if (project(f.latitude_deg, f.longitude_deg, roti, tilti, cx, cy, mx, my)) {
        const int pulse = 2 + (int)((now - fixed_ms) / 60);
        draw_marker(area, mx, my, /*locked=*/true, pulse < 14 ? pulse : -1);
      }
    }

    // Two-line footer: the fix line (the position once fixed, otherwise
    // its state) and, below it, the satellite tally (heard vs used in the
    // solution). Each is centred by true pixel width (Picopixel is
    // proportional, so a strlen estimate clips) over a cleared black box
    // so a scattered star or low-orbiting satellite never bleeds in.
    auto draw_status = [&](int16_t ty, const char* s) {
      int16_t bx, by; uint16_t bw, bh;
      area.getTextBounds(s, 0, ty, &bx, &by, &bw, &bh);
      const int16_t tx = (int16_t)((area.width() - (int16_t)bw) / 2 - bx);
      area.fillRect((int16_t)(tx + bx - 1), (int16_t)(by - 1),
                    (int16_t)(bw + 2), (int16_t)(bh + 2), 0);
      Common::OledText::line_at(area, tx, ty, s);
    };
    const Gnss::PowerMode mode = Gnss::acq_status().mode;
    char fix_line[24];
    if (f.valid) {
      snprintf(fix_line, sizeof(fix_line), "%.2f%c %.2f%c",
               fabsf((float)f.latitude_deg),  f.latitude_deg  >= 0 ? 'N' : 'S',
               fabsf((float)f.longitude_deg), f.longitude_deg >= 0 ? 'E' : 'W');
    } else if (mode == Gnss::PowerMode::Off) {
      snprintf(fix_line, sizeof(fix_line), "Location off");
    } else {
      snprintf(fix_line, sizeof(fix_line), "No fix");
    }
    if (mode == Gnss::PowerMode::Off) {
      draw_status((int16_t)(y_bottom - 3), fix_line);
    } else {
      char sat_line[24];
      snprintf(sat_line, sizeof(sat_line), "Trk %u  Used %u",
               (unsigned)f.sats_tracked, (unsigned)f.sats);
      draw_status((int16_t)(y_bottom - 10), fix_line);
      draw_status((int16_t)(y_bottom - 3),  sat_line);
    }
  }

  // Detail level: the numbers behind the globe, reached with POWER.
  inline void gps_detail_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const Gnss::Fix f = Gnss::last_fix();
    const Gnss::AcqStatus a = Gnss::acq_status();
    (void)y_bottom;
    int16_t y = (int16_t)(y_top + 5);
    linef(area, y, "Seen %u  Used %u",
          (unsigned)f.sats_visible, (unsigned)f.sats); y += 8;
    linef(area, y, "Signal %u dB", (unsigned)f.best_snr_db); y += 8;
    if (f.acc_valid) {
      linef(area, y, "Acc H%.0f V%.0f m", f.hacc_m, f.vacc_m); y += 8;
    }
    // Age of the last *valid* fix, not the last sentence: the M10 emits
    // RMC ~1 Hz whether it has a fix or not, so fix_received_ms would
    // read "0s ago" forever while never actually fixing.
    if (f.last_valid_fix_ms == 0) {
      linef(area, y, "Fix: none yet"); y += 8;
    } else {
      char age[10];
      fmt_age(age, sizeof(age), f.last_valid_fix_ms);
      linef(area, y, "Fix %s ago", age); y += 8;
    }
    if (Gnss::module() == Gnss::Module::MAXM10) {
      const auto rf = MaxM10::rf_status();
      static const char* JAM[] = { "unknown", "none", "warning", "critical" };
      linef(area, y, "Jam: %s", JAM[rf.valid ? (rf.jamming_state & 0x03) : 0]); y += 8;
      linef(area, y, "Noise %u  AGC %u%%",
            (unsigned)(rf.valid ? rf.noise_per_ms : 0),
            (unsigned)(rf.valid ? (uint8_t)((uint32_t)rf.agc * 100 / 8191) : 0)); y += 8;
    }
    if (a.mode == Gnss::PowerMode::AlwaysOn) {
      linef(area, y, "Loc: always on"); y += 8;
    } else if (a.mode == Gnss::PowerMode::Pulsed) {
      linef(area, y, "Loc: every %lum",
            (unsigned long)(Gnss::power_config().interval_s / 60)); y += 8;
    } else {
      linef(area, y, "Loc: off"); y += 8;
    }
    const auto gc = Clock::Manager::get_config(Clock::Manager::Source::GPS);
    if (gc.interval_s == 0) linef(area, y, "Sync: at boot");
    else linef(area, y, "Sync: every %luh",
               (unsigned long)(gc.interval_s / 3600));
  }

  // Router: globe or detail.
  inline void gps_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    if (gps_detail_ref()) gps_detail_body(area, y_top, y_bottom);
    else                  gps_globe_body(area, y_top, y_bottom);
  }

  // ---- HEADING ----
  inline void heading_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_COMPASS;
    *title = "HEADING";
  }
  // Ship's compass: the card rotates so the direction you face is at
  // the top of the screen (the read point). Each mark sits at screen
  // bearing (mark - heading), so facing east brings E to the top and
  // swings N to the left. A filled wedge tags north on the card; the
  // exact bearing is printed below the ring.
  inline void heading_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
    const QMC6310::Reading r = QMC6310::last_reading();
    if (!r.valid) {
      Common::OledText::line(area, (int16_t)(y_top + 14), "No compass");
      Common::OledText::line(area, (int16_t)(y_top + 22), "reading yet");
      return;
    }
    const int16_t cx = (int16_t)(area.width() / 2);
    const int16_t R  = 27;
    const int16_t cy = (int16_t)(y_top + R + 6);   // ring up top, bearing below it
    const int16_t ny = (int16_t)(cy + R + 4);      // below-ring text row
    const float   D2R = 0.01745329f;

    area.drawCircle(cx, cy, R, 1);
    area.fillCircle(cx, cy, 1, 1);                 // pivot

    if (!r.cal_ready) {
      // Uncalibrated: no rotating card (the heading is not trustworthy
      // yet). Prompt and a fill-as-you-go progress bar sit below the
      // ring, clear of it.
      area.setFont(Common::OledText::OLED_FONT);
      Common::OledText::line_at(area, (int16_t)(cx - 15), (int16_t)(ny + 3), "turn + tilt");
      Common::OledText::line_at(area, (int16_t)(cx - 17), (int16_t)(ny + 10), "to calibrate");
      const int16_t bw = 44, bx = (int16_t)(cx - bw / 2), by = (int16_t)(ny + 14);
      area.drawRect(bx, by, bw, 4, 1);
      area.fillRect(bx, by, (int16_t)(bw * r.cal_progress), 4, 1);
      return;
    }

    // Calibrated: rotating card (cardinals labelled, intercardinals
    // ticked) with the bearing below the ring, so labels own the centre.
    const float hd = r.heading_deg;
    struct Mark { float ang; const char* lbl; };
    static const Mark marks[] = {
      {0, "N"}, {45, nullptr}, {90, "E"}, {135, nullptr},
      {180, "S"}, {225, nullptr}, {270, "W"}, {315, nullptr},
    };
    for (const Mark& m : marks) {
      const float phi = (m.ang - hd) * D2R;
      const float s = sinf(phi), c = cosf(phi);
      const int16_t tick = (int16_t)(m.lbl ? 5 : 3);
      area.drawLine((int16_t)(cx + s * R), (int16_t)(cy - c * R),
                    (int16_t)(cx + s * (R - tick)), (int16_t)(cy - c * (R - tick)), 1);
      if (m.lbl) {
        Common::OledText::line_at(area, (int16_t)(cx + s * (R - 9) - 1),
                                  (int16_t)(cy - c * (R - 9) + 2), m.lbl);
      }
    }
    // Static index arrow (the read point), floated inside the ring below
    // the cardinal lettering so it never collides with the top mark. It
    // points up at whichever mark the rotating card has brought to the
    // top, which is the current heading. Not part of the rotating card.
    area.fillTriangle(cx, (int16_t)(cy - R + 14),
                      (int16_t)(cx - 3), (int16_t)(cy - R + 19),
                      (int16_t)(cx + 3), (int16_t)(cy - R + 19), 1);
    // Big bearing below the ring, with a degree ring. Wrap the rounded
    // value so 359.5+ shows 0, not 360 (the bearing range is 0-359).
    int hdi = (int)lrintf(hd);
    if (hdi >= 360) hdi -= 360;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", hdi);
    area.setFont(nullptr);
    area.setTextSize(2);
    const int16_t bw = (int16_t)(strlen(buf) * 12);
    area.setCursor((int16_t)(cx - bw / 2), ny);
    area.print(buf);
    area.setTextSize(1);
    area.setFont(Common::OledText::OLED_FONT);
    area.drawCircle((int16_t)(cx + bw / 2 + 3), (int16_t)(ny + 1), 1, 1);
    // Corrected field magnitude, small, at the foot of the screen.
    char fbuf[12];
    snprintf(fbuf, sizeof(fbuf), "%.0f uT", r.field_uT);
    Common::OledText::line_at(area, (int16_t)(cx - (int)strlen(fbuf) * 2),
                              (int16_t)(y_bottom - 1), fbuf);
  }

  // ---- ENVIRONMENT ----
  // Battery / CPU move to a separate SYSTEM screen later; this screen
  // is the environment sensors only. The battery block is kept
  // commented below as a record of its layout for that future screen.
  inline void environment_header(const uint8_t** glyph, const char** title) {
    *glyph = Display::Screens::GLYPH_THERMO;
    *title = "CLIMATE";   // short enough to clear the header spinner
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
      area.setFont(Common::OledText::OLED_FONT);
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
      y += 10;
      // Pressure trend: a sparkline of the recent history, bars scaled
      // to the window's own min/max so even small weather swings show.
      float hist[BME280::PRESS_HIST_N];
      const int hn = BME280::pressure_history(hist, BME280::PRESS_HIST_N);
      const int16_t gtop = (int16_t)(y + 2);
      const int16_t gh   = (int16_t)(y_bottom - gtop - 1);
      if (hn >= 2 && gh > 5) {
        float pmin = hist[0], pmax = hist[0];
        for (int i = 1; i < hn; ++i) {
          if (hist[i] < pmin) pmin = hist[i];
          if (hist[i] > pmax) pmax = hist[i];
        }
        // A calm pressure must read as a calm low line, not noise blown
        // up to full height; PRESS_GRAPH_FULL_PA is the swing that fills
        // the chart, so anything smaller stays proportionally short.
        constexpr float PRESS_GRAPH_FULL_PA = 300.0f;   // 3 hPa fills the chart
        const float eff = (pmax - pmin) > PRESS_GRAPH_FULL_PA
                          ? (pmax - pmin) : PRESS_GRAPH_FULL_PA;
        const int16_t gx = 2, gw = (int16_t)(area.width() - 4);
        area.drawFastHLine(gx, (int16_t)(gtop + gh), gw, 1);   // baseline
        const float bwf = (float)gw / hn;
        for (int i = 0; i < hn; ++i) {
          const float frac = (hist[i] - pmin) / eff;
          const int16_t bh = (int16_t)(frac * (gh - 1)) + 1;
          const int16_t bx = (int16_t)(gx + i * bwf);
          const int16_t biw = bwf >= 2.0f ? (int16_t)(bwf - 1) : 1;
          area.fillRect(bx, (int16_t)(gtop + gh - bh), biw, bh, 1);
        }
      }
    } else {
      Common::OledText::line(area, (int16_t)(y + 6), "No env data");
    }

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
  inline void demand_compass() { QMC6310::request_live(); QMI8658::request_live(); }
  inline void demand_env()     { BME280::request_live(); }
  inline void recal_compass()  { QMC6310::reset_calibration(); }
  // Compass adds a POWER-hold recalibrate to the shared root hints.
  inline size_t compass_hints(const char** out, size_t max) {
    if (max < 3) return root_hints(out, max);
    out[0] = "Tap BOOT: next";
    out[1] = "Hold BOOT: back";
    out[2] = "Hold POWER: recal";
    return 3;
  }
}

// The three registered pages, in BOOT-tap rotation order.
inline const Display::Screens::ScreenPage GPS_PAGE = {
  _detail::gps_header, _detail::gps_hints, _detail::gps_body,
  _detail::noop, _detail::gps_on_exit, _detail::noop, _detail::gps_on_select,
  _detail::gps_on_back, _detail::gps_live, /*live_demand=*/nullptr, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage HEADING_PAGE = {
  _detail::heading_header, _detail::compass_hints, _detail::heading_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::recal_compass,
  _detail::at_root, _detail::always_live, _detail::demand_compass, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage ENVIRONMENT_PAGE = {
  _detail::environment_header, _detail::root_hints, _detail::environment_body,
  _detail::noop, _detail::noop, _detail::noop, _detail::noop,
  _detail::at_root, _detail::always_live, _detail::demand_env, /*ttl_ms=*/0,
};

}  // namespace View
}  // namespace Sensors
