// Cached LittleFS free-space.
//
// filesystem.storageAvailable() resolves to LittleFS.usedBytes(), which runs
// lfs_fs_size() — a full-partition block scan that holds the esp_littlefs
// per-instance semaphore for seconds on a populated partition (~5 s on the
// 4.4 MB flash partition). The attachment-cap clamp (/api/storage/config)
// reads it twice per request (send + receive caps) and the SPA polls it on
// load and on attach, so an uncached scan stalls every storage query for
// seconds; under the rns_lock it stalls the main loop / LoRa with it.
//
// Two accessors, split so the expensive scan can NEVER run under the rns_lock
// (where it would starve LoRa, mid-Resource-reception in the worst case):
//
//   flash_free()         — non-blocking; returns the last scanned value (0
//                          until the first scan). Safe to call from anywhere,
//                          including the receive path and other rns_lock
//                          holders. This is the default every caller should use.
//   flash_free_refresh() — runs the block scan if the cache is stale, updates
//                          it, and returns the value. MUST be called only from
//                          a context that does NOT hold the rns_lock (it blocks
//                          ~5 s): the lock-free /api/storage/config handler and
//                          the boot warm-up. Single-flight: only one caller
//                          ever scans; concurrent callers get the last value.
//
// Free space only drifts as files are written, so a value up to one refresh
// window old is fine for a UI cap hint, the inbound-Resource cap, and the boot
// "FS full" guard. esp_littlefs serialises the scan against other FS ops with
// its own semaphore, so callers need no external lock for FS safety.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <Arduino.h>                 // millis()
#include <microStore/FileSystem.h>

extern microStore::FileSystem filesystem;

namespace Storage {

namespace _freespace {
  struct Cache {
    std::atomic<size_t>   value{0};
    std::atomic<uint32_t> scanned_at{0};   // millis() of last scan; 0 = never
    std::atomic<bool>     refreshing{false};
  };
  inline Cache& ref() { static Cache c; return c; }
}

// Last known bytes free on the flash filesystem. Never scans — returns 0 until
// the first flash_free_refresh() (the boot warm-up) completes. Safe under any
// lock.
inline size_t flash_free() {
  return _freespace::ref().value.load(std::memory_order_relaxed);
}

// Refresh the cache if older than `max_age_ms`, then return it. Blocks ~5 s on
// a cold scan — call only off the rns_lock (web handler / boot). Single-flight:
// while one caller scans, others get the last value (or 0 before the first).
inline size_t flash_free_refresh(uint32_t max_age_ms = 30000) {
  _freespace::Cache& c = _freespace::ref();
  const uint32_t at = c.scanned_at.load(std::memory_order_acquire);
  if (at != 0 && (millis() - at) < max_age_ms)
    return c.value.load(std::memory_order_relaxed);
  bool expected = false;
  if (!c.refreshing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    return c.value.load(std::memory_order_relaxed);
  const size_t v = (size_t)filesystem.storageAvailable();
  c.value.store(v, std::memory_order_relaxed);
  const uint32_t t = millis();
  c.scanned_at.store(t != 0 ? t : 1, std::memory_order_release);   // never store 0
  c.refreshing.store(false, std::memory_order_release);
  return v;
}

// Force the next flash_free_refresh() to rescan — call after a write/delete
// that materially changes free space (e.g. the boot path-store purge).
inline void invalidate_flash_free() {
  _freespace::ref().scanned_at.store(0, std::memory_order_release);
}

}  // namespace Storage
