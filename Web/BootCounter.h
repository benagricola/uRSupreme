#pragma once

#include <ArduinoJson.h>
#include "PsramAllocator.h"
#include <Log.h>
#include <microStore/FileSystem.h>

#include <stdint.h>
#include <vector>

extern microStore::FileSystem filesystem;

namespace Web {

  // Persistent boot epoch counter.
  //
  // We want a monotonic-across-reboots ordering key for inbox + outbox
  // entries. `millis()` alone won't do — it resets to 0 on every boot,
  // so a record appended early in boot N+1 (small millis) would sort
  // before a record persisted late in boot N (large millis), inverting
  // the timeline.
  //
  // BootCounter solves this by maintaining a single uint32_t in
  // /lxmf/boot_epoch.json that increments once per boot. Combined with
  // `received_ms`, the tuple (boot_epoch, received_ms) gives a globally
  // monotonic ordering: any record from boot N+1 sorts after every
  // record from boot N, and within a boot the millis() tiebreak holds.
  //
  // Records loaded from JSONL spools that pre-date this field decode
  // with boot_epoch = 0, so they always sort *before* anything from
  // a fresh (≥1) boot. That's a reasonable post-upgrade default — old
  // history clusters at the bottom of the timeline rather than
  // shuffling unpredictably with new activity.
  //
  // Persistence is best-effort: writeFile here is non-atomic, so a
  // power-cut during the save could lose the increment. Worst case the
  // counter rewinds by one boot, briefly inverting two boots' worth of
  // records. Atomic writes (write-then-rename) would close this gap;
  // worth doing if it bites in practice.
  class BootCounter {
  public:
    static constexpr const char* STORE_PATH = "/lxmf/boot_epoch.json";

    // Increment-and-save on first call; subsequent calls are no-ops.
    // Safe to call from anywhere — current() also lazy-inits — but
    // wiring an explicit call in setup() makes the log line easy to
    // find.
    static uint32_t init() {
      if (_initialized) return _current;
      uint32_t prev = read_prev();
      _current = prev + 1;
      save(_current);
      _initialized = true;
      NOTICEF("BootCounter: boot epoch = %u (prev was %u)",
              (unsigned)_current, (unsigned)prev);
      return _current;
    }

    static uint32_t current() {
      if (!_initialized) return init();
      return _current;
    }

  private:
    static uint32_t read_prev() {
      if (!filesystem.exists(STORE_PATH)) return 0;
      std::vector<uint8_t> data;
      if (filesystem.readFile(STORE_PATH, data) == 0) return 0;
      Web::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return 0;
      return (uint32_t)(doc["boot"] | 0);
    }

    static void save(uint32_t v) {
      Web::PsramJsonDocument doc;
      doc["boot"] = v;
      String body;
      if (serializeJson(doc, body) == 0) {
        WARNING("BootCounter: save serialization failed");
        return;
      }
      filesystem.writeFile(STORE_PATH,
                           reinterpret_cast<const uint8_t*>(body.c_str()),
                           body.length());
    }

    static inline bool     _initialized = false;
    static inline uint32_t _current     = 0;
  };

}
