// Outbound attachment staging — holds the raw bytes between the
// chunked-HTTP upload and the LXMF Resource send. (#130)
//
// Two backends, chosen at allocate-time based on what's available:
//
//   * SdBuffer    — file at /sd/lxmf/staging/<id>.bin. Preferred when a
//                   card is mounted: zero RAM cost, multi-GB headroom,
//                   no impact on PSRAM availability for RNS state.
//   * PsramBuffer — ps_malloc'd buffer in PSRAM. Fallback when no SD is
//                   present. Capped at ~4 MB to leave runtime headroom
//                   for path tables, identity store, etc.
//
// Lifecycle:
//   1. allocate(total_size) — picks backend, reserves space, returns
//      a Buffer ID the upload handler appends chunks to.
//   2. append(id, chunk_data, chunk_len) — multiple calls until total.
//   3. read(id, offset, len, dst) — used by LXMFMinimal during the
//      Resource hashmap computation + per-chunk send.
//   4. release(id) — drops the buffer, frees PSRAM / removes SD file.
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
#include "SDCard.h"

namespace Web {
namespace OutboundStaging {

// PSRAM allocation cap. Leaves ~4 MB headroom after RNS containers
// (~2 MB typical) + ArduinoJson docs (~0.5 MB transient). Tuned for
// the Supreme's 8 MB part; can grow later if profiling shows room.
inline constexpr size_t   PSRAM_CAP_BYTES   = 4 * 1024 * 1024;
// SD soft cap. Files this big over LoRa take hours; cap is more
// about "don't accidentally fill the user's SD card" than ESP32 RAM.
inline constexpr size_t   SD_CAP_BYTES      = 32 * 1024 * 1024;
// Hard ceiling enforced regardless of what the dynamic caps say. Even
// if current_caps() ever reports a giant max_bytes, we will not honour
// it. Defends against pathological client-supplied `total` values and
// any future drift in how the caps are computed.
inline constexpr size_t   ABSOLUTE_MAX_BYTES = 32 * 1024 * 1024;
// Garbage-collect untouched buffers older than this. Browser may
// upload and then disconnect; reclaim within a minute.
inline constexpr uint32_t STAGING_TIMEOUT_MS = 60000;

enum class Backend : uint8_t { Psram, Sd };

struct Buffer {
  uint32_t   id          = 0;
  Backend    backend     = Backend::Psram;
  size_t     total_bytes = 0;
  size_t     written     = 0;   // how much append()'d so far
  uint32_t   created_ms  = 0;
  // PSRAM backend
  uint8_t*   psram_ptr   = nullptr;
  // SD backend
  String     sd_path;
};

namespace _detail {
  inline std::vector<Buffer>& buffers() { static std::vector<Buffer> v; return v; }
  inline uint32_t&            next_id() { static uint32_t n = 1; return n; }

  inline Buffer* find(uint32_t id) {
    for (auto& b : buffers()) if (b.id == id) return &b;
    return nullptr;
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
        if (it->backend == Backend::Psram && it->psram_ptr) {
          heap_caps_free(it->psram_ptr);
        } else if (it->backend == Backend::Sd && !it->sd_path.isEmpty()) {
          if (Web::SDCard::present()) SD.remove(it->sd_path);
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

// Reported via /api/system_status so the SPA can gate its picker
// options + recorder duration to whatever the device can actually
// accept. Picks the larger of the two available backends.
struct Caps {
  size_t  max_bytes;
  Backend chosen_backend;
  size_t  psram_free;
  size_t  sd_free;
  bool    sd_present;
};
inline Caps current_caps() {
  Caps c{};
  c.sd_present = Web::SDCard::present();
  c.psram_free = (size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (c.sd_present) {
    const uint64_t total = Web::SDCard::total_bytes();
    const uint64_t used  = Web::SDCard::used_bytes();
    c.sd_free = (total > used) ? (size_t)(total - used) : 0;
  }
  // PSRAM-backed cap: min(PSRAM_CAP_BYTES, psram_free with margin).
  const size_t psram_max = (c.psram_free > 512 * 1024)
      ? std::min(PSRAM_CAP_BYTES, c.psram_free - 512 * 1024) : 0;
  const size_t sd_max = c.sd_present
      ? std::min(SD_CAP_BYTES, c.sd_free) : 0;
  if (sd_max > psram_max) {
    c.max_bytes      = sd_max;
    c.chosen_backend = Backend::Sd;
  } else {
    c.max_bytes      = psram_max;
    c.chosen_backend = Backend::Psram;
  }
  // Defense-in-depth: never report more than the absolute ceiling, no
  // matter what the underlying backend says is free.
  if (c.max_bytes > ABSOLUTE_MAX_BYTES) c.max_bytes = ABSOLUTE_MAX_BYTES;
  return c;
}

// Allocate a new staging buffer. Returns 0 on failure (over cap,
// PSRAM exhausted, SD write-fail). The size check rejects the request
// before any allocation happens, so a malicious or buggy client can't
// trigger a PSRAM exhaustion / SD fill through a wildly inflated
// `total` parameter.
inline uint32_t allocate(size_t total_bytes) {
  _detail::gc(millis());
  if (total_bytes == 0 || total_bytes > ABSOLUTE_MAX_BYTES) {
    WARNINGF("OutboundStaging: refusing alloc — %u bytes outside absolute bounds (0, %u]",
             (unsigned)total_bytes, (unsigned)ABSOLUTE_MAX_BYTES);
    return 0;
  }
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
  b.backend     = c.chosen_backend;
  if (b.backend == Backend::Psram) {
    b.psram_ptr = (uint8_t*)heap_caps_malloc(total_bytes, MALLOC_CAP_SPIRAM);
    if (!b.psram_ptr) {
      ERRORF("OutboundStaging: PSRAM alloc of %u bytes failed", (unsigned)total_bytes);
      return 0;
    }
  } else {
    b.sd_path = String("/lxmf/staging/") + b.id + ".bin";
    // mkdir parents — single-level only on Arduino SD.
    if (!SD.exists("/lxmf")) SD.mkdir("/lxmf");
    if (!SD.exists("/lxmf/staging")) SD.mkdir("/lxmf/staging");
    // Truncate any pre-existing file at this path before writes.
    if (SD.exists(b.sd_path)) SD.remove(b.sd_path);
  }
  _detail::buffers().push_back(b);
  NOTICEF("OutboundStaging: allocated id=%u backend=%s size=%u",
          (unsigned)b.id,
          b.backend == Backend::Psram ? "psram" : "sd",
          (unsigned)total_bytes);
  return b.id;
}

// Append a chunk. Returns true on success, false on overrun / unknown id.
// The overrun check uses subtraction (not addition) so we can't get
// fooled by a wrap-around on attacker-supplied `len` — the bound check
// stays correct for any size_t input.
inline bool append(uint32_t id, const uint8_t* data, size_t len) {
  Buffer* b = _detail::find(id);
  if (!b) return false;
  if (b->written > b->total_bytes) return false;                 // invariant
  if (len > b->total_bytes - b->written) return false;           // overrun
  if (b->backend == Backend::Psram) {
    if (!b->psram_ptr) return false;
    memcpy(b->psram_ptr + b->written, data, len);
  } else {
    File f = SD.open(b->sd_path, FILE_APPEND);
    if (!f) return false;
    size_t w = f.write(data, len);
    f.close();
    if (w != len) return false;
  }
  b->written += len;
  return true;
}

inline bool complete(uint32_t id) {
  Buffer* b = _detail::find(id);
  return b && b->written == b->total_bytes;
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
  File f = SD.open(b->sd_path, FILE_READ);
  if (!f) return 0;
  if (!f.seek(offset)) { f.close(); return 0; }
  const int got = f.read(dst, avail);
  f.close();
  return got > 0 ? (size_t)got : 0;
}

inline void release(uint32_t id) {
  auto& v = _detail::buffers();
  for (auto it = v.begin(); it != v.end(); ++it) {
    if (it->id != id) continue;
    if (it->backend == Backend::Psram && it->psram_ptr) {
      heap_caps_free(it->psram_ptr);
    } else if (it->backend == Backend::Sd && !it->sd_path.isEmpty()) {
      if (Web::SDCard::present()) SD.remove(it->sd_path);
    }
    NOTICEF("OutboundStaging: released id=%u", (unsigned)id);
    v.erase(it);
    return;
  }
}

}  // namespace OutboundStaging
}  // namespace Web
