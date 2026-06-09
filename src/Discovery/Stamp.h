// LXStamper-compatible proof-of-work engine, shared by discovery
// announces and LXMF delivery stamps, with a background FreeRTOS
// worker so the main loop never blocks while a stamp is searched or
// validated.
//
// Algorithm — mirrors upstream LXMF/LXStamper.py + RNS/Discovery.py:
//
//   workblock = concat( HKDF(material,                 // ROUNDS × LEN
//                            salt    = SHA256(material || msgpack(n)),
//                            context = ∅,
//                            length  = LEN)
//                       for n in 0..ROUNDS-1 )
//   stamp s.t. int(SHA256(workblock || stamp)) <= (1 << (256 - cost))
//
// Two parameter sets share the construction:
//   * Discovery announces: material = SHA256(packed_msgpack_dict),
//     ROUNDS=20, LEN=32 (~640 B workblock), DEFAULT_COST=14.
//   * LXMF delivery stamps (LXStamper.py:12,49-60): material =
//     message_id (the 32-byte message hash) used directly, ROUNDS=3000,
//     LEN=256 → a 768 000-byte workblock. RNS::Bytes is PSRAM-backed on
//     this build (RNS_CONTAINER_ALLOCATOR=RNS_PSRAM_ALLOCATOR), so the
//     workblock never lands in internal SRAM, and it is freed as soon
//     as the job completes.
//
// Search strategy: SHA-256 is a streaming hash, so SHA256(workblock ||
// stamp) can be computed by hashing the workblock ONCE into a midstate
// and then, per candidate, copying the ~100-byte midstate struct and
// hashing only the 32-byte stamp. Identical output to upstream's
// full_hash(workblock+stamp) per attempt, but the per-candidate cost
// drops from ~110 ms (768 KB re-hash at ~7 MB/s) to microseconds —
// without this, a cost-8 LXMF stamp would take ~30 s and cost-14 ~30
// minutes on this CPU. The workblock expansion (3000 HKDF rounds)
// then dominates at roughly one to a few seconds.
//
// Worker model:
//   - One FreeRTOS task, single in-flight job. Discovery submissions
//     keep the original callback + single-slot result cache keyed by
//     SHA256(packed_dict). LXMF submissions are poll-based: the worker
//     posts a JobResult into a small mailbox and the LXMF gateway /
//     delivery ticks collect it with take_result_if() on the main loop
//     (the worker task must not touch RNS, the outbox spool, or the
//     WebSocket directly).
//   - submit*() returns false if the worker is busy with a different
//     job — callers defer to their next tick (FIFO order is the
//     caller's queue, not ours).
//   - cancel(key) aborts an in-flight or pending job; an aborted job
//     posts an ok=false result so pollers always see a terminal state.
//
// Watchdog: a stamp search is a tight CPU-bound loop. Without an
// explicit yield it starves IDLE0 on the pinned core, which trips
// IDLE0's WDT subscription. The search loop calls vTaskDelay(1) every
// 256 candidates, and the (multi-second for LXMF) workblock expansion
// yields every 64 HKDF rounds, so IDLE0 gets scheduling time and feeds
// its own WDT. We deliberately do NOT esp_task_wdt_add() the worker
// itself: the task spends most of its life blocked on a semaphore, and
// a subscribed-but-blocked task can't feed → 5 s blocked = abort.
// IDLE0 is the safety net for genuine hangs of the worker's CPU work.

#pragma once

#include <Arduino.h>
#include <Bytes.h>
#include <Log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <string.h>
#include <functional>
#include <deque>

#include <Identity.h>
#include <Cryptography/Hashes.h>
#include <Cryptography/HKDF.h>
#include <SHA256.h>   // rweather Crypto — same impl behind RNS full_hash; copyable for midstate

#include "../Common/MsgPack.h"

namespace Discovery {
namespace Stamp {

// Discovery-announce parameters (RNS discovery protocol).
inline constexpr uint32_t WORKBLOCK_EXPAND_ROUNDS = 20;
inline constexpr size_t   HKDF_LEN_DISCOVERY      = 32;
inline constexpr uint32_t DEFAULT_COST            = 14;
inline constexpr size_t   STAMP_SIZE              = 32;     // RNS::Identity::HASHLENGTH/8
inline constexpr size_t   WORKBLOCK_SIZE          = WORKBLOCK_EXPAND_ROUNDS * HKDF_LEN_DISCOVERY;

// LXMF delivery-stamp parameters (LXStamper.py:12 WORKBLOCK_EXPAND_ROUNDS
// = 3000; stamp_workblock hkdf length=256 → 768 000-byte workblock).
inline constexpr uint32_t LXMF_WORKBLOCK_EXPAND_ROUNDS = 3000;
inline constexpr size_t   LXMF_HKDF_LEN                = 256;

// --------------------- synchronous primitives ---------------------

// Build the workblock from the 32-byte material. Returns a Bytes
// buffer of rounds × hkdf_len bytes (PSRAM-backed), or NONE on
// allocation failure / abort. Mirrors LXStamper.stamp_workblock:
//   block_n = hkdf(length=hkdf_len, derive_from=material,
//                  salt=SHA256(material || msgpack(n)))
inline RNS::Bytes build_workblock(const RNS::Bytes& material,
                                  uint32_t rounds = WORKBLOCK_EXPAND_ROUNDS,
                                  size_t hkdf_len = HKDF_LEN_DISCOVERY,
                                  const std::function<bool()>& should_abort = {}) {
  RNS::Bytes wb;
  uint8_t* dst = wb.writable((size_t)rounds * hkdf_len);
  if (dst == nullptr) {
    ERRORF("Discovery::Stamp: workblock alloc of %u bytes failed",
           (unsigned)((size_t)rounds * hkdf_len));
    return {RNS::Bytes::NONE};
  }
  for (uint32_t n = 0; n < rounds; ++n) {
    // salt = SHA256(material || msgpack(n)). msgpack uint encoding for
    // n up to 2999 needs up to 3 bytes (0xCD uint16).
    uint8_t mp_n[4];
    const size_t mp_n_len = Common::MsgPack::pack_int(mp_n, sizeof(mp_n), (int64_t)n);
    RNS::Bytes salt_in;
    salt_in.append(material);
    salt_in.append(mp_n, mp_n_len);
    const RNS::Bytes salt = RNS::Identity::full_hash(salt_in);
    const RNS::Bytes block = RNS::Cryptography::hkdf(hkdf_len, material, salt, RNS::Bytes::NONE);
    if (block.size() != hkdf_len) return {RNS::Bytes::NONE};
    memcpy(dst + (size_t)n * hkdf_len, block.data(), hkdf_len);
    // Yield periodically: the LXMF expansion is a one-to-few-second
    // CPU-bound stretch and must not starve IDLE0 (WDT).
    if ((n & 0x3F) == 0x3F) {
      vTaskDelay(1);
      if (should_abort && should_abort()) return {RNS::Bytes::NONE};
    }
  }
  return wb;
}

// Leading-zero-bit count of a 32-byte digest — upstream
// LXStamper.stamp_value (LXStamper.py:62-71).
inline uint32_t digest_value(const uint8_t* digest32) {
  uint32_t zeros = 0;
  for (size_t i = 0; i < 32; ++i) {
    if (digest32[i] == 0) { zeros += 8; continue; }
    uint8_t b = digest32[i];
    while ((b & 0x80) == 0) { zeros++; b <<= 1; }
    break;
  }
  return zeros;
}

// Upstream LXStamper.stamp_valid (LXStamper.py:73-77): valid iff
// int(digest) <= (1 << (256 - cost)). Note the <= — a digest equal to
// the target exactly (one set bit, cost-1 leading zeros) is accepted
// upstream, so it is here too.
inline bool digest_valid(const uint8_t* digest32, uint32_t cost) {
  if (cost == 0) return true;
  if (cost > 256) return false;
  const uint32_t zeros = digest_value(digest32);
  if (zeros >= cost) return true;          // strictly below the target
  if (zeros != cost - 1) return false;
  // Equality edge: exactly the target value has a single set bit.
  uint32_t set_bits = 0;
  for (size_t i = 0; i < 32 && set_bits < 2; ++i) {
    uint8_t b = digest32[i];
    while (b) { set_bits += (b & 1); b >>= 1; }
  }
  return set_bits == 1;
}

// SHA256(workblock || stamp) without concatenating into a fresh
// buffer (the LXMF workblock is 768 KB — an append-copy would double
// the PSRAM footprint per call).
inline void hash_workblock_stamp(const RNS::Bytes& workblock,
                                 const uint8_t* stamp, size_t stamp_len,
                                 uint8_t out_digest[32]) {
  SHA256 h;
  h.reset();
  const uint8_t* p = workblock.data();
  size_t left = workblock.size();
  while (left > 0) {
    const size_t n = left < 16384 ? left : 16384;
    h.update(p, n);
    p += n;
    left -= n;
  }
  h.update(stamp, stamp_len);
  h.finalize(out_digest, 32);
}

// stamp_value / stamp_valid over a workblock — the upstream-named
// helpers, used for validation of a received stamp.
inline uint32_t stamp_value(const RNS::Bytes& workblock, const RNS::Bytes& stamp) {
  uint8_t digest[32];
  hash_workblock_stamp(workblock, stamp.data(), stamp.size(), digest);
  return digest_value(digest);
}
inline bool stamp_valid(const RNS::Bytes& stamp, uint32_t cost, const RNS::Bytes& workblock) {
  uint8_t digest[32];
  hash_workblock_stamp(workblock, stamp.data(), stamp.size(), digest);
  return digest_valid(digest, cost);
}

// Brute-force search for a 32-byte stamp satisfying digest_valid().
// `should_abort` is checked periodically so a queued cancel can exit
// cleanly instead of running to completion.
//
// Sequential search via a 64-bit counter encoded into the trailing
// 8 bytes of the stamp. Upstream uses random candidates, but for a
// memoryless target function (SHA-256) the two are equivalent on
// expectation — sequential is simpler, deterministic for tests, and
// avoids an RNG-state hot loop.
inline bool generate_search(const RNS::Bytes& workblock,
                            uint32_t cost,
                            uint8_t  out_stamp[STAMP_SIZE],
                            uint32_t* out_value,
                            const std::function<bool()>& should_abort) {
  // Midstate: hash the workblock once, then per candidate copy the
  // small SHA256 state and hash only the 32-byte stamp. See header
  // comment — same digest as full_hash(workblock+stamp), ~10^4× less
  // hashing per candidate on the LXMF-sized workblock.
  SHA256 base;
  base.reset();
  {
    const uint8_t* p = workblock.data();
    size_t left = workblock.size();
    while (left > 0) {
      const size_t n = left < 16384 ? left : 16384;
      base.update(p, n);
      p += n;
      left -= n;
    }
  }
  uint8_t stamp[STAMP_SIZE];
  memset(stamp, 0, sizeof(stamp));
  uint64_t n = 0;
  for (;;) {
    memcpy(stamp + STAMP_SIZE - 8, &n, 8);  // little-endian on ESP32
    SHA256 h(base);                          // midstate copy
    h.update(stamp, STAMP_SIZE);
    uint8_t digest[32];
    h.finalize(digest, 32);
    if (digest_valid(digest, cost)) {
      memcpy(out_stamp, stamp, STAMP_SIZE);
      if (out_value) *out_value = digest_value(digest);
      return true;
    }
    n++;
    if ((n & 0xFF) == 0) {
      // Yield to IDLE0 + other tasks pinned to this core so they get
      // scheduling time and feed their own WDT subscriptions.
      vTaskDelay(1);
      if (should_abort && should_abort()) return false;
    }
  }
}

// ------------------------- async worker --------------------------

// Discovery jobs hand their result back through this callback (runs
// on the worker task once the search finishes — guard any shared
// state inside it). LXMF jobs are poll-based instead: see JobResult /
// take_result_if below.
using DoneCb = std::function<void(const RNS::Bytes& stamp32)>;

// Terminal state of an LXMF job, collected by the submitting tick via
// take_result_if(). ok=false means the job was cancelled or failed
// (allocation); for generation, stamp+value are the found stamp; for
// validation, value/valid describe the supplied stamp.
struct JobResult {
  RNS::Bytes stamp;
  uint32_t   value = 0;
  bool       valid = false;
  bool       ok    = false;
};

namespace _detail {
  enum class JobKind : uint8_t {
    DiscoveryGenerate = 0,  // input = packed dict; material = SHA256(input); callback + cache
    LxmfGenerate      = 1,  // input = 32-byte material (message_id); mailbox result
    LxmfValue         = 2,  // input = 32-byte material; stamp supplied; mailbox result
  };
  struct Job {
    JobKind    kind = JobKind::DiscoveryGenerate;
    RNS::Bytes input;       // packed dict (discovery) or raw material (LXMF)
    RNS::Bytes stamp;       // LxmfValue only — the stamp to score
    uint32_t   cost = 0;
    uint32_t   rounds = WORKBLOCK_EXPAND_ROUNDS;
    size_t     hkdf_len = HKDF_LEN_DISCOVERY;
    DoneCb     on_done;     // DiscoveryGenerate only
  };
  inline TaskHandle_t&      task_handle()   { static TaskHandle_t h = nullptr; return h; }
  inline SemaphoreHandle_t& signal()        { static SemaphoreHandle_t s = nullptr; return s; }
  inline SemaphoreHandle_t& mutex()         { static SemaphoreHandle_t m = nullptr; return m; }
  inline Job&               pending_job()   { static Job j; return j; }
  inline bool&              has_pending()   { static bool b = false; return b; }
  inline bool&              is_busy()       { static bool b = false; return b; }
  // Cancellation state for the in-flight job. current_key is the
  // job's material; cancel() sets the flag when keys match. The flag
  // is a plain volatile read in the worker's hot loop — a benign race
  // (worst case one extra 256-candidate chunk before the abort lands).
  inline RNS::Bytes&        current_key()   { static RNS::Bytes b; return b; }
  inline volatile bool&     cancel_flag()   { static volatile bool b = false; return b; }
  // Result cache — one entry, discovery jobs only. Keyed by
  // SHA-256(packed) so resubmits with the same dict return immediately.
  // LXMF message_ids are unique per message, so caching them would only
  // evict the (useful) discovery entry.
  inline RNS::Bytes&        cache_key()     { static RNS::Bytes b; return b; }
  inline RNS::Bytes&        cache_stamp()   { static RNS::Bytes b; return b; }
  // LXMF result mailbox: (material, result) pairs the main-loop ticks
  // poll with take_result_if(). Bounded — jobs run for seconds while
  // pollers run every loop pass, so the queue should never exceed 1;
  // the cap is a leak guard if a poller dies.
  inline std::deque<std::pair<RNS::Bytes, JobResult>>& results() {
    static std::deque<std::pair<RNS::Bytes, JobResult>> r;
    return r;
  }
  inline constexpr size_t RESULTS_MAX = 8;

  inline void post_result(const RNS::Bytes& key, JobResult&& res) {
    if (xSemaphoreTake(mutex(), portMAX_DELAY) != pdTRUE) return;
    results().emplace_back(key, std::move(res));
    while (results().size() > RESULTS_MAX) results().pop_front();
    xSemaphoreGive(mutex());
  }

  inline void worker_main(void*) {
    for (;;) {
      // Wait for a request. The signal is given by submit*().
      if (xSemaphoreTake(signal(), portMAX_DELAY) != pdTRUE) continue;
      // Snapshot the job. is_busy=true means a job is in flight; new
      // submits while we work bounce off.
      Job job;
      if (xSemaphoreTake(mutex(), portMAX_DELAY) != pdTRUE) continue;
      if (!has_pending()) { xSemaphoreGive(mutex()); continue; }
      job = pending_job();
      pending_job() = Job{};   // release the queued Bytes refs promptly
      has_pending() = false;
      is_busy()     = true;
      xSemaphoreGive(mutex());

      // material: discovery hashes the packed dict; LXMF passes the
      // 32-byte message_id straight through (LXStamper.generate_stamp
      // derives from message_id directly).
      const RNS::Bytes material = (job.kind == JobKind::DiscoveryGenerate)
          ? RNS::Identity::full_hash(job.input)
          : job.input;

      if (xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE) {
        current_key() = material;
        cancel_flag() = false;
        xSemaphoreGive(mutex());
      }
      auto aborted = []() { return (bool)cancel_flag(); };

      if (job.kind == JobKind::DiscoveryGenerate) {
        // Cache check, then search — original discovery flow.
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
          const uint32_t t0 = millis();
          const RNS::Bytes wb = build_workblock(material, job.rounds, job.hkdf_len, aborted);
          uint8_t buf[STAMP_SIZE];
          const bool ok = wb.size() > 0
              && generate_search(wb, job.cost, buf, nullptr, aborted);
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
      }
      else if (job.kind == JobKind::LxmfGenerate) {
        const uint32_t t0 = millis();
        JobResult res;
        const RNS::Bytes wb = build_workblock(material, job.rounds, job.hkdf_len, aborted);
        if (wb.size() > 0) {
          uint8_t buf[STAMP_SIZE];
          uint32_t value = 0;
          if (generate_search(wb, job.cost, buf, &value, aborted)) {
            res.stamp = RNS::Bytes(buf, STAMP_SIZE);
            res.value = value;
            res.valid = true;
            res.ok    = true;
          }
        }
        const uint32_t dt = millis() - t0;
        if (res.ok) {
          NOTICEF("Stamp: LXMF stamp cost=%u value=%u generated in %u ms",
                  (unsigned)job.cost, (unsigned)res.value, (unsigned)dt);
        } else {
          NOTICEF("Stamp: LXMF stamp generation cost=%u aborted after %u ms",
                  (unsigned)job.cost, (unsigned)dt);
        }
        post_result(material, std::move(res));
      }
      else {  // JobKind::LxmfValue — score a received stamp
        const uint32_t t0 = millis();
        JobResult res;
        const RNS::Bytes wb = build_workblock(material, job.rounds, job.hkdf_len, aborted);
        if (wb.size() > 0) {
          uint8_t digest[32];
          hash_workblock_stamp(wb, job.stamp.data(), job.stamp.size(), digest);
          res.value = digest_value(digest);
          res.valid = digest_valid(digest, job.cost);
          res.ok    = true;
        }
        const uint32_t dt = millis() - t0;
        NOTICEF("Stamp: LXMF stamp validated in %u ms (cost=%u value=%u valid=%d)",
                (unsigned)dt, (unsigned)job.cost, (unsigned)res.value, (int)res.valid);
        post_result(material, std::move(res));
      }

      if (xSemaphoreTake(mutex(), portMAX_DELAY) == pdTRUE) {
        current_key() = RNS::Bytes();
        cancel_flag() = false;
        is_busy() = false;
        xSemaphoreGive(mutex());
      }
    }
  }

  // Common submit tail: stash the job + wake the worker. Caller holds
  // nothing; returns false when the worker is occupied.
  inline bool submit_job(Job&& job) {
    if (xSemaphoreTake(mutex(), pdMS_TO_TICKS(50)) != pdTRUE) return false;
    if (is_busy() || has_pending()) {
      xSemaphoreGive(mutex());
      return false;
    }
    pending_job() = std::move(job);
    has_pending() = true;
    xSemaphoreGive(mutex());
    xSemaphoreGive(signal());
    return true;
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

// Submit a discovery-announce job. Returns true if accepted. Returns
// false if the worker is currently busy with a different material —
// caller should retry on the next tick. A resubmit of the same
// material that's already cached will succeed and fire the callback
// synchronously here (no task hop) so the Announcer doesn't wait an
// extra tick for a no-op.
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
  xSemaphoreGive(_detail::mutex());
  _detail::Job job;
  job.kind     = _detail::JobKind::DiscoveryGenerate;
  job.input    = packed_dict;
  job.cost     = cost;
  job.rounds   = WORKBLOCK_EXPAND_ROUNDS;
  job.hkdf_len = HKDF_LEN_DISCOVERY;
  job.on_done  = std::move(on_done);
  return _detail::submit_job(std::move(job));
}

// Submit an LXMF delivery-stamp generation (LXStamper.generate_stamp:
// material = message_id, 3000-round workblock). Result arrives via
// take_result_if(message_id, ...). Returns false when the worker is
// busy — retry next tick.
inline bool submit_lxmf_generate(const RNS::Bytes& message_id, uint32_t cost) {
  if (!_detail::task_handle()) start();
  if (!_detail::mutex()) return false;
  _detail::Job job;
  job.kind     = _detail::JobKind::LxmfGenerate;
  job.input    = message_id;
  job.cost     = cost;
  job.rounds   = LXMF_WORKBLOCK_EXPAND_ROUNDS;
  job.hkdf_len = LXMF_HKDF_LEN;
  return _detail::submit_job(std::move(job));
}

// Submit an LXMF delivery-stamp validation (LXMessage.validate_stamp:
// workblock from the message hash, then stamp_valid + stamp_value).
// Result arrives via take_result_if(message_id, ...).
inline bool submit_lxmf_value(const RNS::Bytes& message_id,
                              const RNS::Bytes& stamp,
                              uint32_t cost) {
  if (!_detail::task_handle()) start();
  if (!_detail::mutex()) return false;
  _detail::Job job;
  job.kind     = _detail::JobKind::LxmfValue;
  job.input    = message_id;
  job.stamp    = stamp;
  job.cost     = cost;
  job.rounds   = LXMF_WORKBLOCK_EXPAND_ROUNDS;
  job.hkdf_len = LXMF_HKDF_LEN;
  return _detail::submit_job(std::move(job));
}

// Abort the job whose material key matches (mirrors LXStamper.cancel_work).
// A pending-but-unstarted job is dropped immediately; an in-flight job
// gets its abort flag set and unwinds at the next yield point. Either
// way an ok=false result is posted so pollers see a terminal state.
inline void cancel(const RNS::Bytes& material) {
  if (!_detail::mutex()) return;
  if (xSemaphoreTake(_detail::mutex(), pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (_detail::has_pending()) {
    const _detail::Job& pj = _detail::pending_job();
    const RNS::Bytes key = (pj.kind == _detail::JobKind::DiscoveryGenerate)
        ? RNS::Identity::full_hash(pj.input)
        : pj.input;
    if (key == material) {
      _detail::pending_job() = _detail::Job{};
      _detail::has_pending() = false;
      _detail::results().emplace_back(material, JobResult{});  // ok=false
      while (_detail::results().size() > _detail::RESULTS_MAX) _detail::results().pop_front();
    }
  }
  if (_detail::current_key() == material) _detail::cancel_flag() = true;
  xSemaphoreGive(_detail::mutex());
}

// Collect a completed LXMF job result, but only if it belongs to `key`
// — two independent pollers (outbound generation, inbound validation)
// share the mailbox and must not steal each other's results.
inline bool take_result_if(const RNS::Bytes& key, JobResult& out) {
  if (!_detail::mutex()) return false;
  if (xSemaphoreTake(_detail::mutex(), pdMS_TO_TICKS(10)) != pdTRUE) return false;
  bool found = false;
  auto& q = _detail::results();
  for (auto it = q.begin(); it != q.end(); ++it) {
    if (it->first == key) {
      out = std::move(it->second);
      q.erase(it);
      found = true;
      break;
    }
  }
  xSemaphoreGive(_detail::mutex());
  return found;
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
