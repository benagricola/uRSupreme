// QMC6310 3-axis magnetometer driver.
//
// Lives on the T-Beam Supreme's user/sensor I2C bus (Wire, SDA=17,
// SCL=18) at 0x1C (QMC6310U variant) or 0x3C (QMC6310N). Shares the
// bus with BME280 and the OLED. Each device has its
// own address so coexistence is automatic.
//
// We use Lewis He's SensorLib wrapper (lewisxhe/SensorLib) - the
// same library LilyGo's factory firmware uses, so the calibration
// + configuration paths are well-trodden.
//
// A third board variant, the QMC6309, lives at 0x7C and is a
// different part (different register map); it needs its own driver
// and is not probed here.
//
// API mirrors Bme280/Gps: begin() probes both addresses, pump()
// reads at most once per interval_ms (default 60 s), last_reading()
// returns the cached snapshot.
//
// Heading is computed here, not by the library. SensorLib's readPolar
// is a bare atan2 of the raw vector with no offset removal. On this
// board the magnetometer sits beside the PA, PMU and battery, whose
// static (hard-iron) field reads ~100 uT and swamps earth's ~40 uT,
// so the raw vector orbits a circle far off the origin and a bare
// atan2 only sweeps a few tens of degrees over a full turn. We track a
// running per-axis 3D min/max as the device is moved, subtract the
// centre, and equalise the axis gains to recover a clean field vector.
//
// Heading is then tilt-compensated: the corrected field is remapped
// into the accelerometer's frame (board-fixed alignment below) and the
// bearing is taken from its projection onto the true horizontal plane
// defined by the IMU's gravity vector, so it stays correct when the
// device is held at an angle, not only flat. The calibration is
// RAM-only and improves as more of the sphere is seen;
// reset_calibration() restarts it.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMC6310.hpp>
#include "../Motion/QMI8658.h"   // accelerometer gravity vector, for tilt compensation

namespace Sensors {
namespace QMC6310 {

struct Reading {
  bool      valid       = false;
  bool      cal_ready   = false;  // all axes swept enough to trust heading
  float     cal_progress = 0.0f;  // 0..1 toward cal_ready (least-swept axis)
  float     field_uT    = 0.0f;   // hard/soft-iron corrected |B| (true field)
  uint32_t  taken_ms    = 0;
  float     x_uT        = 0.0f;
  float     y_uT        = 0.0f;
  float     z_uT        = 0.0f;
  float     heading_deg = 0.0f;   // 0 = magnetic north, 0..360
};

// Per-axis sweep (uT) needed on all three axes before flatness is even
// judged - just "enough motion to start". The real gate is flatness
// below, because spans alone can be satisfied by a quick partial wave
// that still leaves the centre wrong.
inline constexpr float CAL_MIN_SPAN_UT = 50.0f;

// Flatness gate. A correct hard-iron centre makes the corrected |B|
// constant at every orientation; a partial-coverage centre leaves it
// swinging with orientation even though the spans look full (this is the
// "fills in 1-2 s but reads wrong" failure). So we window the recent
// readings and only call the calibration good when the corrected |B| is
// flat (low relative spread) WHILE the raw field shows real movement -
// which a held-still device or a quick partial wave cannot fake.
inline constexpr int   CAL_WIN         = 48;     // ~5 s of samples at 10 Hz
inline constexpr float CAL_FLAT_REL    = 0.10f;  // corrected |B| rel-std that counts as flat
inline constexpr float CAL_DIRTY_REL   = 0.22f;  // rel-std at which flatness progress is zero
inline constexpr float CAL_MOVE_MIN_UT = 30.0f;  // raw |B| window spread proving movement

// Accelerometer/magnetometer axis alignment, fixed by the board layout
// (the two chips sit at known, different orientations). Solved once on
// hardware by tumbling the device and finding the axis mapping that
// keeps the gravity-to-field angle constant: the accelerometer frame's
// axes are the magnetometer's (+Y, +X, -Z). That fit won clearly (8.7
// deg residual vs 28.8 for the runner-up). This is a per-model board
// constant, not a per-unit calibration, so it is baked in here rather
// than re-solved on the device (which would need a tumble every boot).
//
// Heading zero offset and rotation sense. The offset is a fixed board
// constant - the angle between the chip axes and the board's top edge -
// so it is measured once and baked, never set per user. Measured 245.6
// deg at magnetic north on a clean (flat-field) calibration, so +74.4
// zeroes it there. Sign +1: clockwise rotation increases the bearing.
// Declination (true vs magnetic) is ~1 deg here, inside the noise, so
// it is ignored; add it from GPS later for true north. The +74.4 still
// wants a confirm flash in the normal hold (it was read screen-down).
inline constexpr float HEADING_OFFSET_DEG = 74.4f;
inline constexpr float HEADING_SIGN       = 1.0f;

// Gyro complementary filter. The gyroscope gives a smooth, low-noise
// yaw rate that we integrate for the instant response; the magnetometer
// slowly pulls that estimate back to absolute north, so its noise is
// averaged away over ~1/FUSE_K updates. This is how a phone holds a
// steady heading off a noisy magnetometer.
inline constexpr float GYRO_SIGN     = -1.0f;  // sign of yaw-rate -> heading
inline constexpr float FUSE_K        = 0.05f;  // magnetometer pull per update
inline constexpr float FUSE_MAX_DT_S = 1.0f;   // longer gap: reseed, don't integrate

namespace _detail {
  inline SensorQMC6310& sensor()     { static SensorQMC6310 s; return s; }
  inline bool&     present_ref()     { static bool v = false; return v; }
  inline Reading&  last_ref()        { static Reading r; return r; }
  inline uint32_t& interval_ms_ref() { static uint32_t v = 60000; return v; }
  inline uint32_t& live_until_ref()  { static uint32_t v = 0; return v; }
  inline uint8_t&  addr_ref()        { static uint8_t v = 0; return v; }
  inline bool&     enabled_ref()     { static bool v = true; return v; }
  // Running hard-iron bounds, accumulated across readings as the
  // device turns. xc/yc = centre = (min+max)/2; the axis spans give
  // both the soft-iron gain and the "calibrated enough" test.
  struct Calib { bool seeded = false; float xmin = 0, xmax = 0, ymin = 0, ymax = 0, zmin = 0, zmax = 0; };
  inline Calib& calib_ref() { static Calib c; return c; }
  inline bool&  cal_dirty_ref()  { static bool v = false; return v; }  // a save is pending
  inline bool&  prev_ready_ref() { static bool v = false; return v; }  // last gated-ready, for edge detect
  // Sliding window of recent raw + corrected |B|, for the flatness gate.
  struct CalWin {
    float raw[CAL_WIN]  = {0};
    float corr[CAL_WIN] = {0};
    int   idx = 0, cnt = 0;
    bool  ready = false;
    float flat_prog = 0.0f;
  };
  inline CalWin& calwin_ref() { static CalWin w; return w; }
}

inline bool begin(TwoWire& wire) {
  // SensorLib's QMC6310 only knows one address constant (0x1C); the
  // QMC6310N is at 0x3C. Probe both via init(). The sda/scl args are
  // -1 so SensorLib's wire.begin(sda,scl) call reuses the existing
  // bus we set up in the .ino. (ESP32 Wire.begin is idempotent.)
  const uint8_t addrs[] = { 0x1C, 0x3C };
  for (uint8_t a : addrs) {
    if (_detail::sensor().init(wire, -1, -1, a)) {
      _detail::present_ref() = true;
      _detail::addr_ref()    = a;
      _detail::sensor().configMagnetometer(
          SensorQMC6310::MODE_CONTINUOUS,
          SensorQMC6310::RANGE_8G,
          SensorQMC6310::DATARATE_50HZ,   // headroom for the 10 Hz live poll
          SensorQMC6310::OSR_8,
          SensorQMC6310::DSR_1);
      // Declination defaults to 0 - fine for relative readings; the
      // user can override per-location in a follow-up if needed.
      _detail::sensor().setDeclination(0.0f);
      NOTICEF("QMC6310: found at 0x%02x", a);
      return true;
    }
  }
  NOTICE("QMC6310: not detected on Wire (probed 0x1C + 0x3C)");
  _detail::present_ref() = false;
  return false;
}

// Fast poll period while a live demand is active. 10 Hz feeds the gyro
// complementary filter a tight integration step.
inline constexpr uint32_t LIVE_POLL_MS = 100;

inline void pump() {
  if (!_detail::present_ref()) return;
  if (!_detail::enabled_ref()) return;
  const uint32_t now = millis();
  const auto& last = _detail::last_ref();
  // A live demand (a screen showing the compass is open) overrides both
  // the idle interval and boot-only mode with a fast poll.
  const bool live = now < _detail::live_until_ref();
  const uint32_t eff_interval = live ? LIVE_POLL_MS : _detail::interval_ms_ref();
  if (!live && _detail::interval_ms_ref() == 0 && last.taken_ms != 0) return;
  if (last.taken_ms != 0 && (now - last.taken_ms) < eff_interval) return;

  Polar p;
  if (!_detail::sensor().readPolar(p)) return;   // triggers the read; p.polar unused
  Reading r;
  r.taken_ms    = now;
  r.x_uT        = _detail::sensor().getX();
  r.y_uT        = _detail::sensor().getY();
  r.z_uT        = _detail::sensor().getZ();
  r.valid       = !isnan(r.x_uT) && !isnan(r.y_uT) && !isnan(r.z_uT);

  if (r.valid) {
    // Grow the 3D hard-iron bounds as the device moves through its
    // orientations.
    _detail::Calib& c = _detail::calib_ref();
    if (!c.seeded) {
      c.xmin = c.xmax = r.x_uT;
      c.ymin = c.ymax = r.y_uT;
      c.zmin = c.zmax = r.z_uT;
      c.seeded = true;
    } else {
      if (r.x_uT < c.xmin) c.xmin = r.x_uT;
      if (r.x_uT > c.xmax) c.xmax = r.x_uT;
      if (r.y_uT < c.ymin) c.ymin = r.y_uT;
      if (r.y_uT > c.ymax) c.ymax = r.y_uT;
      if (r.z_uT < c.zmin) c.zmin = r.z_uT;
      if (r.z_uT > c.zmax) c.zmax = r.z_uT;
    }
    const float rx = c.xmax - c.xmin, ry = c.ymax - c.ymin, rz = c.zmax - c.zmin;
    const float xc = (c.xmin + c.xmax) * 0.5f, yc = (c.ymin + c.ymax) * 0.5f,
                zc = (c.zmin + c.zmax) * 0.5f;
    // Hard-iron centre removal + soft-iron per-axis gain equalisation.
    const float avg = (rx + ry + rz) / 3.0f;
    float mx = r.x_uT - xc, my = r.y_uT - yc, mz = r.z_uT - zc;
    if (rx > 1.0f) mx *= avg / rx;
    if (ry > 1.0f) my *= avg / ry;
    if (rz > 1.0f) mz *= avg / rz;
    // Corrected magnetic vector expressed in the accelerometer frame
    // via the board-fixed alignment (+Y, +X, -Z).
    const float bx = my, by = mx, bz = -mz;
    // Gravity from the IMU. If the IMU is absent or has no reading yet,
    // fall back to "screen flat" so the compass still works untilted.
    const QMI8658::Reading imu = QMI8658::last_reading();
    const bool have_g = imu.valid;
    const float gx = have_g ? imu.accel_x_g : 0.0f;
    const float gy = have_g ? imu.accel_y_g : 0.0f;
    const float gz = have_g ? imu.accel_z_g : 1.0f;
    // Tilt-compensated heading: East = B x G, North = G x East, then
    // the bearing of the device's forward (accel +X) axis: its East and
    // North components are ex and nx, so heading = atan2(ex, nx).
    const float ex = by * gz - bz * gy;
    const float ey = bz * gx - bx * gz;
    const float ez = bx * gy - by * gx;
    const float nx = gy * ez - gz * ey;
    float h = atan2f(HEADING_SIGN * ex, nx) * 57.29578f + HEADING_OFFSET_DEG;
    h = fmodf(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    // Gyro complementary filter: integrate the yaw rate about the
    // vertical (gyro projected onto gravity) for the smooth instant
    // response, then pull gently toward the magnetometer heading so the
    // long-term drift is corrected and the mag noise is averaged out.
    {
      static bool     f_seeded = false;
      static float    f_head   = 0.0f;
      static uint32_t f_ms     = 0;
      const float gmag = sqrtf(gx * gx + gy * gy + gz * gz);
      const float wyaw = (have_g && gmag > 0.01f)
          ? (imu.gyro_x_dps * gx + imu.gyro_y_dps * gy + imu.gyro_z_dps * gz) / gmag
          : 0.0f;
      const float dt = (f_ms == 0) ? 0.0f : (now - f_ms) * 0.001f;
      if (!f_seeded || !have_g || dt <= 0.0f || dt > FUSE_MAX_DT_S) {
        f_head = h;                        // (re)seed straight to the mag heading
        f_seeded = true;
      } else {
        f_head += GYRO_SIGN * wyaw * dt;   // gyro predicts
        float err = h - f_head;            // pull toward the mag heading
        while (err > 180.0f)  err -= 360.0f;
        while (err < -180.0f) err += 360.0f;
        f_head += FUSE_K * err;            // mag corrects
      }
      f_ms = now;
      f_head = fmodf(f_head, 360.0f);
      if (f_head < 0.0f) f_head += 360.0f;
      h = f_head;
    }
    r.heading_deg = h;
    r.field_uT = sqrtf(mx * mx + my * my + mz * mz);

    const bool spans_full = rx >= CAL_MIN_SPAN_UT
                         && ry >= CAL_MIN_SPAN_UT && rz >= CAL_MIN_SPAN_UT;
    // Push raw + corrected |B| into the window and, once it is full and
    // the raw field shows movement, judge flatness: low relative spread
    // of the corrected |B| means the centre is right. ready latches that
    // verdict (so holding still afterwards keeps it), updated only while
    // moving so a static device can neither earn nor lose it falsely.
    const float raw_mag = sqrtf(r.x_uT * r.x_uT + r.y_uT * r.y_uT + r.z_uT * r.z_uT);
    _detail::CalWin& w = _detail::calwin_ref();
    w.raw[w.idx] = raw_mag;
    w.corr[w.idx] = r.field_uT;
    w.idx = (w.idx + 1) % CAL_WIN;
    if (w.cnt < CAL_WIN) w.cnt++;
    if (w.cnt >= CAL_WIN) {
      float rmin = w.raw[0], rmax = w.raw[0], sum = 0.0f, sumsq = 0.0f;
      for (int i = 0; i < CAL_WIN; ++i) {
        if (w.raw[i] < rmin) rmin = w.raw[i];
        if (w.raw[i] > rmax) rmax = w.raw[i];
        sum += w.corr[i];
        sumsq += w.corr[i] * w.corr[i];
      }
      if ((rmax - rmin) > CAL_MOVE_MIN_UT) {        // only judge while moving
        const float mean = sum / CAL_WIN;
        float var = sumsq / CAL_WIN - mean * mean;
        if (var < 0.0f) var = 0.0f;
        const float rel = mean > 1.0f ? sqrtf(var) / mean : 1.0f;
        float fp = (CAL_DIRTY_REL - rel) / (CAL_DIRTY_REL - CAL_FLAT_REL);
        w.flat_prog = fp < 0.0f ? 0.0f : (fp > 1.0f ? 1.0f : fp);
        w.ready = spans_full && rel < CAL_FLAT_REL;
      }
    }
    // The bar shows coverage until the spans are met, then flatness - so
    // it cannot read full until the centre is actually good.
    const float least = rx < ry ? (rx < rz ? rx : rz) : (ry < rz ? ry : rz);
    float span_prog = least / CAL_MIN_SPAN_UT;
    if (span_prog > 1.0f) span_prog = 1.0f;
    r.cal_progress = spans_full ? w.flat_prog : span_prog;
    r.cal_ready = have_g && w.ready;
    // Persist once, on the edge where a GOOD (flat) calibration completes.
    if (w.ready && !_detail::prev_ready_ref()) _detail::cal_dirty_ref() = true;
    _detail::prev_ready_ref() = w.ready;
  }
  _detail::last_ref() = r;
}

inline const char* model_name() { return "QMC6310"; }
inline bool      present()       { return _detail::present_ref(); }
inline uint8_t   address()       { return _detail::addr_ref(); }
inline Reading   last_reading()  { return _detail::last_ref(); }
inline uint32_t  interval_ms()   { return _detail::interval_ms_ref(); }
inline void      set_interval_ms(uint32_t ms) { _detail::interval_ms_ref() = ms; }
// While live (a screen showing this sensor is open), poll fast
// instead of at the idle interval. The consumer renews the TTL.
inline void request_live(uint32_t ttl_ms = 1500) {
  _detail::live_until_ref() = millis() + ttl_ms;
}
// Restart hard-iron calibration: the next full turn re-learns the
// bounds. Use after the device moves to a magnetically different spot.
inline void reset_calibration() {
  _detail::calib_ref()  = _detail::Calib{};
  _detail::calwin_ref() = _detail::CalWin{};
  _detail::prev_ready_ref() = false;
  _detail::cal_dirty_ref()  = true;   // clear the persisted copy too
}

// Calibration persistence (file I/O lives in Sensors::SensorConfig so the
// driver stays storage-free). The running hard-iron bounds survive
// reboots, so one good calibration sticks. take_cal_dirty() reports once
// that they changed; cal_seeded() says whether there is anything to save;
// get/set move the six bounds to and from storage.
inline bool take_cal_dirty() { bool& d = _detail::cal_dirty_ref(); const bool v = d; d = false; return v; }
inline bool cal_seeded() { return _detail::calib_ref().seeded; }
inline void get_cal_bounds(float b[6]) {
  const _detail::Calib& c = _detail::calib_ref();
  b[0] = c.xmin; b[1] = c.xmax; b[2] = c.ymin;
  b[3] = c.ymax; b[4] = c.zmin; b[5] = c.zmax;
}
inline void set_cal_bounds(const float b[6]) {
  _detail::Calib& c = _detail::calib_ref();
  c.xmin = b[0]; c.xmax = b[1]; c.ymin = b[2];
  c.ymax = b[3]; c.zmin = b[4]; c.zmax = b[5];
  c.seeded = true;
  // A persisted cal passed the flatness gate when it was saved, so trust
  // it as ready immediately (the compass works from boot). prev_ready
  // suppresses a redundant re-save on the first poll.
  _detail::calwin_ref().ready = true;
  _detail::prev_ready_ref()   = true;
}
inline bool      enabled()       { return _detail::enabled_ref(); }
inline void      set_enabled(bool on) { _detail::enabled_ref() = on; }

} // namespace QMC6310
} // namespace Sensors
