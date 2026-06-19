#pragma once

// Off-loop receive path.
//
// microReticulum hands two kinds of work to one core-0 worker so the main loop
// (loopTask, core 1) never blocks on the SD card during a receive - which would
// starve LoRa servicing and stall the web UI:
//
//   * PART WRITES: Resource::on_part copies each received part and enqueues
//     (path, offset, bytes) here instead of writing it to SD inline. The worker
//     does the seek+write off-loop. This is what lets a fast TCP backbone feed a
//     large attachment without the per-part SD writes saturating the loop
//     (auth/radio go unresponsive when they run inline).
//
//   * CONCLUDE: when all parts are in, Resource hands the whole receive here;
//     the worker reads the assembled file back (chunked, off-loop) and
//     decrypts+verifies it, then loopTask runs the fast RNS-mutating delivery
//     (proof + concluded callback).
//
// One FIFO queue feeds the worker, so all of a resource's part-writes are
// serviced before its conclude - the conclude read therefore sees a complete
// file with no cross-task barrier. The worker owns ALL file I/O for a deferred
// resource via raw POSIX on the path (one task, one fd at a time): no second
// open handle races it (that corrupted files on this hardware).
//
// Lifecycle: a Conclude job holds a Resource copy (its shared_ptr keeps the
// object + Link alive) plus the detached receive buffer. A cancel()/link
// teardown can free neither under the worker; deliver_assembly() drops a
// resource failed meanwhile. Part-write jobs reference only a copied path +
// PSRAM byte copy, so they are immune to the resource being torn down.

#include <Arduino.h>
#include <new>
#include <memory>
#include <algorithm>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>

#include <Resource.h>
#include <ResourceBuffer.h>
#include <Bytes.h>
#include <Log.h>

#include "Storage/SDCard.h"

namespace RnsConclude {

  enum class Kind : uint8_t { WritePart, Conclude };

  inline constexpr size_t   PATH_LEN    = 96;
  inline constexpr size_t   READ_CHUNK  = 16 * 1024;   // per-BusGuard read slice (~20 ms @ 4 MHz)
  inline constexpr UBaseType_t QUEUE_DEPTH = 48;       // FIFO slots: a window of part-writes + concludes
  // In-flight PSRAM copies for queued part-writes. The receiver window byte-cap
  // (Type.h RECV_MAX_INFLIGHT_BYTES) keeps real occupancy well under this; the
  // cap is the backstop that forces an inline write rather than dropping a part.
  inline constexpr uint32_t INFLIGHT_CAP = 512 * 1024;

  struct Job {
    Kind kind;
    // WritePart:
    char     path[PATH_LEN] = {0};
    uint32_t offset = 0;
    uint8_t* data   = nullptr;   // PSRAM copy, freed by the worker after the write
    uint32_t len    = 0;
    // Conclude (heap-held so a WritePart Job needs no Resource - which has no
    // default constructor; the Resource copy keeps the object + Link alive):
    std::unique_ptr<RNS::Resource>       resource;
    std::unique_ptr<RNS::ResourceBuffer> buffer;
  };

  inline QueueHandle_t& q()              { static QueueHandle_t v = nullptr; return v; }
  inline QueueHandle_t& deliver_q()      { static QueueHandle_t v = nullptr; return v; }
  inline std::atomic<uint32_t>& inflight_bytes() { static std::atomic<uint32_t> v{0}; return v; }
  // Diagnostics (/api/diag/storage).
  inline uint32_t& jobs_done()           { static uint32_t v = 0; return v; }
  inline uint32_t& read_max_us()         { static uint32_t v = 0; return v; }   // worst chunked conclude read
  inline uint32_t& write_max_us()        { static uint32_t v = 0; return v; }   // worst single part write
  inline uint32_t& fallbacks()           { static uint32_t v = 0; return v; }   // inline part-write (queue full / no PSRAM)
  inline uint32_t& write_errors()        { static uint32_t v = 0; return v; }
  inline uint32_t& stack_low_water()     { static uint32_t v = 0; return v; }

  // --- worker-local open write fd (one deferred resource written at a time) ---
  inline int&  cur_fd()   { static int v = -1; return v; }
  inline char* cur_path() { static char v[PATH_LEN] = {0}; return v; }

  inline void close_cur_fd() {
    if (cur_fd() >= 0) { Storage::SDCard::BusGuard _bg; ::close(cur_fd()); cur_fd() = -1; }
    cur_path()[0] = 0;
  }

  // Raw seek+write of one part, off-loop. Keeps the fd open across a resource's
  // parts; reopens when the path changes (a new resource started writing).
  inline void do_write(const Job& j) {
    if (cur_fd() < 0 || strncmp(cur_path(), j.path, PATH_LEN) != 0) {
      close_cur_fd();
      Storage::SDCard::BusGuard _bg;
      cur_fd() = ::open(j.path, O_WRONLY | O_CREAT, 0644);
      if (cur_fd() < 0) { write_errors()++; ERRORF("RnsConclude: open(%s) failed errno=%d", j.path, errno); return; }
      strncpy(cur_path(), j.path, PATH_LEN - 1); cur_path()[PATH_LEN - 1] = 0;
    }
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    Storage::SDCard::BusGuard _bg;
    if (::lseek(cur_fd(), (off_t)j.offset, SEEK_SET) < 0) { write_errors()++; ERRORF("RnsConclude: lseek %u failed", j.offset); return; }
    uint32_t off = 0;
    while (off < j.len) {
      const ssize_t w = ::write(cur_fd(), j.data + off, j.len - off);
      if (w <= 0) { write_errors()++; ERRORF("RnsConclude: write at %u failed errno=%d", j.offset + off, errno); return; }
      off += (uint32_t)w;
    }
    const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
    if (dt > write_max_us()) write_max_us() = dt;
  }

  // Read the whole assembled file back, off-loop, in BusGuard-bounded chunks.
  inline RNS::Bytes read_back(const char* path, size_t total) {
    RNS::Bytes out;
    if (!path || total == 0) return out;
    uint8_t* dst = out.writable(total);   // RNS allocator -> PSRAM
    if (!dst) { ERRORF("RnsConclude: read_back alloc %zu failed", total); return RNS::Bytes(); }
    int fd;
    { Storage::SDCard::BusGuard _bg; fd = ::open(path, O_RDONLY); }
    if (fd < 0) { ERRORF("RnsConclude: read open(%s) failed errno=%d", path, errno); return RNS::Bytes(); }
    const uint32_t t0 = (uint32_t)esp_timer_get_time();
    size_t got = 0;
    while (got < total) {
      ssize_t n;
      { Storage::SDCard::BusGuard _bg; n = ::read(fd, dst + got, std::min(READ_CHUNK, total - got)); }
      if (n <= 0) break;
      got += (size_t)n;
    }
    { Storage::SDCard::BusGuard _bg; ::close(fd); }
    const uint32_t dt = (uint32_t)esp_timer_get_time() - t0;
    if (dt > read_max_us()) read_max_us() = dt;
    if (got != total) { ERRORF("RnsConclude: read_back short %zu/%zu", got, total); out.resize(got); }
    return out;
  }

  inline void worker_task(void*) {
    for (;;) {
      Job* job = nullptr;
      if (xQueueReceive(q(), &job, portMAX_DELAY) != pdTRUE || job == nullptr) continue;

      if (job->kind == Kind::WritePart) {
        do_write(*job);
        if (job->data) { heap_caps_free(job->data); inflight_bytes().fetch_sub(job->len, std::memory_order_relaxed); }
        delete job;
        continue;
      }

      // Conclude: all of this resource's part-writes are done (FIFO). Flush the
      // write fd, then read the assembled file back and decrypt+verify off-loop.
      close_cur_fd();
      const char* path = job->buffer ? job->buffer->backing_path() : nullptr;
      const size_t total = job->buffer ? job->buffer->total_size() : 0;
      RNS::Bytes assembled = read_back(path, total);
      if (job->buffer) job->buffer->discard();   // removes the temp file
      job->buffer.reset();

      if (job->resource) job->resource->prepare_from_assembled(assembled);
      stack_low_water() = uxTaskGetStackHighWaterMark(nullptr);
      jobs_done()++;
      if (xQueueSend(deliver_q(), &job, portMAX_DELAY) != pdTRUE) delete job;
    }
  }

  // loopTask, registered as the ResourceBuffer part-write hook. Copies the part
  // to PSRAM and enqueues a WritePart job. Falls back to an inline raw write
  // only if the queue is full or PSRAM is exhausted (window cap should prevent
  // both) - never drops a part.
  inline void part_write_hook(const char* path, uint32_t offset, const uint8_t* data, uint32_t len) {
    uint8_t* copy = nullptr;
    if (inflight_bytes().load(std::memory_order_relaxed) + len <= INFLIGHT_CAP)
      copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy) {
      memcpy(copy, data, len);
      Job* job = new (std::nothrow) Job{ Kind::WritePart };
      if (job) {
        strncpy(job->path, path, PATH_LEN - 1);
        job->offset = offset; job->data = copy; job->len = len;
        inflight_bytes().fetch_add(len, std::memory_order_relaxed);
        if (xQueueSend(q(), &job, 0) == pdTRUE) return;
        inflight_bytes().fetch_sub(len, std::memory_order_relaxed);
        delete job;
      }
      heap_caps_free(copy);
    }
    // Fallback: inline raw write (rare; keeps the transfer correct).
    fallbacks()++;
    Storage::SDCard::BusGuard _bg;
    int fd = ::open(path, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { write_errors()++; return; }
    if (::lseek(fd, (off_t)offset, SEEK_SET) >= 0) {
      uint32_t off = 0;
      while (off < len) { ssize_t w = ::write(fd, data + off, len - off); if (w <= 0) { write_errors()++; break; } off += (uint32_t)w; }
    }
    ::close(fd);
  }

  // loopTask, registered as Resource::set_conclude_deferrer: detach the receive
  // buffer and enqueue a Conclude job (after this resource's WritePart jobs).
  inline void deferrer(const RNS::Resource& resource) {
    RNS::Resource& r = const_cast<RNS::Resource&>(resource);
    Job* job = new (std::nothrow) Job{ Kind::Conclude };
    if (job != nullptr) {
      job->resource.reset(new (std::nothrow) RNS::Resource(r));  // keep-alive copy
      if (job->resource) {
        job->buffer = r.detach_buffer();
        if (xQueueSend(q(), &job, portMAX_DELAY) == pdTRUE) return;
        r.reattach_buffer(std::move(job->buffer));
      }
      delete job;
    }
    r.deliver_assembly();   // alloc failure: inline conclude on loopTask
  }

  // loopTask: deliver resources the worker has prepared. Call every tick.
  inline void drain_deliveries() {
    if (deliver_q() == nullptr) return;
    Job* job = nullptr;
    while (xQueueReceive(deliver_q(), &job, 0) == pdTRUE && job != nullptr) {
      if (job->resource) job->resource->deliver_assembly();
      delete job;
    }
  }

  inline bool begin() {
    if (q() != nullptr) return true;
    q()         = xQueueCreate(QUEUE_DEPTH, sizeof(Job*));
    deliver_q() = xQueueCreate(QUEUE_DEPTH, sizeof(Job*));
    if (q() == nullptr || deliver_q() == nullptr) { ERROR("RnsConclude: queue alloc failed; receive stays inline"); return false; }
    TaskHandle_t handle = nullptr;
    // Core 0 (off the radio's core 1); 8 KB stack covers decrypt + Bytes + the
    // LXMF concluded callback (measured high-water ~2 KB). Priority 5, matching
    // the SD writer.
    const BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "rxconclude", 8192, nullptr, 5, &handle, 0);
    if (ok != pdPASS) { ERROR("RnsConclude: worker task create failed; receive stays inline"); return false; }
    RNS::Resource::set_conclude_deferrer(deferrer);
    RNS::set_part_write_hook(part_write_hook);
    NOTICE("RnsConclude: off-loop receive (writes + conclude) active (core 0, 12 KB)");
    return true;
  }

}  // namespace RnsConclude
