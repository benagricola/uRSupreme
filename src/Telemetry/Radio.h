// Radio telemetry — 1 Hz rolling history of channel/RSSI/CSMA state.
//
// Captures what the OLED status bar already shows plus the parts we
// usually only learn about post-mortem (peer-side channel utilisation,
// the contention-window band the CSMA loop is parked in, instantaneous
// DCD). Samples are pushed to a fixed ring and published live over the
// SPA WebSocket so a browser can plot what's happening on-air while a
// test is running — much faster diagnosis than re-running the test
// after every firmware change.
//
// Two consumers:
//   * `GET /api/radio/telemetry` — returns the ring (oldest→newest).
//   * WS frame `{"type":"radio_telemetry", ...}` — pushed once per
//     SAMPLE_PERIOD_MS while at least one SPA client is connected.
//
// Field-name choice is deliberately terse (rx/tx/own/peer/total/cw/dcd
// rather than full words) — this frame ships every second over WiFi
// and we want it well under a typical pbuf so it never blocks on lwIP
// flow control.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <stdint.h>
#include <stddef.h>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

// Globals defined in Config.h / RNode_Firmware.ino. Web/WebUI.h is
// included before Config.h in the .ino, so we forward-declare here
// rather than relying on transitive visibility.
extern int      current_rssi;
extern int      noise_floor;
extern float    airtime;
extern float    longterm_airtime;
extern float    local_channel_util;
extern float    total_channel_util;
extern bool     dcd;
extern uint8_t  cw_band;
extern uint8_t  cw_min;
extern uint8_t  cw_max;
extern bool     airtime_lock;
extern uint32_t stat_rx;
extern uint32_t stat_tx;
extern volatile uint32_t lora_tx_dropped;  // packets dropped because TX ring full

namespace Telemetry {
namespace Radio {

inline constexpr uint32_t SAMPLE_PERIOD_MS = 1000;
inline constexpr size_t   RING_CAP         = 120;   // 2 minutes at 1 Hz

struct Sample {
  uint32_t ts_ms;          // device millis() at sample
  int16_t  rssi_dbm;       // current_rssi — last decoded RSSI
  int16_t  noise_dbm;      // noise_floor — measured ambient when DCD off.
                           // Gap to rssi is effective SNR; a +10 dB rise
                           // tracks new interferers walking into the band.
  uint8_t  own_pct;        // (int)(airtime * 100), 0-100 — our own TX duty cycle
  uint8_t  others_pct;     // (int)(local_channel_util * 100), 0-100 — aggregate
                           // non-self DCD-busy fraction. DCD does not sample
                           // while we are TX'ing (modem is in TX mode) so
                           // this excludes our own emissions. It conflates
                           // multiple peers and any LoRa-modulated
                           // interferer the modem demodulates — NOT a
                           // per-device figure.
                           // `total` (own + others, capped at 100) was
                           // dropped — on a half-duplex medium it's a pure
                           // derived value and adds no visual information.
  uint8_t  cw_band;        // 1..CSMA_CW_BANDS
  uint8_t  flags;          // bit0 = dcd, bit1 = airtime_lock
  uint16_t rx_delta;       // stat_rx delta since previous sample
  uint16_t tx_delta;       // stat_tx delta since previous sample
};
// 16 bytes per sample × 120 = 1920 B ring. Allocated in PSRAM at first
// access (see _detail::ring) so it doesn't tax internal SRAM, which is
// the scarce resource on this SoC.

// Per-window own-TX accumulator. The firmware-level `airtime` global
// is a 15-second moving average (two 7.5s bins, summed and divided by
// 2 × AIRTIME_BINLEN_MS), so a single ~30ms announce packet
// contributes 30/15000 ≈ 0.2% — rounds to 0% in our uint8 sample. To
// catch short bursts at 1Hz resolution we accumulate per-packet
// airtime here from add_airtime() and convert to a percentage at each
// snapshot tick. This is purely a telemetry side-channel; the
// firmware's own CSMA / airtime-lock paths still use the smoothed
// global so their semantics are unchanged.
inline volatile uint32_t& tx_window_ms_acc()  { static volatile uint32_t v = 0; return v; }
inline uint32_t& tx_window_start_ms()         { static uint32_t v = 0; return v; }

// Called from add_airtime() after the per-packet airtime calculation.
// `packet_ms` is the same value the firmware adds to airtime_bins.
// Cheap — one add into a volatile uint32_t.
inline void note_tx_ms(uint32_t packet_ms) {
  tx_window_ms_acc() += packet_ms;
}

// Per-window peer-activity accumulator. The firmware-level
// `local_channel_util` is a 7.5-second rolling average of DCD-busy
// samples, which buries short bursts (one ~60ms peer packet ≈ 1%
// there for 7.5s). We mirror the own-TX accumulator pattern: bump
// dcd_high_ticks each time the DCD-busy sample comes back true,
// bump dcd_total_ticks unconditionally, divide at snapshot.
// Caller in check_modem_status() fires every STATUS_INTERVAL_MS (3ms)
// so each tick represents a 3ms slice — the same source the upstream
// local_channel_util uses, just without the 7.5-second smoothing tail.
inline volatile uint32_t& dcd_high_ticks()  { static volatile uint32_t v = 0; return v; }
inline volatile uint32_t& dcd_total_ticks() { static volatile uint32_t v = 0; return v; }

inline void note_dcd_sample(bool busy) {
  dcd_total_ticks()++;
  if (busy) dcd_high_ticks()++;
}

namespace _detail {
  // PSRAM-allocated ring (1920 B). Lazily heap_caps_malloc'd on first
  // access; the pointer is cached in a function-local static so a
  // failed allocation gets re-attempted next tick rather than poisoning
  // the slot permanently. Falls back to MALLOC_CAP_8BIT (internal SRAM)
  // only if PSRAM isn't available — non-fatal for boards without it.
  inline Sample* ring() {
    static Sample* r = nullptr;
    if (r) return r;
#if defined(ESP32)
    r = (Sample*)heap_caps_calloc(RING_CAP, sizeof(Sample), MALLOC_CAP_SPIRAM);
    const bool in_psram = (r != nullptr);
    if (!r) r = (Sample*)heap_caps_calloc(RING_CAP, sizeof(Sample), MALLOC_CAP_8BIT);
    if (r) {
      // One-shot diagnostic: prints once on the first sampler tick,
      // never again. Useful to verify PSRAM placement vs SRAM
      // fallback after a firmware change to the allocator path.
      // ESP32-S3 PSRAM is mapped at 0x3C000000-0x3F000000; internal
      // SRAM at 0x3FC80000-0x3FCFFFFF.
      NOTICEF("RadioTelemetry: ring @ %p (%u B, %s)",
              (void*)r,
              (unsigned)(RING_CAP * sizeof(Sample)),
              in_psram ? "PSRAM" : "internal SRAM fallback");
    }
#else
    static Sample fallback[RING_CAP] = {};
    r = fallback;
#endif
    return r;
  }
  inline size_t&   ring_count()       { static size_t   n = 0; return n; }
  inline size_t&   ring_head()        { static size_t   h = 0; return h; }
  inline uint32_t& last_sample_ms()   { static uint32_t t = 0; return t; }
  inline uint32_t& last_stat_rx()     { static uint32_t v = 0; return v; }
  inline uint32_t& last_stat_tx()     { static uint32_t v = 0; return v; }

  inline void push(const Sample& s) {
    Sample* r = ring();
    if (!r) return;
    r[ring_head()] = s;
    ring_head() = (ring_head() + 1) % RING_CAP;
    if (ring_count() < RING_CAP) ++ring_count();
  }

  inline uint8_t pct_from_unit(float u) {
    int v = (int)(u * 100.0f);
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    return (uint8_t)v;
  }
}

// Build a Sample from the current live globals. RX/TX are reported as
// per-period deltas so the SPA can graph "packets per second" directly
// without bookkeeping; the absolute counters are still available from
// /api/info if needed.
inline Sample snapshot(uint32_t now_ms) {
  Sample s = {};
  s.ts_ms     = now_ms;
  s.rssi_dbm  = (int16_t)current_rssi;
  s.noise_dbm = (int16_t)noise_floor;
  // Own util: prefer the per-window accumulator (catches sub-second
  // bursts like a single announce). Fall back to the smoothed
  // firmware global on the first tick before the window starts.
  {
    const uint32_t start = tx_window_start_ms();
    uint32_t window_ms = (start == 0) ? 0 : (now_ms - start);
    const uint32_t tx_ms = tx_window_ms_acc();
    if (window_ms > 0) {
      uint32_t pct = (tx_ms * 100UL) / window_ms;
      if (pct > 100) pct = 100;
      s.own_pct = (uint8_t)pct;
    } else {
      s.own_pct = _detail::pct_from_unit(airtime);
    }
    tx_window_ms_acc()   = 0;
    tx_window_start_ms() = now_ms;
  }
  // Others util: prefer the per-window DCD accumulator (catches
  // sub-second peer bursts). Fall back to the 7.5s-smoothed firmware
  // global if no samples have arrived yet (boot edge).
  {
    const uint32_t high = dcd_high_ticks();
    const uint32_t tot  = dcd_total_ticks();
    if (tot > 0) {
      uint32_t pct = (high * 100UL) / tot;
      if (pct > 100) pct = 100;
      s.others_pct = (uint8_t)pct;
    } else {
      s.others_pct = _detail::pct_from_unit(local_channel_util);
    }
    dcd_high_ticks()  = 0;
    dcd_total_ticks() = 0;
  }
  s.cw_band   = cw_band;
  uint8_t flags = 0;
  if (dcd)          flags |= 0x01;
  if (airtime_lock) flags |= 0x02;
  s.flags = flags;
  const uint32_t rx_now = stat_rx;
  const uint32_t tx_now = stat_tx;
  uint32_t drx = rx_now - _detail::last_stat_rx();
  uint32_t dtx = tx_now - _detail::last_stat_tx();
  if (drx > 0xFFFF) drx = 0xFFFF;
  if (dtx > 0xFFFF) dtx = 0xFFFF;
  s.rx_delta = (uint16_t)drx;
  s.tx_delta = (uint16_t)dtx;
  _detail::last_stat_rx() = rx_now;
  _detail::last_stat_tx() = tx_now;
  return s;
}

// Encode a Sample into the WS/REST JSON shape. The three utilisation
// figures are grouped under `util` so they're visually clustered apart
// from RSSI/noise/CSMA state. `util.others` = aggregate non-self
// channel activity — see Sample::others_pct comment for what it does
// and doesn't include.
inline void encode(const Sample& s, JsonObject o) {
  o["ts"]    = s.ts_ms;
  o["rssi"]  = s.rssi_dbm;
  o["noise"] = s.noise_dbm;
  JsonObject u = o["util"].to<JsonObject>();
  u["own"]    = s.own_pct;
  u["others"] = s.others_pct;
  o["cw"]    = s.cw_band;
  o["dcd"]   = (bool)(s.flags & 0x01);
  o["lock"]  = (bool)(s.flags & 0x02);
  o["rx"]    = s.rx_delta;
  o["tx"]    = s.tx_delta;
}

// Fill a JsonArray (oldest→newest) with the entire ring. Used by the
// REST endpoint when a fresh SPA client wants the history that
// preceded its WS subscription.
inline void fill_history(JsonArray arr) {
  const size_t n = _detail::ring_count();
  if (n == 0) return;
  const Sample* r = _detail::ring();
  if (!r) return;
  const size_t start = (_detail::ring_head() + RING_CAP - n) % RING_CAP;
  for (size_t i = 0; i < n; ++i) {
    JsonObject o = arr.add<JsonObject>();
    encode(r[(start + i) % RING_CAP], o);
  }
}

// Take a sample if SAMPLE_PERIOD_MS has elapsed. Pushes to the ring
// unconditionally; the WS publish is the caller's job (it has access
// to the WS publish function and the any_subscribers gate). Returns a
// pointer to the freshly-pushed Sample when one was taken this tick,
// nullptr otherwise.
inline const Sample* tick(uint32_t now_ms) {
  if (_detail::last_sample_ms() != 0
      && now_ms - _detail::last_sample_ms() < SAMPLE_PERIOD_MS) {
    return nullptr;
  }
  _detail::last_sample_ms() = now_ms;
  Sample s = snapshot(now_ms);
  _detail::push(s);
  Sample* r = _detail::ring();
  if (!r) return nullptr;
  // Return a pointer into the ring at the head's prior slot — i.e.
  // the slot we just wrote.
  const size_t idx = (_detail::ring_head() + RING_CAP - 1) % RING_CAP;
  return &r[idx];
}

inline size_t history_size()         { return _detail::ring_count(); }
inline size_t history_capacity()     { return RING_CAP; }
inline uint32_t sample_period_ms()   { return SAMPLE_PERIOD_MS; }

} // namespace Radio
} // namespace Telemetry
