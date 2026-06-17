// Storage::SdWriter - one shared off-loop SD writer task.
//
// Lifted from OutboundStaging's internal _sdwriter so every SD-write path can
// share a single task instead of each feature spawning its own. This first
// commit is a pure relocation: the streaming upload path is the byte-for-byte
// same writer, and OutboundStaging now calls Storage::SdWriter:: in place of
// its own _sdwriter::. The discrete-blob job queue and the main-loop interleave
// land in the follow-up commit; nothing about upload behaviour changes here.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include <fcntl.h>      // POSIX open/O_* for the checked SD write path
#include <unistd.h>     // write/fsync/close
#include <sys/stat.h>   // fstat
#include <errno.h>
#include <SHA256.h>     // rweather streaming SHA-256, computed by the writer as it writes
#include <Log.h>        // ERRORF / WARNINGF / NOTICEF
#include "SDCard.h"

namespace Storage {
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
// cleanly - a partial file is never trusted.
//
// One job at a time: uploads are serialised by the multipart handler
// (_current_upload_staging_id is a single slot), so a single global writer +
// ring is sufficient.
namespace SdWriter {

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
    // The fencing assumes begin_job never overlaps a still-running writer -
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
    // SHA-256 of the upload, computed incrementally as bytes are written -
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
  // Worst deferred-finalize duration observed, ms: from the final handler
  // parking the request to the drain answering it (drain + fsync + close +
  // verification). Recorded by drain_upload_finalize in conversations.h.
  inline uint32_t& finish_max_ms()     { static uint32_t v = 0; return v; }
  // Writer-task minimum free stack (bytes), sampled after each job. The task
  // runs FATFS-via-VFS + SHA-256; if this trends toward zero, raise the stack.
  inline uint32_t& stack_low_water()   { static uint32_t v = 0; return v; }

  inline void task_fn(void*) {
    State& s = st();
    // PSRAM only: falling back to a 32 KiB internal-SRAM allocation on this
    // SRAM-starved device would trade an upload failure for WiFi-MAC
    // starvation. No scratch -> the job errors out cleanly per-job below.
    uint8_t* scratch = (uint8_t*)heap_caps_malloc(SCRATCH, MALLOC_CAP_SPIRAM);
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
      // happens here, on this task - never on the web/AsyncTCP task.
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
      // the staged file - no read-back needed.
      if (!s.error && !s.aborting) {
        uint8_t d[32];
        s.sha.finalize(d, sizeof(d));
        for (int i = 0; i < 32; ++i) snprintf(s.digest_hex + i * 2, 3, "%02x", d[i]);
        s.digest_hex[64] = 0;
        s.digest_ready = true;
      }
      { Storage::SDCard::BusGuard _bg; close(fd); }
      stack_low_water() = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
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
    // Priority 5, above loopTask (1), and validated: an equal-priority
    // experiment (writer at 1, round-robinning the core with the main
    // loop) correlated with deterministic upload write failures on the
    // rig and was reverted. The writer mostly blocks on the stream
    // buffer or the SD bus, so the higher priority does not starve the
    // main loop in practice. Re-tune only with /api/diag/loop
    // max-iteration numbers from a sustained large upload.
    // 8 KiB stack: the task runs FATFS-via-VFS + SHA-256 + snprintf; 4 KiB
    // left ~no headroom (see sd_writer_stack_free in /api/diag/storage).
    const BaseType_t ok = xTaskCreatePinnedToCore(
        task_fn, "sdwriter", 8192, nullptr, 5 /*priority*/, &s.task, 1 /*core 1*/);
    if (ok != pdPASS) { s.task = nullptr; return false; }
    NOTICEF("OutboundStaging: SD writer task up, ring=%u KiB", (unsigned)(s.store_sz / 1024));
    return true;
  }

  // Begin a job writing to `posix_path`. Reclaims the writer first: uploads are
  // serialised, so if the writer is NOT parked (idle) here, its previous job was
  // orphaned - the client dropped mid-transfer and AsyncWebServer called neither
  // finalize nor release, leaving the writer looping in its drain loop forever.
  // Force-abort it so this upload can proceed; without this one dropped
  // connection wedges every subsequent upload.
  inline bool begin_job(const char* posix_path) {
    State& s = st();
    if (!ensure()) return false;
    if (s.active) s.aborting = true;   // orphaned prior job: kick the writer out of its drain loop
    // Wait for the writer to park (idle) - either it just abandoned the orphan,
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

  // Non-blocking finalize. request_finish() signals end-of-data; the caller
  // then polls finish_poll() until it reports done. The old blocking join
  // (xSemaphoreTake for up to 20 s) ran on the AsyncTCP task and froze it
  // for the whole drain+fsync; on cold first uploads that overflowed
  // AsyncTCP's event queue and the connection dropped without a response
  // (measured on the rig: first-after-boot uploads reliably died there).
  inline void request_finish() { st().ending = true; }
  // -1 = still draining, 0 = failed (errbuf set), 1 = drained+fsynced+closed.
  inline int finish_poll() {
    State& s = st();
    if (!s.task) return 1;   // never started: nothing to wait for
    if (xSemaphoreTake(s.done_sig, 0) != pdTRUE) return -1;
    return s.error ? 0 : 1;
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

}  // namespace Storage
