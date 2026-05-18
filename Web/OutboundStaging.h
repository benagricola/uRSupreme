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

extern microStore::FileSystem filesystem;

namespace Web {
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

enum class Backend : uint8_t { Psram, Sd, Flash };

struct Buffer {
  uint32_t   id          = 0;
  Backend    backend     = Backend::Psram;
  size_t     total_bytes = 0;
  size_t     written     = 0;   // how much append()'d so far
  uint32_t   created_ms  = 0;
  // PSRAM backend
  uint8_t*   psram_ptr   = nullptr;
  // SD or Flash backend (path semantics differ; underlying FS doesn't)
  String     disk_path;
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
        } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
          if (Web::SDCard::present()) SD.remove(it->disk_path);
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
  c.sd_present = Web::SDCard::present();
  c.psram_free = (size_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  {
    const size_t avail = (size_t)filesystem.storageAvailable();
    c.flash_free = avail;
  }
  if (c.sd_present) {
    const uint64_t total = Web::SDCard::total_bytes();
    const uint64_t used  = Web::SDCard::used_bytes();
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
// (Web::Storage::effective_max_send) is enforced by the caller
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
    if (!SD.exists("/lxmf")) SD.mkdir("/lxmf");
    if (!SD.exists("/lxmf/staging")) SD.mkdir("/lxmf/staging");
    if (SD.exists(b.disk_path)) SD.remove(b.disk_path);
  } else {  // Flash
    b.disk_path = String("/lxmf/staging/") + b.id + ".bin";
    if (filesystem.exists(b.disk_path.c_str())) filesystem.remove(b.disk_path.c_str());
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
inline bool append(uint32_t id, const uint8_t* data, size_t len) {
  Buffer* b = _detail::find(id);
  if (!b) return false;
  if (b->written > b->total_bytes) return false;                 // invariant
  if (len > b->total_bytes - b->written) return false;           // overrun
  if (b->backend == Backend::Psram) {
    if (!b->psram_ptr) return false;
    memcpy(b->psram_ptr + b->written, data, len);
  } else if (b->backend == Backend::Sd) {
    File f = SD.open(b->disk_path, FILE_APPEND);
    if (!f) { Web::SDCard::verify_or_disable(); return false; }
    size_t w = f.write(data, len);
    f.close();
    if (w != len) { Web::SDCard::verify_or_disable(); return false; }
  } else {  // Flash
    microStore::File f = filesystem.open(b->disk_path.c_str(),
                                         microStore::File::ModeAppend, true);
    if (!f) return false;
    const size_t w = f.write(data, len);
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
  if (b->backend == Backend::Sd) {
    File f = SD.open(b->disk_path, FILE_READ);
    if (!f) return 0;
    if (!f.seek(offset)) { f.close(); return 0; }
    const int got = f.read(dst, avail);
    f.close();
    return got > 0 ? (size_t)got : 0;
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
    if (it->backend == Backend::Psram && it->psram_ptr) {
      heap_caps_free(it->psram_ptr);
    } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
      if (Web::SDCard::present()) SD.remove(it->disk_path);
    } else if (it->backend == Backend::Flash && !it->disk_path.isEmpty()) {
      if (filesystem.exists(it->disk_path.c_str())) filesystem.remove(it->disk_path.c_str());
    }
    NOTICEF("OutboundStaging: released id=%u", (unsigned)id);
    v.erase(it);
    return;
  }
}

}  // namespace OutboundStaging
}  // namespace Web
