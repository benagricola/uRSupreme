// Storage::SdWriter - one shared off-loop SD writer task.
//
// Every SD write is a "job" in one priority-scheduled table, so callers never
// block on the card (SD does unpredictable multi-second housekeeping; a blocking
// write on the main loop stalls the radio, on the AsyncTCP task drops the TCP
// connection). The writer task does all blocking SD I/O behind the HSPI bus
// guard with CHECKED POSIX return codes (Arduino File::write/flush hide short
// writes and failed fsyncs).
//
// Two ways to create a job, by where the bytes are:
//   * write()  - all bytes present now (a GPX point, an attachment, a sidecar).
//                One owning copy into PSRAM, then written directly. Fire-and-
//                forget by default; an OFF-LOOP caller can pass a `done`
//                semaphore to wait for the commit.
//   * open()/feed()/finish() - bytes arrive over time (the upload). open()
//                claims a ring from the pool; feed() copies into it with
//                backpressure; the writer drains it. poll()/abort() drive it.
//
// The writer is a scheduler: after every chunk it re-picks the highest-priority
// job with a ready chunk (round-robin among equal priority), so a small write
// preempts a long upload and two streams interpolate while their producers wait
// on the network. Nothing here assumes one stream: MAX_STREAMS rings can be in
// flight at once (currently 2: an upload plus a stamp sidecar). Changing the
// count is one constant; the scheduler and ABI are already N-stream.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <new>          // placement-new for the PSRAM job table
#include <atomic>
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/stream_buffer.h>
#include <freertos/semphr.h>
#include <fcntl.h>      // POSIX ::open/O_*
#include <unistd.h>     // ::write/::fsync/::close/::lseek + SEEK_*
#include <sys/stat.h>
#include <errno.h>
#include <SHA256.h>     // rweather streaming SHA-256
#include <Log.h>        // ERRORF / WARNINGF / NOTICEF
#include "SDCard.h"

namespace Storage {
namespace SdWriter {

  // ---- ring (streaming source) tunables ----
  inline constexpr size_t   RING_TARGET     = 768 * 1024;  // preferred PSRAM ring
  inline constexpr size_t   RING_MIN        =  64 * 1024;  // floor before giving up
  inline constexpr size_t   RING_RESERVE    = 512 * 1024;  // PSRAM left free past the ring
  inline constexpr size_t   SCRATCH         =  32 * 1024;  // writer read/write unit
  inline constexpr uint32_t SEND_TIMEOUT_MS = 4000;        // producer bounded wait on a full ring
  inline constexpr uint32_t RECV_TICK_MS    = 100;         // writer park tick when only streams are idle
  inline constexpr uint32_t IDLE_TICK_MS    = 1000;        // park backstop against a lost wakeup
  inline constexpr uint32_t ABANDON_MS      = 15000;       // give up on a fed-but-silent stream (dropped client)

  // ---- job table / scheduling ----
  inline constexpr size_t   MAX_STREAMS      = 2;          // concurrent ring-backed jobs (== rings in the pool): upload + sidecar
  inline constexpr size_t   MAX_JOBS         = 8;          // total slots (streams + inline in flight)
  inline constexpr uint32_t INLINE_MAX_BYTES = 128 * 1024; // largest single write() payload
  inline constexpr uint32_t INLINE_TOTAL_BYTES = 256 * 1024; // cap on queued inline payload across all jobs
  inline constexpr uint32_t BLOB_SLOW_MS     = 250;        // head-of-line "slow job" threshold

  // Full POSIX path buffer: mountpoint + longest card-relative path. Worst case
  // /sd/lxmf/identities/<16>/attachments/<32>_<8>.bin = 94 chars; 128 leaves room.
  inline constexpr size_t   PATH_LEN         = 128;

  // Priority: higher = serviced sooner. Any uint8_t works; these are the defaults.
  inline constexpr uint8_t  PRIO_BULK   = 16;    // the streaming upload: yields to everything
  inline constexpr uint8_t  PRIO_NORMAL = 128;   // GPX / attachment / sidecar / default write()

  using Handle = uint32_t;                        // 0 = invalid
  enum class Op   : uint8_t { Truncate, AppendSeek };
  // Kinds only tag the per-kind head-of-line metric so a stall is attributable.
  enum class Kind : uint8_t { Test, Upload, GpxCreate, GpxAppend, AttachIn, AttachOut, Sidecar, _Count };

  struct Job {
    Handle   id            = 0;          // 0 = free slot
    uint8_t  priority      = PRIO_NORMAL;
    Kind     kind          = Kind::Test;
    uint32_t enqueue_ms    = 0;
    uint32_t rr_tick       = 0;          // round-robin tiebreak among equal priority
    char     path[PATH_LEN] = {0};       // full POSIX path (mount + card-relative)
    Op       op            = Op::Truncate;
    uint32_t seek_back     = 0;
    // source
    bool     streaming     = false;
    int      ring_idx      = -1;         // streaming: pool ring claimed
    const uint8_t* inbuf   = nullptr;    // inline: payload (our copy, or the caller's for zero-copy sync)
    bool     owns_buf      = false;      // inbuf was heap_caps_malloc'd by us -> free on release
    uint32_t inlen         = 0;
    uint32_t inoff         = 0;          // inline: bytes written so far
    // write state (touched only by the writer once Active, except flags below)
    int      fd            = -1;
    size_t   written       = 0;
    volatile bool ending   = false;      // streaming: producer signalled finish()
    volatile bool aborting = false;      // abort()/reclaim: discard + tear down
    bool     error         = false;
    bool     servicing     = false;      // writer is mid-write; do not free
    bool     started       = false;      // writer has serviced >=1 chunk (HoL wait recorded)
    uint32_t last_active_ms = 0;         // streaming abandon backstop
    // hashing (streaming uploads that opt in)
    bool     want_sha      = false;
    SHA256   sha;
    char     digest[65]    = {0};
    bool     digest_ready  = false;
    // completion
    bool     caller_reaps  = false;      // true: linger Done until reaped (poll/release); false: writer frees + signals
    bool     done_flag     = false;      // writer -> poll(): terminal reached (caller_reaps jobs)
    SemaphoreHandle_t done = nullptr;    // optional: an off-loop caller waits on this
    volatile bool*    ok_out = nullptr;
  };

  struct RingSlot {
    StreamBufferHandle_t handle = nullptr;
    uint8_t*             store  = nullptr;
    StaticStreamBuffer_t ctl;
    bool                 in_use = false;
  };

  struct State {
    SemaphoreHandle_t mux      = nullptr;   // guards the job table + ring pool
    SemaphoreHandle_t wake     = nullptr;   // producer -> writer: work is available
    TaskHandle_t      task     = nullptr;
    Job*              jobs      = nullptr;   // MAX_JOBS, PSRAM (placement-new'd)
    RingSlot          rings[MAX_STREAMS];
    Handle            next_id  = 1;
    uint32_t          rr_clock = 0;         // monotonic service counter for round-robin
    char              last_err[160] = {0};
    // fds of released jobs, closed by the writer OFF the table mutex (closing
    // under it would block every producer on a card op). Bounded by MAX_JOBS:
    // at most one open fd per slot can be outstanding at a time.
    int               pending_close[MAX_JOBS];
    size_t            pending_n = 0;
  };
  inline State& st() { static State s; return s; }
  inline void task_fn(void*);   // defined below; referenced by ensure()

  // ---- telemetry / head-of-line instrumentation (on /api/diag/storage) ----
  inline uint64_t& bytes_written()     { static uint64_t v = 0; return v; }
  inline uint32_t& write_errors()      { static uint32_t v = 0; return v; }
  inline uint32_t& ring_timeouts()     { static uint32_t v = 0; return v; }
  inline uint32_t& feed_max_block_ms() { static uint32_t v = 0; return v; }   // worst producer block on a full ring
  inline uint32_t& feed_slow_blocks()  { static uint32_t v = 0; return v; }
  inline uint32_t& finish_max_ms()     { static uint32_t v = 0; return v; }   // recorded by the upload finalize path
  inline uint32_t& stack_low_water()   { static uint32_t v = 0; return v; }
  inline std::atomic<uint32_t>& inline_bytes() { static std::atomic<uint32_t> v{0}; return v; }
  inline uint32_t& queue_wait_max_ms() { static uint32_t v = 0; return v; }   // since boot: head-of-line wait
  inline uint32_t& queue_wait_win_ms() { static uint32_t v = 0; return v; }   // resettable window
  inline uint32_t& mux_wait_max_ms()   { static uint32_t v = 0; return v; }   // worst table-mutex acquisition wait (windowed): a producer's stall behind a finalize op. Should stay ~0.
  inline uint32_t& queue_slow_jobs()   { static uint32_t v = 0; return v; }   // wait >= BLOB_SLOW_MS, window
  inline uint32_t& service_max_ms()    { static uint32_t v = 0; return v; }   // worst single-chunk write
  inline uint32_t& latency_max_ms()    { static uint32_t v = 0; return v; }   // wait + service
  inline uint32_t& jobs_done()         { static uint32_t v = 0; return v; }
  inline uint32_t& jobs_preempted()    { static uint32_t v = 0; return v; }   // a job serviced while a stream had data ready
  inline uint32_t& job_errors()        { static uint32_t v = 0; return v; }
  inline uint32_t& jobs_dropped()      { static uint32_t v = 0; return v; }   // create rejected at a cap
  inline uint32_t& jobs_inflight_max() { static uint32_t v = 0; return v; }
  inline uint32_t* wait_by_kind()      { static uint32_t v[(size_t)Kind::_Count] = {0}; return v; }
  inline const char* kind_name(uint8_t k) {
    switch ((Kind)k) {
      case Kind::Upload:    return "upload";
      case Kind::GpxCreate: return "gpx_create";
      case Kind::GpxAppend: return "gpx_append";
      case Kind::AttachIn:  return "attach_in";
      case Kind::AttachOut: return "attach_out";
      case Kind::Sidecar:   return "sidecar";
      default:              return "test";
    }
  }
  inline void reset_window() {
    queue_wait_win_ms() = 0;
    queue_slow_jobs()   = 0;
    mux_wait_max_ms()   = 0;
    for (size_t i = 0; i < (size_t)Kind::_Count; ++i) wait_by_kind()[i] = 0;
  }

  // Card-relative path (what callers hold, e.g. /lxmf/staging/7.bin) -> full
  // POSIX path the writer opens, by prefixing the SD mountpoint. Owning this
  // here keeps the mount knowledge out of every call site.
  inline void to_posix(char* dst, size_t cap, const char* card_path) {
    snprintf(dst, cap, "%s%s", Storage::SDCard::MOUNT, card_path);
  }

  // ---- internals (all callers below the lock helpers) ----
  inline bool take_mux() {
    const uint32_t t0 = millis();
    const bool ok = xSemaphoreTake(st().mux, portMAX_DELAY) == pdTRUE;
    const uint32_t dt = millis() - t0;
    if (dt > mux_wait_max_ms()) mux_wait_max_ms() = dt;   // a non-zero spike means a finalize op held the lock
    return ok;
  }
  inline void give_mux() { xSemaphoreGive(st().mux); }

  inline Job* find_job(Handle h) {           // caller holds mux
    if (h == 0) return nullptr;
    State& s = st();
    for (size_t i = 0; i < MAX_JOBS; ++i) if (s.jobs[i].id == h) return &s.jobs[i];
    return nullptr;
  }
  inline Job* find_free() {                  // caller holds mux; first id==0 slot
    State& s = st();
    for (size_t i = 0; i < MAX_JOBS; ++i) if (s.jobs[i].id == 0) return &s.jobs[i];
    return nullptr;
  }
  // Reset a free slot to defaults before reuse (avoids SHA256 copy-assign that a
  // whole-struct Job{} assignment would need). Caller holds mux; sets id after.
  inline void init_slot(Job& j) {
    j.priority = PRIO_NORMAL; j.kind = Kind::Test; j.enqueue_ms = 0; j.rr_tick = 0;
    j.path[0] = 0; j.op = Op::Truncate; j.seek_back = 0;
    j.streaming = false; j.ring_idx = -1; j.inbuf = nullptr; j.owns_buf = false;
    j.inlen = 0; j.inoff = 0;
    j.fd = -1; j.written = 0; j.ending = false; j.aborting = false; j.error = false;
    j.servicing = false; j.started = false; j.last_active_ms = 0;
    j.want_sha = false; j.sha.reset(); j.digest[0] = 0; j.digest_ready = false;
    j.caller_reaps = false; j.done_flag = false; j.done = nullptr; j.ok_out = nullptr;
  }

  // Release a finished/aborted job: hand off any held fd for an off-lock close,
  // return its ring, free its inline buffer, signal a waiter, mark the slot free.
  // Caller holds mux. The ring + slot are freed synchronously (memory only) so a
  // waiting open() can reclaim the ring at once; only the fd close is deferred.
  inline void release_job(Job& j) {
    if (j.fd >= 0) {
      State& s = st();
      if (s.pending_n < MAX_JOBS) s.pending_close[s.pending_n++] = j.fd;  // writer closes it off-lock
      else { Storage::SDCard::BusGuard _bg; ::close(j.fd); }              // list full (cannot happen): fall back
      j.fd = -1;
    }
    if (j.streaming && j.ring_idx >= 0) {
      xStreamBufferReset(st().rings[j.ring_idx].handle);
      st().rings[j.ring_idx].in_use = false;
      j.ring_idx = -1;
    }
    if (j.owns_buf && j.inbuf) heap_caps_free((void*)j.inbuf);   // only free buffers we allocated
    j.inbuf = nullptr; j.owns_buf = false;
    if (j.ok_out) *j.ok_out = !j.error;
    if (j.done)   xSemaphoreGive(j.done);
    j.id = 0;   // slot free
  }

  // Serialises the lazy init below so two producer tasks racing the first SD
  // write can't both create the task (two writers on one job table). Creating
  // this mutex is the one unavoidable race; a function-local static makes it
  // thread-safe via the C++ static-init guard, as everywhere else here (st()).
  inline SemaphoreHandle_t init_mux() { static SemaphoreHandle_t m = xSemaphoreCreateMutex(); return m; }

  // Lazily create the task, mutex, wake semaphore and PSRAM job table. Safe to
  // call from several tasks at once: the first does the work under init_mux, the
  // rest then see s.task already set. Returns false (and retries next call) if a
  // resource could not be allocated.
  inline bool ensure() {
    State& s = st();
    if (s.task) return true;                       // fast path: up, no lock needed
    SemaphoreHandle_t im = init_mux();
    if (im) xSemaphoreTake(im, portMAX_DELAY);
    if (!s.task) {                                 // re-check under the lock
      if (!s.mux)  s.mux  = xSemaphoreCreateMutex();
      if (!s.wake) s.wake = xSemaphoreCreateBinary();
      if (s.mux && s.wake) {
        if (!s.jobs) {
          void* mem = heap_caps_malloc(sizeof(Job) * MAX_JOBS, MALLOC_CAP_SPIRAM);
          if (mem) {
            s.jobs = static_cast<Job*>(mem);
            for (size_t i = 0; i < MAX_JOBS; ++i) new (&s.jobs[i]) Job();   // construct (SHA256 ctor)
          }
        }
        if (s.jobs) {
          // Priority 5, above loopTask (1): validated for the upload path; the
          // writer mostly blocks on the ring or the bus, so it does not starve
          // the loop. 8 KiB stack: FATFS-via-VFS + SHA-256 + snprintf.
          extern void task_fn(void*);
          const BaseType_t r = xTaskCreatePinnedToCore(
              task_fn, "sdwriter", 8192, nullptr, 5, &s.task, 1);
          if (r != pdPASS) s.task = nullptr;
          else NOTICEF("SdWriter: task up (%u job slots, %u ring(s))",
                       (unsigned)MAX_JOBS, (unsigned)MAX_STREAMS);
        }
      }
    }
    if (im) xSemaphoreGive(im);
    return s.task != nullptr;
  }

  // Claim a free ring from the pool, allocating it on first use. Caller holds
  // mux. Returns the pool index or -1 if all rings are in use (MAX_STREAMS).
  inline int claim_ring() {
    State& s = st();
    for (size_t i = 0; i < MAX_STREAMS; ++i) {
      RingSlot& r = s.rings[i];
      if (r.in_use) continue;
      if (!r.handle) {
        size_t want = RING_TARGET;
        const size_t freeblk = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        while (want > RING_MIN && want + RING_RESERVE > freeblk) want >>= 1;
        r.store = (uint8_t*)heap_caps_malloc(want + 1, MALLOC_CAP_SPIRAM);
        if (!r.store) return -1;
        r.handle = xStreamBufferCreateStatic(want, 1, r.store, &r.ctl);
        if (!r.handle) { heap_caps_free(r.store); r.store = nullptr; return -1; }
      }
      r.in_use = true;
      return (int)i;
    }
    return -1;
  }

  // ---- the scheduler ----

  // Does this job have work the writer can do right now? Caller holds mux.
  inline bool ready(const Job& j) {
    if (j.id == 0) return false;
    if (j.done_flag) return false;                     // finished, lingering for the caller to reap
    if (j.aborting) return true;                       // tear it down (serviced once -> terminal -> reaped)
    if (j.streaming) {
      if (j.error) return true;                        // close it out
      if (xStreamBufferIsEmpty(st().rings[j.ring_idx].handle) != pdTRUE) return true;  // data to drain
      return j.ending;                                 // ending + empty -> finalize
    }
    return !j.error;                                   // inline: work until written + finalized
  }

  // Pick the highest-priority ready job (round-robin among equal priority by
  // oldest service). Also trips the abandon backstop on a silent stream and
  // counts in-flight. Caller holds mux. Returns nullptr if nothing is ready.
  inline Job* pick(bool& any_stream_ready) {
    State& s = st();
    Job* best = nullptr;
    uint32_t inflight = 0;
    any_stream_ready = false;
    const uint32_t now = millis();
    for (size_t i = 0; i < MAX_JOBS; ++i) {
      Job& j = s.jobs[i];
      if (j.id == 0) continue;
      inflight++;
      // Abandon a fed-but-silent stream whose producer vanished.
      if (j.streaming && !j.ending && !j.aborting && !j.error
          && xStreamBufferIsEmpty(s.rings[j.ring_idx].handle) == pdTRUE
          && (now - j.last_active_ms) >= ABANDON_MS) {
        snprintf(s.last_err, sizeof(s.last_err),
                 "SD writer abandoned idle stream after %u ms", (unsigned)(now - j.last_active_ms));
        WARNINGF("SdWriter: %s", s.last_err);
        j.aborting = true;   // producer vanished: discard + reap, don't linger for a poll() that won't come
      }
      if (!ready(j)) continue;
      if (j.streaming && xStreamBufferIsEmpty(s.rings[j.ring_idx].handle) != pdTRUE) any_stream_ready = true;
      if (!best || j.priority > best->priority ||
          (j.priority == best->priority && j.rr_tick < best->rr_tick)) {
        best = &j;
      }
    }
    if (inflight > jobs_inflight_max()) jobs_inflight_max() = inflight;
    return best;
  }

  // Service exactly one chunk of `j` (no mux held during the SD I/O). Returns
  // true when the job has reached a terminal state (done or error).
  inline bool service(Job& j, uint8_t* scratch) {
    State& s = st();
    if (j.aborting) return true;                        // caller releases it
    // Open on the first chunk.
    if (j.fd < 0) {
      Storage::SDCard::BusGuard _bg;
      j.fd = (j.op == Op::Truncate)
                 ? ::open(j.path, O_WRONLY | O_CREAT | O_TRUNC, 0644)
                 : ::open(j.path, O_WRONLY);             // AppendSeek: file must exist
      if (j.fd >= 0 && j.op == Op::AppendSeek && ::lseek(j.fd, -(off_t)j.seek_back, SEEK_END) < 0) {
        ::close(j.fd); j.fd = -1;
      }
      if (j.fd < 0) {
        snprintf(s.last_err, sizeof(s.last_err), "SD open(%s) failed errno=%d", j.path, errno);
        ERRORF("SdWriter: %s", s.last_err);
        j.error = true; return true;
      }
    }
    // Pull one chunk from the job's source.
    size_t got = 0;
    if (j.streaming) {
      got = xStreamBufferReceive(s.rings[j.ring_idx].handle, scratch, SCRATCH, 0);  // non-blocking slice
      if (got == 0) {
        // No data: either finalize (ending) or nothing to do this pass.
        if (j.ending && xStreamBufferIsEmpty(s.rings[j.ring_idx].handle) == pdTRUE) { /* fall through to finalize */ }
        else return false;
      } else {
        j.last_active_ms = millis();
      }
    } else {
      got = (j.inlen - j.inoff) < SCRATCH ? (j.inlen - j.inoff) : SCRATCH;
    }
    // Write the chunk.
    if (got > 0) {
      const uint8_t* src = j.streaming ? scratch : (j.inbuf + j.inoff);
      size_t off = 0;
      while (off < got) {
        ssize_t w;
        { Storage::SDCard::BusGuard _bg; w = ::write(j.fd, src + off, got - off); }
        if (w <= 0) {
          write_errors()++;
          snprintf(s.last_err, sizeof(s.last_err), "SD write failed at %u errno=%d",
                   (unsigned)(j.written + off), errno);
          ERRORF("SdWriter: %s", s.last_err);
          j.error = true; return true;
        }
        off += (size_t)w;
      }
      if (j.want_sha) j.sha.update(src, off);
      j.written += off;
      bytes_written() += off;
      if (!j.streaming) j.inoff += off;
    }
    // Finished?
    const bool done = j.streaming
        ? (j.ending && xStreamBufferIsEmpty(s.rings[j.ring_idx].handle) == pdTRUE)
        : (j.inoff >= j.inlen);
    if (!done) return false;
    // Finalize: single fsync, then the digest for SHA jobs.
    { Storage::SDCard::BusGuard _bg; if (::fsync(j.fd) != 0) { write_errors()++; j.error = true; } }
    if (!j.error && j.want_sha) {
      uint8_t d[32]; j.sha.finalize(d, sizeof(d));
      for (int i = 0; i < 32; ++i) snprintf(j.digest + i * 2, 3, "%02x", d[i]);
      j.digest[64] = 0; j.digest_ready = true;
    }
    return true;
  }

  inline void task_fn(void*) {
    State& s = st();
    uint8_t* scratch = (uint8_t*)heap_caps_malloc(SCRATCH, MALLOC_CAP_SPIRAM);
    for (;;) {
      // Drain deferred fd closes OFF the table mutex: a released job hands its fd
      // here so the close never runs under the lock or on a producer's task.
      for (;;) {
        int fd = -1;
        take_mux();
        if (s.pending_n > 0) fd = s.pending_close[--s.pending_n];
        give_mux();
        if (fd < 0) break;
        { Storage::SDCard::BusGuard _bg; ::close(fd); }
      }
      // Pick the next job under the lock; mark it servicing so a concurrent
      // abort/release can't free it mid-write.
      Job* j = nullptr; bool stream_ready = false; uint8_t kind = 0; uint32_t enq = 0;
      take_mux();
      if (scratch) {
        j = pick(stream_ready);
        if (j) {
          j->servicing = true;
          j->rr_tick = ++s.rr_clock;
          kind = (uint8_t)j->kind;
          enq  = j->enqueue_ms;
          // Head-of-line wait: recorded once, the FIRST time the writer services
          // this job (enqueue -> first chunk) = how long it sat behind others.
          // For a stream this is near-immediate; it is NOT the transfer duration.
          if (!j->started) {
            j->started = true;
            const uint32_t wait_ms = millis() - enq;
            if (wait_ms > queue_wait_max_ms()) queue_wait_max_ms() = wait_ms;
            if (wait_ms > queue_wait_win_ms()) queue_wait_win_ms() = wait_ms;
            if (wait_ms >= BLOB_SLOW_MS) queue_slow_jobs()++;
            if (kind < (uint8_t)Kind::_Count && wait_ms > wait_by_kind()[kind]) wait_by_kind()[kind] = wait_ms;
            if (stream_ready && !j->streaming) jobs_preempted()++;   // preempted a stream that had data ready
          }
        }
      }
      give_mux();

      if (!j) {                                   // nothing ready: park
        xSemaphoreTake(s.wake, pdMS_TO_TICKS(IDLE_TICK_MS));
        continue;
      }

      const uint32_t t0 = millis();
      const bool terminal = service(*j, scratch);
      const uint32_t svc = millis() - t0;

      // Finalize I/O runs OFF the table mutex, while `servicing` still guards the
      // job (a concurrent release() defers to the writer instead of freeing it).
      // Producers contend for st().mux, so a card op (close, used-space scan) must
      // never run under it: that is the stall this writer exists to remove.
      if (terminal) {
        if (j->fd >= 0) { Storage::SDCard::BusGuard _bg; ::close(j->fd); j->fd = -1; }
        if (j->streaming && !j->error) Storage::SDCard::refresh_used_cache();
      }

      take_mux();
      j->servicing = false;
      if (svc > service_max_ms()) service_max_ms() = svc;        // worst single-chunk write (every chunk)
      if (terminal) {
        const uint32_t life = millis() - enq;
        if (life > latency_max_ms()) latency_max_ms() = life;    // longest job, enqueue -> done
        jobs_done()++;
        if (j->error) job_errors()++;
        if (!j->streaming && j->owns_buf) inline_bytes().fetch_sub(j->inlen, std::memory_order_relaxed);
        stack_low_water() = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
        // A caller_reaps stream (success OR error) lingers in Done until the
        // producer poll()s + release()s it: poll() then reports the real outcome
        // (1 ok / 0 failed), and the ring stays reserved so a late feed() lands in
        // this dead job's own ring, never a recycled one. Abandoned/aborted jobs
        // (the producer is gone) the writer reaps now. The fd is already closed
        // above; release_job only frees memory + signals here.
        if (j->caller_reaps && !j->aborting) {
          j->done_flag = true;
        } else {
          release_job(*j);
        }
      }
      give_mux();
    }
  }

  // ===========================================================================
  // Public API
  // ===========================================================================

  // One-shot write: all bytes present now. Copies the payload into PSRAM (so it
  // survives the caller returning) and queues it; the writer commits it off the
  // caller's task. Rejects (false, bumps jobs_dropped) at the slot or byte cap.
  // Fire-and-forget by default. An OFF-LOOP caller that needs the commit before
  // proceeding passes a binary `done` semaphore + `ok_out` and waits on it
  // (NEVER from the main loop). AppendSeek seeks `seek_back` from EOF first.
  // `card_path` is card-relative; the writer prefixes the SD mountpoint.
  inline bool write(const char* card_path, const uint8_t* data, uint32_t len,
                    Op op = Op::Truncate, uint32_t seek_back = 0, Kind kind = Kind::Test,
                    uint8_t priority = PRIO_NORMAL,
                    SemaphoreHandle_t done = nullptr, volatile bool* ok_out = nullptr) {
    if (!ensure() || len == 0 || len > INLINE_MAX_BYTES) { jobs_dropped()++; return false; }
    // Async (no `done`): the caller returns, so copy the bytes to own them, and
    // count them against our PSRAM budget. Sync (`done` given): the caller blocks
    // on `done` before it touches its buffer, so we read it in place - zero copy,
    // no budget. Whether to copy is decided here, invisibly to the caller.
    const bool owns = (done == nullptr);
    const uint8_t* src = data;
    if (owns) {
      if (inline_bytes().load(std::memory_order_relaxed) + len > INLINE_TOTAL_BYTES) { jobs_dropped()++; return false; }
      uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
      if (!buf) { jobs_dropped()++; return false; }
      memcpy(buf, data, len);
      src = buf;
    }
    State& s = st();
    take_mux();
    Job* slot = find_free();
    if (!slot) { give_mux(); if (owns) heap_caps_free((void*)src); jobs_dropped()++; return false; }
    init_slot(*slot);
    slot->id = s.next_id++; if (s.next_id == 0) s.next_id = 1;
    slot->priority = priority; slot->kind = kind; slot->enqueue_ms = millis();
    to_posix(slot->path, sizeof(slot->path), card_path);
    slot->op = op; slot->seek_back = seek_back;
    slot->streaming = false; slot->inbuf = src; slot->inlen = len; slot->owns_buf = owns;
    slot->caller_reaps = false; slot->done = done; slot->ok_out = ok_out;
    if (owns) inline_bytes().fetch_add(len, std::memory_order_relaxed);
    give_mux();
    xSemaphoreGive(s.wake);
    return true;
  }

  // Begin a streaming job (the upload). Claims a ring; returns 0 if no slot or
  // ring is free (caller falls back / 409s). `card_path` is card-relative.
  // `keep` true: the job lingers Done until the caller poll()s + release()s it
  // (uploads read the digest; the sidecar polls to confirm the write). `keep`
  // false: fire-and-forget, the writer reaps it on finish (no poll needed).
  inline Handle open(const char* card_path, Op op = Op::Truncate, uint32_t seek_back = 0,
                     Kind kind = Kind::Upload, uint8_t priority = PRIO_BULK,
                     bool want_sha = false, bool keep = true) {
    if (!ensure()) return 0;
    State& s = st();
    take_mux();
    Job* slot = find_free();
    int ring = slot ? claim_ring() : -1;
    if (!slot || ring < 0) { give_mux(); return 0; }
    init_slot(*slot);
    slot->id = s.next_id++; if (s.next_id == 0) s.next_id = 1;
    slot->priority = priority; slot->kind = kind; slot->enqueue_ms = millis();
    to_posix(slot->path, sizeof(slot->path), card_path);
    slot->op = op; slot->seek_back = seek_back;
    slot->streaming = true; slot->ring_idx = ring; slot->last_active_ms = millis();
    slot->want_sha = want_sha;
    slot->caller_reaps = keep;
    const Handle h = slot->id;
    give_mux();
    return h;
  }

  // Push bytes into a streaming job's ring. Bounded wait so a card stall fails
  // cleanly instead of wedging the producer task. False on timeout / dead job.
  inline bool feed(Handle h, const uint8_t* data, size_t len) {
    State& s = st();
    take_mux();
    Job* j = find_job(h);
    StreamBufferHandle_t ring = (j && j->streaming && !j->error) ? s.rings[j->ring_idx].handle : nullptr;
    give_mux();
    if (!ring) { snprintf(s.last_err, sizeof(s.last_err), "SdWriter: feed on dead job"); return false; }
    const uint32_t t0 = millis();
    size_t off = 0;
    while (off < len) {
      const size_t sent = xStreamBufferSend(ring, data + off, len - off, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
      off += sent;
      if (sent == 0) {
        ring_timeouts()++;
        snprintf(s.last_err, sizeof(s.last_err), "SD ring stalled at %u/%u", (unsigned)off, (unsigned)len);
        ERRORF("SdWriter: %s", s.last_err);
        take_mux(); j = find_job(h); if (j) j->error = true; give_mux();
        xSemaphoreGive(s.wake);
        return false;
      }
    }
    const uint32_t dt = millis() - t0;
    if (dt > feed_max_block_ms()) feed_max_block_ms() = dt;
    if (dt > 250) feed_slow_blocks()++;
    xSemaphoreGive(s.wake);
    return true;
  }

  inline void finish(Handle h) {
    State& s = st();
    take_mux(); Job* j = find_job(h); if (j) j->ending = true; give_mux();
    xSemaphoreGive(s.wake);
  }

  // -1 still working, 0 failed, 1 done. A terminal caller_reaps stream lingers and
  // keeps returning its outcome (0 or 1) until the caller release()s it, so a
  // failed write is never reported as success. A handle that is already gone reads
  // as 1 (nothing left to wait for).
  inline int poll(Handle h) {
    State& s = st();
    take_mux();
    Job* j = find_job(h);
    int r;
    if (!j) r = 1;                          // gone == nothing to wait for
    else if (!j->done_flag && !j->error) r = -1;
    else r = j->error ? 0 : 1;
    give_mux();
    return r;
  }

  // Read the SHA-256 hex of a finished streaming job (call after poll()==1). Does
  // NOT free the job: the caller must still release() it (the upload reads the
  // digest, then releases). Empty string if the job has no digest or failed.
  inline const char* digest_hex(Handle h) {
    State& s = st();
    static char out[65];
    take_mux();
    Job* j = find_job(h);
    out[0] = 0;
    if (j && j->digest_ready) { memcpy(out, j->digest, 65); }
    give_mux();
    return out;
  }

  // Tear down a job: a polled-done stream the caller is finished with, or a
  // discard (client gone / GC / cancel). NEVER blocks on the card and NEVER
  // spins: an idle job is freed now (memory only; its fd, if any, is queued for
  // the writer to close off-lock), and a job the writer is mid-write on is just
  // marked for the writer to reap after the current chunk. Either way the ring is
  // returned synchronously, so a waiting open() can reclaim it immediately.
  inline void release(Handle h) {
    State& s = st();
    take_mux();
    Job* j = find_job(h);
    if (!j) { give_mux(); return; }                            // already gone
    if (j->servicing) { j->aborting = true; give_mux(); xSemaphoreGive(s.wake); return; }  // writer reaps it
    release_job(*j);                                           // frees ring + slot now; fd close deferred
    give_mux();
    xSemaphoreGive(s.wake);                                    // nudge the writer to drain the deferred close
  }

  inline const char* last_error() { return st().last_err; }

}  // namespace SdWriter
}  // namespace Storage
