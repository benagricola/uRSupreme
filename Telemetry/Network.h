// Network telemetry — 1 Hz rolling history of WiFi/transport byte rates.
//
// The radio graph (Telemetry::Radio) shows what's happening on-air; this is
// its counterpart for the IP side — the aggregate tx/rx byte rate across the
// non-LoRa interfaces (rmap TCP client, TCP server, UDP). It makes the
// otherwise-invisible backbone traffic visible while a test runs: whether a
// forwarded path request is actually leaving over rmap, how hard the firehose
// is hitting us, whether a transfer is moving bytes at all.
//
// Two consumers, identical mechanism to the radio telemetry:
//   * `GET /api/network/telemetry` — returns the ring (oldest→newest).
//   * WS frame `{"type":"network_telemetry", ...}` — pushed once per
//     SAMPLE_PERIOD_MS while at least one SPA client is connected.
//
// This header stays free of RNS includes (like Telemetry::Radio): the caller
// sums the interface txb()/rxb() counters and passes the cumulative totals to
// tick(); we keep the previous totals and emit per-second deltas (= B/s, since
// the period is 1 s). Deltas are uint32 — rmap can push tens of KB/s, well
// past the uint16 the radio packet-count deltas use.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <stdint.h>
#include <stddef.h>
#if defined(ESP32)
#include <esp_heap_caps.h>
#endif

namespace Telemetry {
namespace Network {

inline constexpr uint32_t SAMPLE_PERIOD_MS = 1000;
inline constexpr size_t   RING_CAP         = 120;   // 2 minutes at 1 Hz

struct Sample {
  uint32_t ts_ms;     // device millis() at sample
  uint32_t tx_bps;    // bytes transmitted across non-LoRa ifaces this period
  uint32_t rx_bps;    // bytes received across non-LoRa ifaces this period
};
// 12 bytes per sample × 120 = 1440 B ring, PSRAM-backed (see _detail::ring).

namespace _detail {
  // PSRAM-allocated ring; mirrors Telemetry::Radio's allocator path. Falls
  // back to internal SRAM only where PSRAM is unavailable.
  inline Sample* ring() {
    static Sample* r = nullptr;
    if (r) return r;
#if defined(ESP32)
    r = (Sample*)heap_caps_calloc(RING_CAP, sizeof(Sample), MALLOC_CAP_SPIRAM);
    const bool in_psram = (r != nullptr);
    if (!r) r = (Sample*)heap_caps_calloc(RING_CAP, sizeof(Sample), MALLOC_CAP_8BIT);
    if (r) {
      NOTICEF("NetTelemetry: ring @ %p (%u B, %s)",
              (void*)r, (unsigned)(RING_CAP * sizeof(Sample)),
              in_psram ? "PSRAM" : "internal SRAM fallback");
    }
#else
    static Sample fallback[RING_CAP] = {};
    r = fallback;
#endif
    return r;
  }
  inline size_t&   ring_count()      { static size_t   n = 0; return n; }
  inline size_t&   ring_head()       { static size_t   h = 0; return h; }
  inline uint32_t& last_sample_ms()  { static uint32_t t = 0; return t; }
  inline uint64_t& last_tx_total()   { static uint64_t v = 0; return v; }
  inline uint64_t& last_rx_total()   { static uint64_t v = 0; return v; }
  inline bool&     primed()          { static bool b = false; return b; }

  inline void push(const Sample& s) {
    Sample* r = ring();
    if (!r) return;
    r[ring_head()] = s;
    ring_head() = (ring_head() + 1) % RING_CAP;
    if (ring_count() < RING_CAP) ++ring_count();
  }
}

// Build a Sample from cumulative tx/rx byte totals (summed by the caller over
// the non-LoRa interfaces). The first call only primes the baseline — it can't
// know the rate without a prior total — and returns a zero-rate sample.
inline Sample snapshot(uint32_t now_ms, uint64_t tx_total, uint64_t rx_total) {
  Sample s = {};
  s.ts_ms = now_ms;
  if (_detail::primed()) {
    // Guard against counter resets (interface re-registered): a negative
    // delta means the totals went backwards, so report 0 for that tick.
    s.tx_bps = (tx_total >= _detail::last_tx_total())
               ? (uint32_t)(tx_total - _detail::last_tx_total()) : 0;
    s.rx_bps = (rx_total >= _detail::last_rx_total())
               ? (uint32_t)(rx_total - _detail::last_rx_total()) : 0;
  }
  _detail::last_tx_total() = tx_total;
  _detail::last_rx_total() = rx_total;
  _detail::primed()        = true;
  return s;
}

inline void encode(const Sample& s, JsonObject o) {
  o["ts"] = s.ts_ms;
  o["tx"] = s.tx_bps;
  o["rx"] = s.rx_bps;
}

// Fill a JsonArray (oldest→newest) with the entire ring — for the REST history
// a freshly-connected SPA client wants before its WS subscription catches up.
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

// Take a sample if SAMPLE_PERIOD_MS has elapsed. Returns a pointer to the
// freshly-pushed Sample (for the caller to WS-publish), or nullptr if it
// wasn't time yet. tx_total/rx_total are the cumulative non-LoRa byte counters.
inline const Sample* tick(uint32_t now_ms, uint64_t tx_total, uint64_t rx_total) {
  if (_detail::last_sample_ms() != 0
      && now_ms - _detail::last_sample_ms() < SAMPLE_PERIOD_MS) {
    return nullptr;
  }
  _detail::last_sample_ms() = now_ms;
  Sample s = snapshot(now_ms, tx_total, rx_total);
  _detail::push(s);
  Sample* r = _detail::ring();
  if (!r) return nullptr;
  const size_t idx = (_detail::ring_head() + RING_CAP - 1) % RING_CAP;
  return &r[idx];
}

inline size_t   history_size()      { return _detail::ring_count(); }
inline size_t   history_capacity()  { return RING_CAP; }
inline uint32_t sample_period_ms()  { return SAMPLE_PERIOD_MS; }

} // namespace Network
} // namespace Telemetry
