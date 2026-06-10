// Outbound attachment staging — holds the raw bytes between the
// chunked-HTTP upload and the LXMF Resource send.
//
// Three backends, chosen at allocate-time based on what's available:
//
//   * SdBuffer    — file at /sd/lxmf/staging/<id>.bin. Preferred when a
//                   card is mounted: zero RAM cost, multi-GB headroom,
//                   no impact on PSRAM availability for RNS state.
//   * FlashBuffer — file at /lxmf/staging/<id>.bin on LittleFS. Used
//                   when no SD is mounted and the requested size is
//                   larger than a comfortable PSRAM fraction, so the
//                   upload doesn't tip RNS containers into PSRAM
//                   exhaustion. Slower than PSRAM but slow + working
//                   beats fast + OOMing.
//   * PsramBuffer — ps_malloc'd buffer in PSRAM. Fastest, used when
//                   the upload comfortably fits.
//
// Lifecycle:
//   1. allocate(total_size) — picks backend, reserves space, returns
//      a Buffer ID the upload handler appends chunks to.
//   2. append(id, chunk_data, chunk_len) — multiple calls until total.
//   3. read(id, offset, len, dst) — used by LXMFMinimal during the
//      Resource hashmap computation + per-chunk send.
//   4. release(id) — drops the buffer, frees PSRAM / removes file.
//
// IDs are simple monotonic counters scoped to this boot — a buffer
// not released by send-completion / failure / browser-disconnect is
// garbage-collected after STAGING_TIMEOUT_MS to bound leakage.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <microStore/FileSystem.h>
#include "SDCard.h"
#include "FreeSpace.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include <fcntl.h>      // POSIX open/O_* for the checked SD write path
#include <unistd.h>     // write/fsync/close/ftruncate
#include <sys/stat.h>   // fstat
#include <errno.h>
#include <SHA256.h>     // rweather streaming SHA-256, computed by the writer as it writes

extern microStore::FileSystem filesystem;

namespace Storage {
namespace OutboundStaging {

// PSRAM allocation cap. Leaves ~4 MB headroom after RNS containers
// (~2 MB typical) + ArduinoJson docs (~0.5 MB transient). Tuned for
// the Supreme's 8 MB part; can grow later if profiling shows room.
// Backend selection criterion, not a user-facing cap.
inline constexpr size_t   PSRAM_CAP_BYTES   = 4 * 1024 * 1024;
// LittleFS staging cap. The partition is 4.4 MB total; we share it
// with inbox JSONL, attachments, identity meta, etc. Cap at 2 MB to
// leave half the partition free for everything else. Backend
// selection criterion, not a user-facing cap.
inline constexpr size_t   FLASH_CAP_BYTES   = 2 * 1024 * 1024;
// Garbage-collect untouched buffers older than this. Browser may
// upload and then disconnect; reclaim within a minute.
inline constexpr uint32_t STAGING_TIMEOUT_MS = 60000;

// SD writes are handled by a dedicated writer task (see the _sdwriter
// namespace below). The web/AsyncTCP task only memcpys upload bytes into a
// PSRAM ring; the writer drains it with checked POSIX I/O. Ring sizing and
// timeouts live in _sdwriter; there is no longer any in-line SD write loop,
// accumulator, or close/reopen tolerance layer here.

enum class Backend : uint8_t { Psram, Sd, Flash };

// ===========================================================================
// SD writer task
// ---------------------------------------------------------------------------
// Keeps SD blocking OFF the AsyncTCP/web task. Upload bytes are pushed into a
// PSRAM-backed FreeRTOS stream buffer (fast, bounded-wait); a dedicated task
// drains it to the card with POSIX I/O and CHECKED return codes.
//
// Two problems this solves, both proven on hardware:
//   * The web task holding the HSPI bus through a card stall dropped the TCP
//     connection (and stalled the shared WebSocket). Now the web task only
//     touches a RAM ring; the writer absorbs stalls.
//   * Arduino File::write()/flush() hide the underlying write()/fsync() status,
//     so a short write or a failed fsync silently corrupted the staged file.
//     POSIX write()/fsync()/fstat() return checked codes, so we KNOW.
//
// Durability is committed once, at finalize: staging data is transient,
// re-uploadable, and SHA-verified by the caller, so per-block fsync bought
// nothing but extra card stalls. On any checked failure the upload is rejected
// cleanly — a partial file is never trusted.
//
// One job at a time: uploads are serialised by the multipart handler
// (_current_upload_staging_id is a single slot), so a single global writer +
// ring is sufficient.
namespace _sdwriter {

  inline constexpr size_t   RING_TARGET     = 768 * 1024;  // preferred PSRAM ring
  inline constexpr size_t   RING_MIN        =  64 * 1024;  // floor before giving up
  inline constexpr size_t   RING_RESERVE    = 512 * 1024;  // PSRAM left free past the ring
  inline constexpr size_t   SCRATCH         =  32 * 1024;  // writer read/write unit
  inline constexpr uint32_t SEND_TIMEOUT_MS = 4000;        // web-task bounded wait on a full ring
  inline constexpr uint32_t JOIN_TIMEOUT_MS = 20000;       // finalize wait for drain + fsync
  inline constexpr uint32_t ABORT_TIMEOUT_MS = 5000;       // release/gc wait for the writer to stop
  inline constexpr uint32_t READY_TIMEOUT_MS = 5000;       // begin_job wait for the writer to park
  inline constexpr uint32_t RECV_TICK_MS    = 100;         // writer drain poll interval
  inline constexpr uint32_t ABANDON_MS      = 15000;       // give up on a fed-but-silent job (dropped client)

  struct State {
    StreamBufferHandle_t stream    = nullptr;
    uint8_t*             store     = nullptr;     // ring storage (PSRAM)
    size_t               store_sz  = 0;
    StaticStreamBuffer_t ctl;                     // ring control block (internal RAM)
    TaskHandle_t         task      = nullptr;
    SemaphoreHandle_t    start_sig = nullptr;     // web -> writer: a job is queued
    SemaphoreHandle_t    done_sig  = nullptr;     // writer -> web: job finished
    // Single active job. Plain fields: the start/done semaphores fence all
    // access (web writes them only before give(start)/after take(done); the
    // writer only between take(start) and give(done)), so no extra mutex.
    // The fencing assumes begin_job never overlaps a still-running writer —
    // enforced by waiting for `idle` (see begin_job), which closes the
    // join-timeout zombie-writer race.
    char                 path[96]  = {0};         // POSIX path, e.g. /sd/lxmf/staging/7.bin
    volatile bool        idle      = false;       // writer parked at take(start_sig)
    volatile bool        active    = false;
    volatile bool        ending    = false;       // finalize: no more data is coming
    volatile bool        aborting  = false;       // release/gc: discard + stop
    volatile bool        error     = false;
    char                 errbuf[160] = {0};
    volatile size_t      written   = 0;           // bytes write()'n to the card this job
    // SHA-256 of the upload, computed incrementally as bytes are written —
    // off the AsyncTCP task, no read-back. digest_hex is valid after a clean
    // finish() (digest_ready true). Lets the upload handler verify byte
    // integrity against a client-supplied X-SHA256 without re-reading the file.
    SHA256               sha;
    char                 digest_hex[65] = {0};
    volatile bool        digest_ready   = false;
  };
  inline State& st() { static State s; return s; }

  // Telemetry (exposed on /api/diag/mem).
  inline uint64_t& bytes_written()     { static uint64_t v = 0; return v; }
  inline uint32_t& write_errors()      { static uint32_t v = 0; return v; }
  inline uint32_t& ring_timeouts()     { static uint32_t v = 0; return v; }
  // How long the chunk handler ever blocked on a full ring (= how long the
  // AsyncTCP task was frozen by an SD stall). Headline responsiveness metric.
  inline uint32_t& feed_max_block_ms() { static uint32_t v = 0; return v; }
  inline uint32_t& feed_slow_blocks()  { static uint32_t v = 0; return v; }  // count of >250 ms blocks

  inline void task_fn(void*) {
    State& s = st();
    uint8_t* scratch = (uint8_t*)heap_caps_malloc(SCRATCH, MALLOC_CAP_SPIRAM);
    if (!scratch) scratch = (uint8_t*)malloc(SCRATCH);
    for (;;) {
      // Park until a job is queued. `idle` lets begin_job confirm the writer
      // has fully finished the previous job (given done_sig, looped back here)
      // before it mutates shared state for the next one.
      s.idle = true;
      const BaseType_t got_start = xSemaphoreTake(s.start_sig, portMAX_DELAY);
      s.idle = false;
      if (got_start != pdTRUE) continue;
      if (!scratch) {
        snprintf(s.errbuf, sizeof(s.errbuf), "SD writer: no scratch buffer");
        s.error = true; s.active = false; xSemaphoreGive(s.done_sig); continue;
      }
      // Open the file with POSIX so write/fsync/close return checked codes.
      int fd = -1;
      {
        Storage::SDCard::BusGuard _bg;
        fd = open(s.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      }
      if (fd < 0) {
        snprintf(s.errbuf, sizeof(s.errbuf), "SD open(%s) failed errno=%d", s.path, errno);
        ERRORF("OutboundStaging: %s", s.errbuf);
        s.error = true; s.active = false; xSemaphoreGive(s.done_sig); continue;
      }
      // Drain loop: pull from the ring, write to the card. All blocking SD I/O
      // happens here, on this task — never on the web/AsyncTCP task.
      uint32_t idle_ms = 0;   // time with no data and no finalize: abandoned upload?
      for (;;) {
        if (s.aborting) break;
        const size_t got = xStreamBufferReceive(s.stream, scratch, SCRATCH,
                                                 pdMS_TO_TICKS(RECV_TICK_MS));
        if (got > 0 && !s.error) {
          idle_ms = 0;
          size_t off = 0;
          while (off < got) {
            ssize_t w;
            { Storage::SDCard::BusGuard _bg; w = write(fd, scratch + off, got - off); }
            if (w <= 0) {
              write_errors()++;
              snprintf(s.errbuf, sizeof(s.errbuf),
                       "SD write failed at %u (errno=%d)",
                       (unsigned)(s.written + off), errno);
              ERRORF("OutboundStaging: %s", s.errbuf);
              s.error = true;
              break;
            }
            off += (size_t)w;
          }
          if (off) s.sha.update(scratch, off);   // hash exactly the bytes that landed
          s.written += off;
          bytes_written() += off;
        } else if (got == 0) {
          // Nothing waiting. If finalize has been requested and the ring is
          // drained, the job is complete.
          if (s.ending && xStreamBufferIsEmpty(s.stream) == pdTRUE) break;
          if (s.error && s.ending) break;   // give up once no more data is coming
          // Self-abandon backstop: a live upload feeds continuously, so a long
          // gap with no finalize means the client vanished mid-transfer
          // (dropped TCP) without a finalize/abort. Give up so the writer parks
          // and frees the fd instead of looping forever. begin_job's force-abort
          // is the primary reclaim path; this covers the no-next-upload case.
          if (!s.ending) {
            idle_ms += RECV_TICK_MS;
            if (idle_ms >= ABANDON_MS) {
              snprintf(s.errbuf, sizeof(s.errbuf), "SD writer abandoned idle job after %u ms", (unsigned)idle_ms);
              WARNINGF("OutboundStaging: %s", s.errbuf);
              s.error = true;
              break;
            }
          }
        }
      }
      // Commit + close. fsync ONCE here is the only durability barrier.
      if (!s.error && !s.aborting) {
        Storage::SDCard::BusGuard _bg;
        if (fsync(fd) != 0) {
          write_errors()++;
          snprintf(s.errbuf, sizeof(s.errbuf), "SD fsync failed (errno=%d)", errno);
          ERRORF("OutboundStaging: %s", s.errbuf);
          s.error = true;
        }
      }
      // Finalize the incremental hash on a clean job: the written bytes are now
      // durable, and sha covers exactly them, so digest_hex is the SHA-256 of
      // the staged file — no read-back needed.
      if (!s.error && !s.aborting) {
        uint8_t d[32];
        s.sha.finalize(d, sizeof(d));
        for (int i = 0; i < 32; ++i) snprintf(s.digest_hex + i * 2, 3, "%02x", d[i]);
        s.digest_hex[64] = 0;
        s.digest_ready = true;
      }
      { Storage::SDCard::BusGuard _bg; close(fd); }
      s.active = false;
      xSemaphoreGive(s.done_sig);
      // Refresh the SD free-space cache here, off the AsyncTCP task and AFTER
      // signalling done (so finalize's join never waits on the FAT scan). The
      // scan is throttled to once per 30 s internally, so most jobs skip it.
      if (!s.error && !s.aborting) Storage::SDCard::refresh_used_cache();
    }
  }

  // Lazily create the writer task + ring on first SD use. Returns false if the
  // ring/task could not be set up (caller falls back / fails the alloc).
  inline bool ensure() {
    State& s = st();
    if (s.task) return true;
    if (!s.start_sig) s.start_sig = xSemaphoreCreateBinary();
    if (!s.done_sig)  s.done_sig  = xSemaphoreCreateBinary();
    if (!s.start_sig || !s.done_sig) return false;
    if (!s.store) {
      size_t want = RING_TARGET;
      const size_t freeblk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
      while (want > RING_MIN && want + RING_RESERVE > freeblk) want >>= 1;
      // xStreamBufferCreateStatic needs (size + 1) bytes of storage.
      s.store = (uint8_t*)heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM);
      if (!s.store) return false;
      s.store_sz = want;
      s.stream = xStreamBufferCreateStatic(want, 1 /*trigger*/, s.store, &s.ctl);
      if (!s.stream) { heap_caps_free(s.store); s.store = nullptr; return false; }
    }
    const BaseType_t ok = xTaskCreatePinnedToCore(
        task_fn, "sdwriter", 4096, nullptr, 5 /*priority*/, &s.task, 1 /*core 1*/);
    if (ok != pdPASS) { s.task = nullptr; return false; }
    NOTICEF("OutboundStaging: SD writer task up, ring=%u KiB", (unsigned)(s.store_sz / 1024));
    return true;
  }

  // Begin a job writing to `posix_path`. Reclaims the writer first: uploads are
  // serialised, so if the writer is NOT parked (idle) here, its previous job was
  // orphaned — the client dropped mid-transfer and AsyncWebServer called neither
  // finalize nor release, leaving the writer looping in its drain loop forever.
  // Force-abort it so this upload can proceed; without this one dropped
  // connection wedges every subsequent upload.
  inline bool begin_job(const char* posix_path) {
    State& s = st();
    if (!ensure()) return false;
    if (s.active) s.aborting = true;   // orphaned prior job: kick the writer out of its drain loop
    // Wait for the writer to park (idle) — either it just abandoned the orphan,
    // or it is starting up for the first time.
    uint32_t waited = 0;
    while (!s.idle && waited < READY_TIMEOUT_MS) { vTaskDelay(pdMS_TO_TICKS(10)); waited += 10; }
    if (!s.idle) {
      snprintf(s.errbuf, sizeof(s.errbuf), "SD writer stuck (could not reclaim)");
      ERRORF("OutboundStaging: %s", s.errbuf);
      return false;
    }
    // Writer is parked: now it is safe to clear stale ring data + a leftover
    // done signal and set up the new job. (Reset succeeds because no task is
    // blocked on the buffer while the writer is parked at take(start_sig).)
    xStreamBufferReset(s.stream);
    while (xSemaphoreTake(s.done_sig, 0) == pdTRUE) {}
    strncpy(s.path, posix_path, sizeof(s.path) - 1);
    s.path[sizeof(s.path) - 1] = 0;
    s.ending = false; s.aborting = false; s.error = false; s.errbuf[0] = 0; s.written = 0;
    s.sha.reset(); s.digest_ready = false; s.digest_hex[0] = 0;
    s.active = true;
    xSemaphoreGive(s.start_sig);
    return true;
  }

  // Push bytes into the ring. Bounded wait so a long stall fails cleanly
  // instead of wedging the AsyncTCP task. Returns false on timeout / writer
  // error (with errbuf set).
  inline bool feed(const uint8_t* data, size_t len) {
    State& s = st();
    if (!s.active) { snprintf(s.errbuf, sizeof(s.errbuf), "SD writer: no active job"); return false; }
    const uint32_t t0 = millis();
    size_t off = 0;
    while (off < len) {
      if (s.error) return false;     // writer already failed; stop feeding
      const size_t sent = xStreamBufferSend(s.stream, data + off, len - off,
                                            pdMS_TO_TICKS(SEND_TIMEOUT_MS));
      off += sent;
      if (sent == 0) {
        if (s.error) return false;
        ring_timeouts()++;
        snprintf(s.errbuf, sizeof(s.errbuf),
                 "SD ring stalled at %u/%u (card not draining)",
                 (unsigned)off, (unsigned)len);
        ERRORF("OutboundStaging: %s", s.errbuf);
        return false;
      }
    }
    const uint32_t dt = millis() - t0;   // ~time the AsyncTCP task was blocked here
    if (dt > feed_max_block_ms()) feed_max_block_ms() = dt;
    if (dt > 250) feed_slow_blocks()++;
    return true;
  }

  // Signal end-of-data and wait for the writer to drain + fsync + close.
  // Returns true iff the whole file was written and committed cleanly.
  inline bool finish() {
    State& s = st();
    if (!s.active && !s.task) return true;   // nothing to finish (e.g. never started)
    s.ending = true;
    if (xSemaphoreTake(s.done_sig, pdMS_TO_TICKS(JOIN_TIMEOUT_MS)) != pdTRUE) {
      snprintf(s.errbuf, sizeof(s.errbuf), "SD writer join timed out");
      ERRORF("OutboundStaging: %s", s.errbuf);
      return false;
    }
    return !s.error;
  }

  // Abort the active job (client disconnected / GC). Drops buffered data and
  // waits (briefly) for the writer to close the fd so the file can be removed.
  // Short timeout: abort runs on the AsyncTCP task (via release/gc), so it must
  // not stall it the way a full join would. The writer wakes within RECV_TICK_MS
  // and finishes its current SD op (driver-capped). If it is genuinely wedged we
  // return anyway; the next begin_job's idle-wait is the real safety net.
  inline void abort_job() {
    State& s = st();
    if (!s.active) return;
    s.aborting = true;
    xSemaphoreTake(s.done_sig, pdMS_TO_TICKS(ABORT_TIMEOUT_MS));
  }

  inline const char* last_error() { return st().errbuf; }
  // SHA-256 hex of the last finished job (valid only after a clean finish()).
  inline bool        digest_ready() { return st().digest_ready; }
  inline const char* digest_hex()   { return st().digest_hex; }
}

// SHA-256 hex of the staged SD upload, computed by the writer as it wrote
// (empty for non-SD backends or a failed job). Used by the upload handler to
// verify integrity against a client X-SHA256 without re-reading the file.
inline const char* sd_digest_hex() {
  return _sdwriter::digest_ready() ? _sdwriter::digest_hex() : "";
}

struct Buffer {
  uint32_t   id          = 0;
  Backend    backend     = Backend::Psram;
  size_t     total_bytes = 0;
  size_t     written     = 0;   // how much append()'d so far
  uint32_t   created_ms  = 0;
  // PSRAM backend
  uint8_t*   psram_ptr   = nullptr;
  // SD or Flash backend (path semantics differ; underlying FS doesn't).
  // For Backend::Sd the file handle lives in the writer task, not here — this
  // Buffer only records the path (used to read the file back after finalize
  // and to remove it on release/GC).
  String     disk_path;
  microStore::File flash_file;  // Backend::Flash only
};

namespace _detail {
  inline std::vector<Buffer>& buffers() { static std::vector<Buffer> v; return v; }
  inline uint32_t&            next_id() { static uint32_t n = 1; return n; }

  inline Buffer* find(uint32_t id) {
    for (auto& b : buffers()) if (b.id == id) return &b;
    return nullptr;
  }

  // Close the Flash write handle a buffer holds open. Idempotent. SD has no
  // handle here (the writer task owns the fd); SD teardown is abort_job() +
  // SD.remove(), done in release()/gc() directly.
  inline void close_write_handle(Buffer& b) {
    if (b.backend == Backend::Flash && b.flash_file) {
      b.flash_file.close();
      b.flash_file.clear();
    }
  }

  // Drop stale buffers. Two cases:
  //   * incomplete + STAGING_TIMEOUT_MS old:   browser disconnected mid-upload.
  //   * completed + 5*STAGING_TIMEOUT_MS old:  /send was never called
  //     (page closed, validation error the user gave up on, etc).
  //     Completed buffers normally live only as long as the /send call
  //     itself takes — the StagingReleaser in LXMFMinimal::send_message
  //     drops them right after the wire bytes are built — so anything
  //     that's still around minutes later is leaked.
  inline void gc(uint32_t now) {
    auto& v = buffers();
    for (auto it = v.begin(); it != v.end(); ) {
      const bool incomplete = it->written < it->total_bytes;
      const uint32_t age    = now - it->created_ms;
      const bool stale      = incomplete ? (age > STAGING_TIMEOUT_MS)
                                         : (age > 5 * STAGING_TIMEOUT_MS);
      if (stale) {
        close_write_handle(*it);  // release the flash fd before removing the file
        if (it->backend == Backend::Psram && it->psram_ptr) {
          heap_caps_free(it->psram_ptr);
        } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
          _sdwriter::abort_job();   // stop the writer + free its fd if this upload is still live
          if (Storage::SDCard::present()) { Storage::SDCard::BusGuard _bg; SD.remove(it->disk_path); }
        } else if (it->backend == Backend::Flash && !it->disk_path.isEmpty()) {
          if (filesystem.exists(it->disk_path.c_str())) filesystem.remove(it->disk_path.c_str());
        }
        WARNINGF("OutboundStaging: GC'd stale buffer id=%u (%u/%u bytes, %s)",
                 (unsigned)it->id, (unsigned)it->written, (unsigned)it->total_bytes,
                 incomplete ? "incomplete" : "abandoned");
        it = v.erase(it);
      } else {
        ++it;
      }
    }
  }
}

// Reported via WS system_update so the SPA can gate its picker
// options + recorder duration to whatever the device can actually
// accept. Backend selection priority follows the storage-hierarchy
// rule (SD → PSRAM → Flash):
//   1. SD    if mounted — multi-GB headroom, replaceable wear medium
//   2. PSRAM if request fits — fastest, no flash wear
//   3. Flash last resort   — LittleFS partition, internal flash wear
// `max_bytes` is the largest single allocation the largest available
// backend can hold; `chosen_backend` indicates which backend that is
// for SPA display. The actual per-request backend pick happens in
// allocate() and may differ (a small upload routes to PSRAM even
// when SD is mounted).
struct Caps {
  size_t  max_bytes;
  Backend chosen_backend;
  size_t  psram_free;
  size_t  flash_free;
  size_t  sd_free;
  bool    sd_present;
  size_t  psram_max;
  size_t  flash_max;
  size_t  sd_max;
};
inline Caps current_caps() {
  Caps c{};
  c.sd_present = Storage::SDCard::present();
  c.psram_free = (size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  {
    const size_t avail = Storage::flash_free();
    c.flash_free = avail;
  }
  if (c.sd_present) {
    const uint64_t total = Storage::SDCard::total_bytes();
    const uint64_t used  = Storage::SDCard::used_bytes();
    c.sd_free = (total > used) ? (size_t)(total - used) : 0;
  }
  c.psram_max = (c.psram_free > 512 * 1024)
      ? std::min(PSRAM_CAP_BYTES, c.psram_free - 512 * 1024) : 0;
  c.sd_max    = c.sd_present ? c.sd_free : 0;
  c.flash_max = (c.flash_free > 256 * 1024)
      ? std::min(FLASH_CAP_BYTES, c.flash_free - 256 * 1024) : 0;
  // SD → PSRAM → Flash. Pick the largest-headroom backend in that
  // priority order for the displayed default; allocate() refines
  // per-request.
  if (c.sd_max > 0) {
    c.max_bytes      = c.sd_max;
    c.chosen_backend = Backend::Sd;
  } else if (c.psram_max > 0) {
    c.max_bytes      = c.psram_max;
    c.chosen_backend = Backend::Psram;
  } else {
    c.max_bytes      = c.flash_max;
    c.chosen_backend = Backend::Flash;
  }
  return c;
}

inline const char* backend_name(Backend b) {
  switch (b) {
    case Backend::Sd:    return "sd";
    case Backend::Flash: return "flash";
    default:             return "psram";
  }
}

// Allocate a new staging buffer. Returns 0 on failure (over cap,
// PSRAM exhausted, SD write-fail). The user-facing transfer cap
// (Storage::Config::effective_max_send) is enforced by the caller
// before this point; allocate() only enforces backing-store
// reality.
inline uint32_t allocate(size_t total_bytes) {
  _detail::gc(millis());
  if (total_bytes == 0) return 0;
  const Caps c = current_caps();
  if (total_bytes > c.max_bytes) {
    WARNINGF("OutboundStaging: refusing alloc — %u bytes > dynamic cap %u",
             (unsigned)total_bytes, (unsigned)c.max_bytes);
    return 0;
  }
  Buffer b;
  b.id          = _detail::next_id()++;
  b.total_bytes = total_bytes;
  b.created_ms  = millis();
  // Storage hierarchy: SD → PSRAM → Flash. SD wins whenever
  // mounted and large enough; else PSRAM if the request fits;
  // else Flash. Internal flash is last resort because its write
  // cycles wear the device — SD wear is replaceable for £5.
  if (c.sd_max >= total_bytes) {
    b.backend = Backend::Sd;
  } else if (c.psram_max >= total_bytes) {
    b.backend = Backend::Psram;
  } else if (c.flash_max >= total_bytes) {
    b.backend = Backend::Flash;
  } else {
    WARNINGF("OutboundStaging: no backend fits %u bytes (sd=%u psram=%u flash=%u)",
             (unsigned)total_bytes, (unsigned)c.sd_max,
             (unsigned)c.psram_max, (unsigned)c.flash_max);
    return 0;
  }
  if (b.backend == Backend::Psram) {
    b.psram_ptr = (uint8_t*)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);
    if (!b.psram_ptr) {
      ERRORF("OutboundStaging: PSRAM alloc of %u bytes failed", (unsigned)total_bytes);
      return 0;
    }
  } else if (b.backend == Backend::Sd) {
    b.disk_path = String("/lxmf/staging/") + b.id + ".bin";
    {
      Storage::SDCard::BusGuard _bg;   // serialise mkdir/exists against the IMU pump
      if (!SD.exists("/lxmf")) SD.mkdir("/lxmf");
      if (!SD.exists("/lxmf/staging")) SD.mkdir("/lxmf/staging");
    }
    // The dedicated writer task owns the file handle and all SD writes, off
    // the web/AsyncTCP task. Hand it the POSIX path (SD is mounted at /sd).
    const String posix = String("/sd") + b.disk_path;
    if (!_sdwriter::begin_job(posix.c_str())) {
      ERRORF("OutboundStaging: SD writer begin_job failed: %s", _sdwriter::last_error());
      return 0;
    }
  } else {  // Flash
    b.disk_path = String("/lxmf/staging/") + b.id + ".bin";
    if (filesystem.exists(b.disk_path.c_str())) filesystem.remove(b.disk_path.c_str());
    b.flash_file = filesystem.open(b.disk_path.c_str(), microStore::File::ModeAppend, true);
    if (!b.flash_file) {
      ERRORF("OutboundStaging: flash open of %s failed", b.disk_path.c_str());
      return 0;
    }
  }
  _detail::buffers().push_back(b);
  NOTICEF("OutboundStaging: allocated id=%u backend=%s size=%u",
          (unsigned)b.id, backend_name(b.backend),
          (unsigned)total_bytes);
  return b.id;
}

// Append a chunk. Returns true on success, false on overrun / unknown id.
// The overrun check uses subtraction (not addition) so we can't get
// fooled by a wrap-around on attacker-supplied `len` — the bound check
// stays correct for any size_t input.
// Human-readable detail of the most recent append() failure, surfaced to
// the SPA in the upload error response (the device serial log is garbled
// on this hardware, so the toast is the reliable diagnostic channel).
inline char* fail_detail() { static char buf[192] = {0}; return buf; }

inline bool append(uint32_t id, const uint8_t* data, size_t len) {
  Buffer* b = _detail::find(id);
  if (!b) return false;
  if (b->written > b->total_bytes) return false;                 // invariant
  if (len > b->total_bytes - b->written) {                       // overrun
    snprintf(fail_detail(), 192, "overrun off=%u total=%u len=%u",
             (unsigned)b->written, (unsigned)b->total_bytes, (unsigned)len);
    ERRORF("OutboundStaging: %s", fail_detail());
    return false;
  }
  if (b->backend == Backend::Psram) {
    if (!b->psram_ptr) return false;
    memcpy(b->psram_ptr + b->written, data, len);
  } else if (b->backend == Backend::Sd) {
    // Hand the bytes to the SD writer task's ring. This is a fast memcpy into
    // PSRAM with a bounded wait if the ring is full — it never blocks on the
    // card, so a stall can't drop the upload's TCP connection.
    if (!_sdwriter::feed(data, len)) {
      snprintf(fail_detail(), 192, "%s", _sdwriter::last_error());
      return false;
    }
  } else {  // Flash
    if (!b->flash_file) return false;
    const size_t w = b->flash_file.write(data, len);
    if (w != len) return false;
  }
  b->written += len;
  return true;
}

inline bool complete(uint32_t id) {
  Buffer* b = _detail::find(id);
  return b && b->written == b->total_bytes;
}

// Flush + close the disk write handle once the upload's final chunk has
// landed. Must run before read() (used by /send) opens the file for
// reading, so the read sees all bytes. Returns false if the final flush
// of the SD accumulator to the card failed (the staged file is then
// incomplete and the caller must reject the upload). No-op for PSRAM.
inline bool finalize_write(uint32_t id) {
  Buffer* b = _detail::find(id);
  if (!b) return true;
  if (b->backend != Backend::Sd) {
    _detail::close_write_handle(*b);  // Flash: close the handle (PSRAM: no-op)
    return true;
  }
  // SD: tell the writer no more data is coming and wait for it to drain the
  // ring, fsync, and close. finish() returns false on any checked write/fsync
  // failure or a join timeout.
  bool ok = _sdwriter::finish();
  if (!ok) {
    snprintf(fail_detail(), 192, "%s", _sdwriter::last_error());
    return false;
  }
  // Integrity gate: the on-card file must be exactly the size we fed. Read
  // size() on a FRESHLY OPENED read handle (the writer has closed its fd) —
  // VFSFileImpl::size() returns cached _stat data when stat() fails, so it is
  // only trustworthy on a fresh open. SHA-verify in the upload handler is the
  // byte-for-byte check on top of this length check.
  size_t fsz = 0;
  {
    Storage::SDCard::BusGuard _bg;
    File rf = SD.open(b->disk_path, FILE_READ);
    if (rf) { fsz = (size_t)rf.size(); rf.close(); }
  }
  if (fsz != b->written) {
    snprintf(fail_detail(), 192, "SD file size %u != written %u (writer desync)",
             (unsigned)fsz, (unsigned)b->written);
    ERRORF("OutboundStaging: %s", fail_detail());
    return false;
  }
  return true;
}

inline size_t total_bytes(uint32_t id) {
  Buffer* b = _detail::find(id);
  return b ? b->total_bytes : 0;
}

inline Backend backend_of(uint32_t id) {
  Buffer* b = _detail::find(id);
  return b ? b->backend : Backend::Psram;
}

// Read `len` bytes from `offset` into `dst`. Used by LXMFMinimal
// to compute Resource hashmaps + stream chunks during the send.
// Returns bytes actually read (may be < len at EOF).
inline size_t read(uint32_t id, size_t offset, size_t len, uint8_t* dst) {
  Buffer* b = _detail::find(id);
  if (!b) return 0;
  if (offset >= b->total_bytes) return 0;
  const size_t avail = std::min(len, b->total_bytes - offset);
  if (b->backend == Backend::Psram) {
    memcpy(dst, b->psram_ptr + offset, avail);
    return avail;
  }
  if (b->backend == Backend::Sd) {
    Storage::SDCard::BusGuard _bg;     // open/seek/read/close on the shared HSPI bus
    File f = SD.open(b->disk_path, FILE_READ);
    if (!f) { Storage::SDCard::verify_or_disable(); return 0; }
    if (!f.seek(offset)) { f.close(); Storage::SDCard::verify_or_disable(); return 0; }
    const int got = f.read(dst, avail);
    f.close();
    if (got <= 0) { Storage::SDCard::verify_or_disable(); return 0; }
    return (size_t)got;
  }
  // Flash
  microStore::File f = filesystem.open(b->disk_path.c_str(),
                                       microStore::File::ModeRead);
  if (!f) return 0;
  if (f.seek((uint32_t)offset, microStore::SeekModeSet) < 0) { f.close(); return 0; }
  const size_t got = f.read(dst, avail);
  f.close();
  return got;
}

inline void release(uint32_t id) {
  auto& v = _detail::buffers();
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (it->id != id) continue;
    _detail::close_write_handle(*it);  // release the flash fd before removing the file
    if (it->backend == Backend::Psram && it->psram_ptr) {
      heap_caps_free(it->psram_ptr);
    } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
      _sdwriter::abort_job();   // stop the writer + free its fd if this upload is still live
      if (Storage::SDCard::present()) { Storage::SDCard::BusGuard _bg; SD.remove(it->disk_path); }
    } else if (it->backend == Backend::Flash && !it->disk_path.isEmpty()) {
      if (filesystem.exists(it->disk_path.c_str())) filesystem.remove(it->disk_path.c_str());
    }
    NOTICEF("OutboundStaging: released id=%u", (unsigned)id);
    v.erase(it);
    return;
  }
}

}  // namespace OutboundStaging
}  // namespace Storage
