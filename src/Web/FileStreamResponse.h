// Crash-safe chunked file-streaming response for the web server.
//
// AsyncTCP invokes a response producer with maxLen ~= one TCP segment
// (~1460 B). Reading the backing file per call means hundreds of reads
// for a large blob; worse, the producer can be re-entered after it
// returns 0 (EOF), so any state freed on EOF is a use-after-free - the
// exact crash that wedged the SX webserver during inbound Resource
// transfers. This helper owns the fix once, for every file route:
//
//   - One source read per 32 KiB scratch refill, drained across many
//     producer calls (amortises the per-segment read cost ~22x).
//   - The producer state lives in a std::shared_ptr captured BY VALUE
//     into the lambda. The last refcount drops when AsyncWebServer
//     destroys the response (completion, disconnect, or internal
//     abort), and only then does on_destroy run. Idempotent and safe
//     no matter how many times the producer is invoked.
//
// The caller supplies the byte source as two closures, so the helper
// stays agnostic to the backend (SD card, internal flash, ...):
//   reader(dst, want) -> bytes copied, 0 = EOF. The reader does its own
//                        bus arbitration (e.g. SDCard::BusGuard for SD).
//   on_destroy()      -> release the source (close the file handle).
#pragma once

#include <ESPAsyncWebServer.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <functional>
#include <memory>

namespace Web {
namespace FileStream {

// One source read per refill. PSRAM-backed (the web buffers must not
// churn internal SRAM, which the WiFi MAC needs for esf_buf descriptors).
inline constexpr size_t SCRATCH_SIZE = 32 * 1024;

// Build a streaming response of `total` bytes. Returns nullptr if the
// scratch buffer cannot be allocated (the caller should send a 503);
// otherwise the response is returned for the caller to add headers and
// pass to req->send(). `on_destroy` always runs exactly once, when the
// response is torn down.
inline AsyncWebServerResponse* begin(
    AsyncWebServerRequest* req, size_t total, const char* content_type,
    std::function<size_t(uint8_t*, size_t)> reader,
    std::function<void()> on_destroy) {
  struct StreamState {
    std::function<size_t(uint8_t*, size_t)> reader;
    std::function<void()>                   on_destroy;
    uint8_t* scratch        = nullptr;
    size_t   scratch_size   = 0;
    size_t   scratch_valid  = 0;
    size_t   scratch_offset = 0;
    bool     eof            = false;
    ~StreamState() {
      if (on_destroy) on_destroy();
      if (scratch)    heap_caps_free(scratch);
    }
  };
  auto st = std::make_shared<StreamState>();
  st->reader       = std::move(reader);
  st->on_destroy   = std::move(on_destroy);
  st->scratch_size = SCRATCH_SIZE;
  st->scratch = (uint8_t*)heap_caps_malloc(SCRATCH_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!st->scratch) st->scratch = (uint8_t*)heap_caps_malloc(SCRATCH_SIZE, MALLOC_CAP_8BIT);
  if (!st->scratch) return nullptr;   // on_destroy still runs when st dies
  return req->beginResponse(
      content_type, total,
      [st](uint8_t* dst, size_t maxLen, size_t /*index*/) -> size_t {
        // Refill scratch on demand: one source read per 32 KiB instead
        // of one per TCP segment.
        if (!st->eof && st->scratch_offset >= st->scratch_valid) {
          const size_t got   = st->reader(st->scratch, st->scratch_size);
          st->scratch_offset = 0;
          st->scratch_valid  = got;
          if (got == 0) st->eof = true;
        }
        if (st->eof && st->scratch_offset >= st->scratch_valid) {
          return 0;   // refcount drops when AsyncWebServer frees the lambda
        }
        const size_t avail = st->scratch_valid - st->scratch_offset;
        const size_t n     = avail < maxLen ? avail : maxLen;
        memcpy(dst, st->scratch + st->scratch_offset, n);
        st->scratch_offset += n;
        return n;
      });
}

}  // namespace FileStream
}  // namespace Web
