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

// SD short-write retry tuning. A card commits bytes up to a sector
// boundary then can stall for 100s of ms during internal housekeeping
// before it will accept the next sector — far longer than a few ms. We
// flush, pause, and retry the remainder; while we block here TCP
// back-pressure pauses the upload, so the card gets real breathing room.
// Budget ~1.6 s of retry before declaring a genuinely failing card.
// The retry releases the HSPI bus during each wait (see _sd_write_all), so
// it never blocks the rest of the device. ~1.6 s budget recovers a card
// that briefly stalls a sector-commit; a card that returns 0 for longer
// than this is genuinely failing the block and won't recover with more
// waiting (measured), so we fail the upload reasonably promptly instead.
inline constexpr int      SD_WRITE_MAX_STALLS    = 80;
inline constexpr uint32_t SD_WRITE_STALL_DELAY_MS = 20;

// The async multipart parser hands an upload over in thousands of tiny,
// often sub-sector chunks (observed min 1 byte). Writing each straight to
// SD means thousands of unaligned sub-sector writes, which thrash the card
// into stalls. Instead accumulate into this PSRAM buffer and flush to SD in
// large aligned blocks (one ~64 KiB write per 128 chunks), which the card
// handles cleanly. 64 KiB = 128 sectors; PSRAM-backed, so cheap.
inline constexpr size_t   SD_WRITE_BUF_BYTES = 64 * 1024;

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
  // Held write handle, kept open for the whole upload so each chunk is
  // a bare write() rather than an open()+append()+close() round-trip.
  // Re-opening the file per chunk forces a FAT/dir-entry flush to the
  // card on every ~2 KB the async server hands us, which collapsed an
  // 8 MB upload to tens of KB/s despite a fast WiFi link. Exactly one
  // of these is live at a time, matching `backend`; both are closed at
  // finalize_write() / release() / gc(). (File is shared-ptr-backed,
  // so copying the Buffer into the static vector keeps the handle open.)
  File             sd_file;     // Backend::Sd
  microStore::File flash_file;  // Backend::Flash
  // SD backend: PSRAM accumulator so the card sees large aligned writes
  // instead of the parser's thousands of tiny chunks. Freed at
  // close_write_handle(). nullptr => fall back to direct per-chunk writes.
  uint8_t*   sd_wbuf      = nullptr;
  size_t     sd_wbuf_used = 0;
};

namespace _detail {
  inline std::vector<Buffer>& buffers() { static std::vector<Buffer> v; return v; }
  inline uint32_t&            next_id() { static uint32_t n = 1; return n; }

  inline Buffer* find(uint32_t id) {
    for (auto& b : buffers()) if (b.id == id) return &b;
    return nullptr;
  }

  // Close whichever disk write handle a buffer holds open. Idempotent —
  // safe to call from finalize_write() and again from release()/gc().
  inline void close_write_handle(Buffer& b) {
    if (b.sd_wbuf) { heap_caps_free(b.sd_wbuf); b.sd_wbuf = nullptr; b.sd_wbuf_used = 0; }
    if (b.backend == Backend::Sd && b.sd_file) {
      Storage::SDCard::BusGuard _bg;   // close() flushes over the shared HSPI bus
      b.sd_file.close();
      b.sd_file = File();
    } else if (b.backend == Backend::Flash && b.flash_file) {
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
        close_write_handle(*it);  // release the fd before removing the file
        if (it->backend == Backend::Psram && it->psram_ptr) {
          heap_caps_free(it->psram_ptr);
        } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
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
    Storage::SDCard::BusGuard _bg;     // serialise the mkdir/exists/remove/open
                                       // against the IMU pump on the shared bus
    if (!SD.exists("/lxmf")) SD.mkdir("/lxmf");
    if (!SD.exists("/lxmf/staging")) SD.mkdir("/lxmf/staging");
    if (SD.exists(b.disk_path)) SD.remove(b.disk_path);
    // Open once; append() writes into this handle and finalize_write()
    // closes it. FILE_WRITE truncates (file was just removed) then
    // appends on each sequential write.
    b.sd_file = SD.open(b.disk_path, FILE_WRITE);
    if (!b.sd_file) {
      ERRORF("OutboundStaging: SD open of %s failed", b.disk_path.c_str());
      Storage::SDCard::verify_or_disable();
      return 0;
    }
    // PSRAM write accumulator (best-effort: if it can't be had, append()
    // falls back to direct per-chunk writes).
    b.sd_wbuf = (uint8_t*)heap_caps_malloc(SD_WRITE_BUF_BYTES, MALLOC_CAP_SPIRAM);
    b.sd_wbuf_used = 0;
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

// Write `len` bytes to the buffer's SD handle, retrying a short write
// (sector-commit stall). Returns false after the stall budget, setting
// fail_detail().
//
// Crucially this acquires the HSPI bus mutex ONLY around each individual
// write/flush call, and RELEASES it across the inter-retry wait. Holding
// the shared bus (and, transitively, any rns_lock holder waiting on it)
// for the whole multi-second retry is what let a failing/slow upload
// degrade the rest of the device — main-loop bus users and other web
// requests stalled behind it. With the bus freed during the wait, a
// stalled card can't impair anything else; only this upload waits, and
// the card gets idle bus time to recover (which it can't if we squat it).
inline bool _sd_write_all(Buffer& b, const uint8_t* data, size_t len) {
  size_t off = 0;
  int stalls = 0;
  while (off < len) {
    size_t w;
    { Storage::SDCard::BusGuard _bg; w = b.sd_file.write(data + off, len - off); }
    off += w;
    if (off >= len) return true;
    if (w > 0) { stalls = 0; continue; }
    if (++stalls > SD_WRITE_MAX_STALLS) {
      const uint64_t total = Storage::SDCard::total_bytes();
      const uint64_t used  = Storage::SDCard::used_bytes();
      snprintf(fail_detail(), 192,
               "SD short-write near=%u blk=%u wrote=%u free=%llu total=%llu",
               (unsigned)b.written, (unsigned)len, (unsigned)off,
               (unsigned long long)(total > used ? total - used : 0),
               (unsigned long long)total);
      ERRORF("OutboundStaging: %s", fail_detail());
      Storage::SDCard::verify_or_disable();
      return false;
    }
    // Bus released across the wait: yields (delay == vTaskDelay) so the main
    // loop / IMU / other web work run while the card settles. No flush() —
    // the card returned 0 (accepted nothing), so there is nothing new to
    // commit; the next write() re-attempts the same sector once the card is
    // ready, and squatting the bus to flush only denies it recovery time.
    delay(SD_WRITE_STALL_DELAY_MS);
  }
  return true;
}

// Flush the PSRAM accumulator to the card as one large aligned write.
// _sd_write_all takes the bus per write call, so we must NOT hold it here.
inline bool _flush_sd_wbuf(Buffer& b) {
  if (b.sd_wbuf_used == 0) return true;
  if (!_sd_write_all(b, b.sd_wbuf, b.sd_wbuf_used)) return false;
  b.sd_wbuf_used = 0;
  return true;
}

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
    if (!b->sd_file) { Storage::SDCard::verify_or_disable(); return false; }
    if (b->sd_wbuf) {
      // Accumulate into the PSRAM buffer; flush to SD only in full aligned
      // blocks so the card sees ~64 KiB writes, not the parser's thousands
      // of tiny sub-sector chunks (which thrash it into stalls).
      size_t off = 0;
      while (off < len) {
        const size_t take = std::min(SD_WRITE_BUF_BYTES - b->sd_wbuf_used, len - off);
        memcpy(b->sd_wbuf + b->sd_wbuf_used, data + off, take);
        b->sd_wbuf_used += take;
        off += take;
        if (b->sd_wbuf_used == SD_WRITE_BUF_BYTES && !_flush_sd_wbuf(*b)) return false;
      }
    } else {
      // No accumulator (PSRAM alloc failed): direct write. _sd_write_all
      // takes the bus per write call.
      if (!_sd_write_all(*b, data, len)) return false;
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
  bool ok = true;
  if (b->backend == Backend::Sd && b->sd_wbuf)
    ok = _flush_sd_wbuf(*b);          // commit the buffered tail
  _detail::close_write_handle(*b);    // also frees the accumulator
  return ok;
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
    _detail::close_write_handle(*it);  // release the fd before removing the file
    if (it->backend == Backend::Psram && it->psram_ptr) {
      heap_caps_free(it->psram_ptr);
    } else if (it->backend == Backend::Sd && !it->disk_path.isEmpty()) {
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
