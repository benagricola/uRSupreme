// Dithered globe renderer for the GPS screen: a shaded sphere with
// recognisable continents, an orbiting satellite, and a marker dropped
// on the surface at the device's real position. The expensive
// per-pixel geometry (surface normal, Lambert shading, and the base
// latitude/longitude of each pixel) depends only on pixel position,
// not on rotation, so it is precomputed once into PSRAM tables; each
// frame only adds the rotation, tests the continent mask, and dithers.
// No per-frame trig over the disc.
//
// Future: the same screen is the zoomed-out end of a map view that
// zooms into SD-card vector tiles for a local map (see issue #3).
#pragma once

#include <Adafruit_GFX.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <stdint.h>

#include "LandMask.h"

namespace Sensors {
namespace PlanetView {

inline constexpr int PLANET_R = 23;             // sphere radius, px
inline constexpr int BB       = 2 * PLANET_R + 1;   // table bounding box
inline constexpr float RAD2DEG = 57.29578f;

// Continents come from the real Natural Earth land mask
// (Sensors::LandMask, ~2 KB in flash); is_land samples it by lat/lon.
inline bool is_land(int lat_d, int lon_d) { return LandMask::is_land(lat_d, lon_d); }

// Ordered-dither matrix, 4x4.
inline const uint8_t BAYER[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};

namespace _d {
  inline uint8_t*&  shade()  { static uint8_t*  p = nullptr; return p; }  // 0..15, 0xFF outside
  inline int8_t*&   lat()    { static int8_t*   p = nullptr; return p; }  // degrees
  inline int16_t*&  lonb()   { static int16_t*  p = nullptr; return p; }  // base longitude, deg
  inline bool&      built()  { static bool b = false; return b; }

  inline void ensure() {
    if (built()) return;
    const size_t n = (size_t)BB * BB;
    shade() = (uint8_t*) heap_caps_malloc(n,     MALLOC_CAP_SPIRAM);
    lat()   = (int8_t*)  heap_caps_malloc(n,     MALLOC_CAP_SPIRAM);
    lonb()  = (int16_t*) heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM);
    if (!shade() || !lat() || !lonb()) return;   // OOM: render() falls back
    // Light from upper-left toward the viewer.
    float lx = -0.5f, ly = -0.55f, lz = 0.66f;
    const float ln = sqrtf(lx*lx + ly*ly + lz*lz);
    lx /= ln; ly /= ln; lz /= ln;
    for (int dy = -PLANET_R; dy <= PLANET_R; ++dy) {
      for (int dx = -PLANET_R; dx <= PLANET_R; ++dx) {
        const size_t i = (size_t)(dy + PLANET_R) * BB + (dx + PLANET_R);
        const float nx = (float)dx / PLANET_R, ny = (float)dy / PLANET_R;
        const float d2 = nx*nx + ny*ny;
        if (d2 > 1.0f) { shade()[i] = 0xFF; lat()[i] = 0; lonb()[i] = 0; continue; }
        const float nz = sqrtf(1.0f - d2);
        float diff = nx*lx + ny*ly + nz*lz; if (diff < 0) diff = 0;
        const float sh = 0.15f + 0.85f * diff;
        shade()[i] = (uint8_t)(sh * 15.0f + 0.5f);
        lat()[i]   = (int8_t)lrintf(asinf(-ny) * RAD2DEG);   // north up
        lonb()[i]  = (int16_t)lrintf(atan2f(nx, nz) * RAD2DEG);
      }
    }
    built() = true;
  }
}

// Project a (lat, lon) surface point at rotation rot_deg to screen,
// returning whether it is front-facing (nz > 0) plus the pixel.
inline bool project(double lat_d, double lon_d, int rot_deg,
                    int16_t cx, int16_t cy, int16_t& ox, int16_t& oy) {
  const float la = (float)lat_d / RAD2DEG;
  const float lo = ((float)lon_d - rot_deg) / RAD2DEG;
  const float x = cosf(la) * sinf(lo);
  const float y = sinf(la);
  const float z = cosf(la) * cosf(lo);
  ox = (int16_t)lrintf(cx + x * PLANET_R);
  oy = (int16_t)lrintf(cy - y * PLANET_R);   // north up
  return z > 0.05f;
}

// Draw the shaded globe at (cx, cy) rotated rot_deg about its axis.
inline void render_globe(GFXcanvas1& area, int16_t cx, int16_t cy, int rot_deg) {
  _d::ensure();
  if (!_d::built()) { area.drawCircle(cx, cy, PLANET_R, 1); return; }
  for (int dy = -PLANET_R; dy <= PLANET_R; ++dy) {
    const int16_t py = cy + dy;
    for (int dx = -PLANET_R; dx <= PLANET_R; ++dx) {
      const size_t i = (size_t)(dy + PLANET_R) * BB + (dx + PLANET_R);
      const uint8_t sh = _d::shade()[i];
      if (sh == 0xFF) continue;
      const int16_t px = cx + dx;
      const int lon = _d::lonb()[i] + rot_deg;
      const bool land = is_land(_d::lat()[i], lon);
      const float shf  = sh / 15.0f;
      const float thrf = (BAYER[((py & 3) << 2) | (px & 3)] + 0.5f) / 16.0f;
      // Land reads as solid on the lit hemisphere; ocean is a sparse
      // dither only where brightest. The contrast makes continents pop.
      const bool lit = land ? (shf > 0.33f || shf > 0.9f * thrf)
                            : (shf > 0.62f + 0.35f * thrf);
      if (lit) area.drawPixel(px, py, 1);
    }
  }
  // Crisp rim.
  area.drawCircle(cx, cy, PLANET_R, 1);
}

// Marker on the surface: clear a dark halo so it reads against the
// dither, then draw a pin (locked) or a blinking crosshair (searching).
inline void draw_marker(GFXcanvas1& area, int16_t mx, int16_t my,
                        bool locked, int pulse) {
  for (int yy = my - 3; yy <= my + 3; ++yy)
    for (int xx = mx - 3; xx <= mx + 3; ++xx)
      if ((xx-mx)*(xx-mx) + (yy-my)*(yy-my) <= 9) area.drawPixel(xx, yy, 0);
  if (locked) {
    for (int d = -4; d <= 0; ++d) area.drawPixel(mx, my + d, 1);
    area.drawPixel(mx - 1, my - 4, 1);
    area.drawPixel(mx + 1, my - 4, 1);
    if (pulse >= 0 && pulse < 14) area.drawCircle(mx, my, pulse, 1);
  } else {
    for (int d = -1; d <= 1; ++d) { area.drawPixel(mx + d, my, 1); area.drawPixel(mx, my + d, 1); }
  }
}

// Small satellite marker (body + two panels) with a cleared halo so
// it reads over the globe and the stars.
inline void draw_satellite(GFXcanvas1& area, int16_t sx, int16_t sy) {
  for (int yy = sy - 3; yy <= sy + 3; ++yy)
    for (int xx = sx - 3; xx <= sx + 3; ++xx)
      if ((xx-sx)*(xx-sx) + (yy-sy)*(yy-sy) <= 9) area.drawPixel(xx, yy, 0);
  for (int d = -2; d <= 2; ++d) area.drawPixel(sx + d, sy, 1);
  area.drawPixel(sx, sy - 1, 1);
  area.drawPixel(sx, sy + 1, 1);
}

// A fixed pseudo-random star field for the background outside the
// globe. Deterministic (xorshift seeded by index) so stars do not
// crawl frame to frame; a few twinkle on a slow phase.
inline void draw_stars(GFXcanvas1& area, int16_t y_top, int16_t y_bot,
                       int16_t gcx, int16_t gcy, uint32_t now) {
  const int16_t w = area.width();
  for (int i = 0; i < 28; ++i) {
    uint32_t r = (uint32_t)(i * 2654435761u);   // cheap hash per star
    const int16_t sx = (int16_t)(r % w);
    const int16_t sy = (int16_t)(y_top + 2 + ((r >> 8) % (uint32_t)(y_bot - y_top - 4)));
    // skip stars that would fall on the globe disc
    const int ddx = sx - gcx, ddy = sy - gcy;
    if (ddx*ddx + ddy*ddy <= (PLANET_R + 2) * (PLANET_R + 2)) continue;
    if (((now / 400) + i) % 11 == 0) continue;   // occasional twinkle-off
    area.drawPixel(sx, sy, 1);
  }
}

}  // namespace PlanetView
}  // namespace Sensors
