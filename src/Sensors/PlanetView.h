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
  // View-space unit vector per disc pixel (vx right, vy up, vz toward
  // viewer) plus the fixed Lambert shade. The earth-frame lat/lon is
  // derived per frame by rotating this vector, so the globe can both
  // spin (longitude) AND tilt (latitude) - the latter centres the
  // device on a fix.
  inline int8_t*&  vx()    { static int8_t* p = nullptr; return p; }   // -127..127 = -1..1
  inline int8_t*&  vy()    { static int8_t* p = nullptr; return p; }
  inline int8_t*&  vz()    { static int8_t* p = nullptr; return p; }
  inline uint8_t*& shade() { static uint8_t* p = nullptr; return p; }  // 0..15, 0xFF outside
  inline bool&     built() { static bool b = false; return b; }

  inline void ensure() {
    if (built()) return;
    const size_t n = (size_t)BB * BB;
    vx()    = (int8_t*)  heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    vy()    = (int8_t*)  heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    vz()    = (int8_t*)  heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    shade() = (uint8_t*) heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!vx() || !vy() || !vz() || !shade()) return;   // OOM: render() falls back
    float lx = -0.5f, ly = -0.55f, lz = 0.66f;
    const float ln = sqrtf(lx*lx + ly*ly + lz*lz);
    lx /= ln; ly /= ln; lz /= ln;
    for (int dy = -PLANET_R; dy <= PLANET_R; ++dy) {
      for (int dx = -PLANET_R; dx <= PLANET_R; ++dx) {
        const size_t i = (size_t)(dy + PLANET_R) * BB + (dx + PLANET_R);
        const float nx = (float)dx / PLANET_R, ny = (float)dy / PLANET_R;
        const float d2 = nx*nx + ny*ny;
        if (d2 > 1.0f) { shade()[i] = 0xFF; vx()[i] = vy()[i] = vz()[i] = 0; continue; }
        const float nz = sqrtf(1.0f - d2);
        // View space: x right, y up (screen down is +ny), z to viewer.
        vx()[i] = (int8_t)lrintf(nx  * 127.0f);
        vy()[i] = (int8_t)lrintf(-ny * 127.0f);
        vz()[i] = (int8_t)lrintf(nz  * 127.0f);
        float diff = nx*lx + ny*ly + nz*lz; if (diff < 0) diff = 0;
        shade()[i] = (uint8_t)((0.15f + 0.85f * diff) * 15.0f + 0.5f);
      }
    }
    built() = true;
  }
}

// Rotate a view-space vector into earth coordinates by tilt phi (about
// X) then spin theta (about Y), returning (lat, lon) in degrees. This
// is the inverse of the view rotation, so a pixel whose earth point is
// the device centre maps to the front.
inline void view_to_latlon(float vx, float vy, float vz,
                           float cphi, float sphi, float cth, float sth,
                           int& lat_d, int& lon_d) {
  // Rx(phi)
  const float y1 = vy * cphi - vz * sphi;
  const float z1 = vy * sphi + vz * cphi;
  // Ry(theta)
  const float x2 = vx * cth + z1 * sth;
  const float z2 = -vx * sth + z1 * cth;
  lat_d = (int)lrintf(asinf(y1 > 1 ? 1 : (y1 < -1 ? -1 : y1)) * RAD2DEG);
  lon_d = (int)lrintf(atan2f(x2, z2) * RAD2DEG);
}

// Project an earth (lat, lon) point to screen for the current spin
// (theta=rot) and tilt (phi). Front-facing when the rotated z > 0.
inline bool project(double lat_d, double lon_d, int rot_deg, int tilt_deg,
                    int16_t cx, int16_t cy, int16_t& ox, int16_t& oy) {
  const float la = (float)lat_d / RAD2DEG, lo = (float)lon_d / RAD2DEG;
  float x = cosf(la) * sinf(lo), y = sinf(la), z = cosf(la) * cosf(lo);
  // Apply the same view rotation: Ry(-theta) then Rx(-phi).
  const float th = rot_deg / RAD2DEG, phi = tilt_deg / RAD2DEG;
  const float cth = cosf(th), sth = sinf(th), cph = cosf(phi), sph = sinf(phi);
  const float x1 = x * cth - z * sth;
  const float z1 = x * sth + z * cth;
  const float y2 = y * cph + z1 * sph;
  const float z2 = -y * sph + z1 * cph;
  ox = (int16_t)lrintf(cx + x1 * PLANET_R);
  oy = (int16_t)lrintf(cy - y2 * PLANET_R);
  return z2 > 0.05f;
}

// Draw the shaded globe spun by rot_deg (longitude) and tilted by
// tilt_deg (latitude). tilt_deg 0 keeps north up; a non-zero tilt
// brings that latitude to the centre (used to centre the device on a
// fix). Per frame this rotates the precomputed view vectors and
// samples the land mask - a couple of transcendentals per disc pixel.
inline void render_globe(GFXcanvas1& area, int16_t cx, int16_t cy,
                         int rot_deg, int tilt_deg) {
  _d::ensure();
  if (!_d::built()) { area.drawCircle(cx, cy, PLANET_R, 1); return; }
  const float th = rot_deg / RAD2DEG, phi = tilt_deg / RAD2DEG;
  const float cth = cosf(th), sth = sinf(th), cph = cosf(phi), sph = sinf(phi);
  for (int dy = -PLANET_R; dy <= PLANET_R; ++dy) {
    const int16_t py = cy + dy;
    for (int dx = -PLANET_R; dx <= PLANET_R; ++dx) {
      const size_t i = (size_t)(dy + PLANET_R) * BB + (dx + PLANET_R);
      const uint8_t sh = _d::shade()[i];
      if (sh == 0xFF) continue;
      int lat_d, lon_d;
      view_to_latlon(_d::vx()[i] / 127.0f, _d::vy()[i] / 127.0f, _d::vz()[i] / 127.0f,
                     cph, sph, cth, sth, lat_d, lon_d);
      const bool land = is_land(lat_d, lon_d);
      const int16_t px = cx + dx;
      const float shf  = sh / 15.0f;
      const float thrf = (BAYER[((py & 3) << 2) | (px & 3)] + 0.5f) / 16.0f;
      const bool lit = land ? (shf > 0.33f || shf > 0.9f * thrf)
                            : (shf > 0.62f + 0.35f * thrf);
      if (lit) area.drawPixel(px, py, 1);
    }
  }
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
