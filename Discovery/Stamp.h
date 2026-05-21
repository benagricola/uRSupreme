// LXStamper-compatible proof-of-work for discovery announces, with a
// background FreeRTOS worker so the main loop never blocks while the
// stamp is searched.
//
// Algorithm — mirrors upstream LXMF/LXStamper.py + RNS/Discovery.py:
//
//   material  = SHA256(packed_msgpack_dict)            // 32 B
//   workblock = concat( HKDF(material,                 // ROUNDS × 32 B
//                            salt    = SHA256(material || msgpack(n)),
//                            context = ∅,
//                            length  = 32)
//                       for n in 0..ROUNDS-1 )
//   stamp s.t. int(SHA256(workblock || stamp)) <= (1 << (256 - cost))
//
// For discovery: ROUNDS=20 (~640 B workblock), DEFAULT_COST=14 (≈16k
// SHA256 invocations expected). On the S3 with mbedtls SHA at ~7 MB/s,
// that lands around 1-2 s, but the variance is wide (tail behaviour
// is geometric — a 14-bit search can take ≫10× longer on a bad seed).
// Even the expected case is too long for the main loop, hence the
// background worker.
//
// Worker model:
//   - One FreeRTOS task, single in-flight job, single-slot result
//     cache keyed by SHA256(packed_dict). Submitting the same material
//     twice (e.g. a retry after a transient failure) reuses the cached
//     stamp instead of recomputing.
//   - submit() returns false if the worker is busy with a different
//     job — callers (the Announcer) defer to their next tick.
//   - When done, the worker invokes the caller's on_done() inside the
//     worker task. The Announcer's callback grabs rns_lock before
//     touching the destination.
//
// Watchdog: the search loop feeds the task watchdog every 1024
// iterations. A 14-bit search at ~50 µs/try takes ~0.8 s in the median
// — well under the default 5 s WDT window — but adding the feed makes
// the worst-case-bad-seed (≈20× median) safe.

#pragma once

#include <Arduino.h>
#include <Bytes.h>
#include <Log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <stdint.h>
#include <string.h>
#include <functional>

#include <Identity.h>
#include <Cryptography/Hashes.h>
#include <Cryptography/HKDF.h>

#include "../Common/MsgPack.h"

namespace Discovery {
namespace Stamp {

inline constexpr int      WORKBLOCK_EXPAND_ROUNDS = 20;
inline constexpr uint32_t DEFAULT_COST            = 14;
inline constexpr size_t   STAMP_SIZE              = 32;     // RNS::Identity::HASHLENGTH/8
inline constexpr size_t   WORKBLOCK_SIZE          = WORKBLOCK_EXPAND_ROUNDS * 32;

// --------------------- synchronous primitives ---------------------

// Build the workblock from the 32-byte material hash. Returns a Bytes
// buffer of WORKBLOCK_SIZE bytes.
inline RNS::Bytes build_workblock(const RNS::Bytes& material) {
  RNS::Bytes wb;
  for (int n = 0; n < WORKBLOCK_EXPAND_ROUNDS; ++n) {
    // salt = SHA256(material || msgpack(n))
    uint8_t mp_n[2];
    size_t  mp_n_len = Common::MsgPack::pack_int(mp_n, sizeof(mp_n), n);
    RNS::Bytes salt_in;
    salt_in.append(material);
    salt_in.append(mp_n, mp_n_len);
    const RNS::Bytes salt = RNS::Identity::full_hash(salt_in);
    // hkdf(length=32, derive_from=material, salt=salt, context=NONE)
    wb.append(RNS::Cryptography::hkdf(32, material, salt, RNS::Bytes::NONE));
  }
  return wb;
}

// True iff SHA256(workblock || stamp) has at least `cost` leading
// zero bits. Equivalent to the upstream "int(hash) <= (1 << (256 -
// cost))" check via a count-leading-zeros short-circuit.
inline bool valid(const uint8_t* stamp32, const RNS::Bytes& workblock, uint32_t cost) {
  if (cost == 0) return true;
  RNS::Bytes in;
  in.append(workblock);
  in.append(stamp32, STAMP_SIZE);
  const RNS::Bytes h = RNS::Identity::full_hash(in);
  const uint8_t* p = h.data();
  uint32_t zeros = 0;
  for (size_t i = 0; i < h.size() && zeros < cost; ++i) {
    if (p[i] == 0) { zeros += 8; continue; }
    uint8_t b = p[i];
    while ((b & 0x80) == 0) { zeros++; b <<= 1; }
    break;
  }
  return zeros >= cost;
}

// Brute-force search for a 32-byte stamp satisfying valid().
// `should_abort` is checked once per outer chunk so a queued cancel
// can exit cleanly instead of running to completion.
//
// Sequential search via a 64-bit counter encoded into the trailing
// 8 bytes of the stamp. Upstream uses random candidates, but for a
// memoryless target function (SHA-256) the two are equivalent on
// expectation — sequential is simpler, deterministic for tests, and
// avoids an RNG-state hot loop.
inline bool generate(const RNS::Bytes& material,
                     uint32_t cost,
                     uint8_t  out_stamp[STAMP_SIZE],
                     const std::function<bool()>& should_abort) {
  const RNS::Bytes wb = build_workblock(material);
  uint8_t stamp[STAMP_SIZE];
  memset(stamp, 0, sizeof(stamp));
  uint64_t n = 0;
  // The fast path: cost == 0 means accept any stamp. The wire format
  // still needs 32 bytes, so emit zeroes.
  if (cost == 0) {
    memset(out_stamp, 0, STAMP_SIZE);
    return true;
  }
  while (true) {
    memcpy(stamp + STAMP_SIZE - 8, &n, 8);  // little-endian on ESP32
    if (valid(stamp, wb, cost)) {
      memcpy(out_stamp, stamp, STAMP_SIZE);
      return true;
    }
    n++;
    if ((n & 0x3FF) == 0) {
      esp_task_wdt_reset();
      if (should_abort && should_abort()) return false;
    }
  }
}

// ------------------------- async worker --------------------------

// A submitted job. on_done runs on the worker task once the search
// finishes — guard any shared state inside it. material is the
// packed_msgpack bytes; the worker takes a SHA-256 of it to produce
// the 32-byte LXStamper "material" input.
using DoneCb = std::function<void(const RNS::Bytes& stamp32)>;

namespace _detail {
  struct Job {
    RNS::Bytes packed;      // msgpack dict bytes (what gets hashed → material)
    uint32_t   cost;
    DoneCb     on_done;
  };
  inline TaskHandle_t&     task_handle()   { static TaskHandle_t h = nullptr; return h; }
  inline SemaphoreHandle_t& signal()       { static SemaphoreHandle_t s = nullptr; return s; }
  inline SemaphoreHandle_t& mutex()        { static SemaphoreHandle_t m = nullptr; return m; }
  inline Job&              pending_job()   { static Job j; return j; }
  inline bool&             has_pending()   { static bool b = false; return b; }
  inline bool&             is_busy()       { static bool b = false; return b; }
  // Result cache — one entry. Keyed by SHA-256(packed) so resubmits
  // with the same dict return immediately. We don't bother with an LRU;
  // discovery announces don't churn material fast enough to need it.
  inline RNS::Bytes&       cache_key()     { static RNS::Bytes b; return b; }
  inline RNS::Bytes&       cache_stamp()   { static RNS::Bytes b; return b; }

  inline void worker_main(void*) {
    esp_task_wdt_add(nullptr);  // join the WDT subscriber list
    for (;;) {
      // Wait for a request. The signal is given by submit().
      if (xSemaphoreTake(signal(), portMAX_DELAY) != pdTRUE) continue;
      // Snapshot the job. is_busy=true means a job is in flight; new
      // submits while we work bounce off.
      Job job;
      if (xSemaphoreTake(mutex(), portMAX_DELAY) != pdTRUE) continue;
      if (!has_pending()) { xSemaphoreGive(mutex()); continue; }
      job = pending_job();
      has_pending() = false;
      is_busy()     = true;
      xSemaphoreGive(mutex());

      // Cache check. material == SHA256(packed).
      const RNS::Bytes material = RNS::Identity::full_hash(job.packed);
      RNS::Bytes stamp_out;
      bool from_cache = false;
      if (xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE) {
        if (cache_key().size() == material.size()
            && memcmp(cache_key().data(), material.data(), material.size()) == 0
            && cache_stamp().size() == STAMP_SIZE) {
          stamp_out  = cache_stamp();
          from_cache = true;
        }
        xSemaphoreGive(mutex());
      }

      if (!from_cache) {
        uint8_t buf[STAMP_SIZE];
        const uint32_t t0 = millis();
        const bool ok = generate(material, job.cost, buf, /*should_abort*/{});
        const uint32_t dt = millis() - t0;
        if (ok) {
          stamp_out = RNS::Bytes(buf, STAMP_SIZE);
          NOTICEF("Discovery::Stamp: solved cost=%u in %u ms",
                  (unsigned)job.cost, (unsigned)dt);
          if (xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE) {
            cache_key()   = material;
            cache_stamp() = stamp_out;
            xSemaphoreGive(mutex());
          }
        } else {
          WARNINGF("Discovery::Stamp: search aborted after %u ms", (unsigned)dt);
        }
      } else {
        NOTICEF("Discovery::Stamp: cache hit for cost=%u", (unsigned)job.cost);
      }

      // Hand the result off. Callback may be empty if the job was
      // submitted with no callback (fire-and-cache pattern).
      if (job.on_done && stamp_out.size() == STAMP_SIZE) job.on_done(stamp_out);

      if (xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE) {
        is_busy() = false;
        xSemaphoreGive(mutex());
      }
      esp_task_wdt_reset();
    }
  }
}  // namespace _detail

// Create the worker. Idempotent; safe to call from setup() repeatedly.
inline void start() {
  if (_detail::task_handle()) return;
  // xSemaphoreCreate* are macros that return-from-call-expression; the
  // ref-returning accessor can't be the lvalue. Take a local and
  // copy in.
  SemaphoreHandle_t sig = xSemaphoreCreateBinary();
  SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
  _detail::signal() = sig;
  _detail::mutex()  = mtx;
  // Pin to core 0 (PRO_CPU); APP_CPU runs Arduino's loop + AsyncTCP
  // — leaving the announcer's compute side off the app core means
  // a long-running stamp search can't starve the WebSocket / loop.
  // Priority 1: just above the IDLE task, well below the radio /
  // AsyncTCP tasks so we never preempt time-critical work.
  xTaskCreatePinnedToCore(
      _detail::worker_main,
      "stamp",
      6144,
      nullptr,
      1,
      &_detail::task_handle(),
      0  // PRO_CPU
  );
}

// Submit a job. Returns true if accepted. Returns false if the worker
// is currently busy with a different material — caller should retry
// on the next tick. A resubmit of the same material that's already
// cached will succeed and fire the callback synchronously here (no
// task hop) so the Announcer doesn't wait an extra tick for a no-op.
inline bool submit(const RNS::Bytes& packed_dict, uint32_t cost, DoneCb on_done) {
  if (!_detail::task_handle()) start();
  if (!_detail::mutex()) return false;
  if (xSemaphoreTake(_detail::mutex(), pdMS_TO_TICKS(50)) != pdTRUE) return false;
  // Fast path: cached. Hand the result back inline to skip the task hop.
  const RNS::Bytes material = RNS::Identity::full_hash(packed_dict);
  if (_detail::cache_key().size() == material.size()
      && memcmp(_detail::cache_key().data(), material.data(), material.size()) == 0
      && _detail::cache_stamp().size() == STAMP_SIZE) {
    RNS::Bytes cached = _detail::cache_stamp();
    xSemaphoreGive(_detail::mutex());
    if (on_done) on_done(cached);
    return true;
  }
  if (_detail::is_busy() || _detail::has_pending()) {
    xSemaphoreGive(_detail::mutex());
    return false;
  }
  _detail::pending_job() = { packed_dict, cost, std::move(on_done) };
  _detail::has_pending() = true;
  xSemaphoreGive(_detail::mutex());
  xSemaphoreGive(_detail::signal());
  return true;
}

// Snapshot for /api/discovery/state. Lets the SPA show whether a
// search is in flight + whether the most recent material is cached.
struct Status {
  bool busy;
  bool cached;
};
inline Status status() {
  Status s = {false, false};
  if (!_detail::mutex()) return s;
  if (xSemaphoreTake(_detail::mutex(), pdMS_TO_TICKS(10)) != pdTRUE) return s;
  s.busy   = _detail::is_busy() || _detail::has_pending();
  s.cached = _detail::cache_stamp().size() == STAMP_SIZE;
  xSemaphoreGive(_detail::mutex());
  return s;
}

}  // namespace Stamp
}  // namespace Discovery
