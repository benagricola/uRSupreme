// SD card driver for the T-Beam Supreme.
//
// The board exposes a microSD slot wired to a dedicated SPI bus
// (SD_MISO=37, SD_MOSI=35, SD_CLK=36, SD_CS=47) - separate from the
// LoRa modem's SPI on pins 11-13, so we can mount the card without
// stepping on the radio. The bus is shared with nothing else on this
// platform, so we own SPIClass(HSPI) for it.
//
// Self-detecting: begin() probes for a card and sets `_present` to
// the result. If no card is inserted at boot, the driver stays inert
// - no errors, just present=false. We don't currently hot-detect a
// card inserted later; the user reboots after inserting a card.
//
// Responsibilities for now (first slice):
//   * Mount the card if present, expose total/used/free byte counts.
//   * Leave the bus alone if no card is present so it stays cheap to
//     query state via /api/system_status.
//
// Subsequent slice will route LXMF attachment storage here when a
// card is mounted so we can lift the 48 KB per-message cap.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../Boards.h"

namespace Storage {
namespace SDCard {

// HSPI bus arbitration. The SD card and the QMI8658 IMU share the HSPI
// bus on this hardware. SD I/O issued from the AsyncTCP web task
// (attachment upload / download) would otherwise interleave on the bus
// with the IMU pump run from the main loop - a long upload spans many
// pump ticks, so the two drive the bus at once and corrupt the SD write
// (a short write that surfaces as "Chunk write failed"). Every bus
// access, SD op or IMU read, holds this recursive mutex for its
// duration. Recursive so a guarded SD op that calls another guarded
// helper (e.g. append -> verify_or_disable -> SD.totalBytes) can't
// self-deadlock. Lives here because SDCard owns the shared SPIClass.
namespace _detail {
  inline SemaphoreHandle_t& bus_mtx_ref() {
    static SemaphoreHandle_t m = xSemaphoreCreateRecursiveMutex();
    return m;
  }
}
struct BusGuard {
  bool held = false;
  BusGuard() {
    SemaphoreHandle_t m = _detail::bus_mtx_ref();
    if (m) held = (xSemaphoreTakeRecursive(m, portMAX_DELAY) == pdTRUE);
  }
  ~BusGuard() { if (held) xSemaphoreGiveRecursive(_detail::bus_mtx_ref()); }
  BusGuard(const BusGuard&) = delete;
  BusGuard& operator=(const BusGuard&) = delete;
};

namespace _detail {
  inline bool&     present_ref()    { static bool v = false; return v; }
  inline SPIClass*& spi_ref()       { static SPIClass* v = nullptr; return v; }
  inline uint8_t&  card_type_ref()  { static uint8_t v = CARD_NONE; return v; }
  // Last diagnostic line from begin() - surfaced via the API when
  // probe fails so the user can tell "no card in slot" from
  // "card present, mount failed" from "wrong filesystem", etc.
  inline String& last_status_ref()  { static String v = "not_probed"; return v; }
  // Edge flag flipped by verify_or_disable on an ejection trip.
  // WebUI::loop drains this and fires the storage_changed WS event
  // - keeps the SD-aware code free of the WebSocket dependency.
  inline bool&    eject_flag_ref()  { static bool v = false; return v; }
}

// Take + clear the eject-edge flag. Returns true exactly once after
// an ejection has been detected by verify_or_disable; the WebUI
// drains this in its periodic loop to fire storage_changed.
inline bool take_eject_edge() {
  bool& f = _detail::eject_flag_ref();
  if (!f) return false;
  f = false;
  return true;
}

// Idempotent shared-bus init. The SD card and the QMI8658 IMU both
// live on the HSPI bus on this hardware. Whichever module
// runs first creates the bus + drives IMU_CS HIGH to keep the IMU
// from squatting MISO. The other reuses it. Returns the SPIClass on
// success, nullptr on boards that don't have the slot.
inline SPIClass* ensure_shared_bus() {
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  if (_detail::spi_ref()) return _detail::spi_ref();
  // Drive IMU_CS HIGH (deassert) before touching the bus so the IMU
  // doesn't ACK during SD comms (or vice versa).
  pinMode(IMU_CS, OUTPUT);
  digitalWrite(IMU_CS, HIGH);
  _detail::spi_ref() = new SPIClass(HSPI);
  _detail::spi_ref()->begin(SD_CLK, SD_MISO, SD_MOSI);
  return _detail::spi_ref();
#else
  return nullptr;
#endif
}

// Forward decls: begin() populates the free-space cache (both defined below).
inline void init_space_cache();
inline uint64_t total_bytes();
inline uint64_t used_bytes();

// One-shot init. Safe to call when HAS_SD == false at the board level
// (just no-ops). Returns true if a card was detected and mounted.
inline bool begin() {
  // Compile-out path for boards without an SD slot. The pin constants
  // SD_* are only defined on T-Beam Supreme variants in Boards.h, so
  // gate everything on the same board check the .ino uses.
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  // Bus init is shared with the IMU; whichever module runs first
  // owns the SPIClass(HSPI) + drives IMU_CS HIGH. (ensure_shared_bus
  // is idempotent.)
  SPIClass* bus = ensure_shared_bus();
  if (!bus) {
    _detail::last_status_ref() = "no_slot_on_board";
    return false;
  }
  NOTICEF("SDCard: probing pins clk=%d miso=%d mosi=%d cs=%d imu_cs=%d (held high)",
          SD_CLK, SD_MISO, SD_MOSI, SD_CS, IMU_CS);
  // SD SPI clock: 10 MHz. The earlier 4 MHz cap was measured against the
  // pre-writer-task upload path (no bus serialisation, per-block fsync,
  // latched-handle recovery); re-swept on the rig against the current path
  // (2026-06-10, SanDisk Ultra 64 GB FAT32, rmap detached): 10 MHz ran
  // 7/7 uploads with 0 write errors, SHA-verified, 10 MiB at ~540 KB/s
  // (~1.9x the 4 MHz rate). 20 MHz was also error-free but no faster -
  // above ~10 MHz the WiFi/multipart ingest is the ceiling - so 10 MHz
  // keeps double the signal margin on the shared IMU bus for the same
  // throughput. URTN_SD_SPI_HZ overrides for sweep builds.
#ifndef URTN_SD_SPI_HZ
#define URTN_SD_SPI_HZ 10000000
#endif
  // Max simultaneously-open files. The Arduino default (5) is below a
  // browser's per-host parallel-connection limit (Chrome/Firefox open up
  // to 6), and each in-flight map-tile range response holds an SD File
  // open for the whole stream, so the 6th concurrent open failed with a
  // spurious 404 (blank tiles). The FATFS fd array is PSRAM-backed
  // (CONFIG_FATFS_ALLOC_PREFER_EXTRAM=y, ~552 B/file, ~16 KB at 30), so
  // PSRAM is not the limit. The real ceiling is the global VFS descriptor
  // table (FD_SETSIZE = 64), shared by lwip sockets
  // (CONFIG_LWIP_MAX_SOCKETS = 16) and the 3 std streams. 30 leaves the
  // sockets comfortable headroom (30 + 16 + 3 = 49 < 64) while covering
  // multiple browser tabs plus device-side file ops (persistence,
  // download/extract writers, log appends). Default mountpoint "/sd"
  // passed through to reach the max_files argument.
  static constexpr uint8_t URTN_SD_MAX_OPEN_FILES = 30;
  if (!SD.begin(SD_CS, *bus, URTN_SD_SPI_HZ, "/sd", URTN_SD_MAX_OPEN_FILES)) {
    _detail::last_status_ref() = "sd_begin_failed";
    NOTICE("SDCard: SD.begin() failed - card absent, wrong pinout, or unsupported FS (try FAT32)");
    _detail::present_ref() = false;
    return false;
  }
  const uint8_t ct = SD.cardType();
  _detail::card_type_ref() = ct;
  if (ct == CARD_NONE) {
    _detail::last_status_ref() = "card_type_none";
    NOTICE("SDCard: SD.begin succeeded but cardType == CARD_NONE - slot empty");
    SD.end();
    _detail::present_ref() = false;
    return false;
  }
  _detail::present_ref() = true;
  init_space_cache();   // one-time FAT scan at mount; populates the free-space cache
  const char* ct_name = ct == CARD_MMC  ? "MMC"
                     : ct == CARD_SD   ? "SD"
                     : ct == CARD_SDHC ? "SDHC"
                     : "unknown";
  _detail::last_status_ref() = "mounted";
  NOTICEF("SDCard: mounted %s, total=%llu bytes used=%llu bytes",
          ct_name,
          (unsigned long long)total_bytes(),    // cached (no extra FAT scan)
          (unsigned long long)used_bytes());
  return true;
#else
  _detail::last_status_ref() = "no_slot_on_board";
  return false;
#endif
}

// Diagnostic string from the most recent begin() - values:
// "not_probed", "sd_begin_failed", "card_type_none", "mounted",
// "no_slot_on_board".
inline const char* last_status() { return _detail::last_status_ref().c_str(); }

// PMU rail snapshot captured by the .ino just before SDCard::begin
// runs. Used by /api/system_status to distinguish "rails came up"
// from "card not detected". Set via set_rail_state().
struct RailState {
  bool captured  = false;
  bool bldo1_on  = false;
  int  bldo1_mV  = 0;
  bool bldo2_on  = false;
  int  bldo2_mV  = 0;
};
namespace _detail { inline RailState& rail_state_ref() { static RailState v; return v; } }
inline void set_rail_state(bool b1_on, int b1_mV, bool b2_on, int b2_mV) {
  RailState& r = _detail::rail_state_ref();
  r.captured = true;
  r.bldo1_on = b1_on; r.bldo1_mV = b1_mV;
  r.bldo2_on = b2_on; r.bldo2_mV = b2_mV;
}
inline const RailState& rail_state() { return _detail::rail_state_ref(); }

inline bool      present()      { return _detail::present_ref(); }

// Verify the card is still responsive. Called from write/open
// failure paths - if the underlying SD op fails AND a free-space
// query also fails, the card has been ejected; we tear down the
// mount and return true so the caller knows to fall back to flash.
// On a healthy card this is one SDMMC query (1-10 ms). On no-card
// boards it short-circuits on present_ref() and costs nothing.
//
// No periodic polling: SD presence is only re-evaluated when an
// operation actually fails. Plugging a card requires a reboot
// (same as the rest of the firmware). This avoids the per-tick
// SDMMC query cost that was starving the AsyncWebServer listener
// startup on boot.
inline bool verify_or_disable() {
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  if (!_detail::present_ref()) return true;
  BusGuard _bg;                        // SD.totalBytes() touches the shared HSPI bus
  if (SD.totalBytes() > 0) return false;
  NOTICE("SDCard: ejection detected on write failure - disabling");
  SD.end();
  _detail::present_ref()    = false;
  _detail::card_type_ref()  = CARD_NONE;
  _detail::last_status_ref() = "ejected";
  _detail::eject_flag_ref()  = true;
  return true;
#else
  return false;
#endif
}
// SD.usedBytes() walks the FAT to count free clusters - slow (observed multi-
// second on a 64 GB card) AND on the shared HSPI bus. Calling it from
// current_caps() at every upload's allocate() froze the AsyncTCP task for
// seconds (measured /api/info spikes to 4 s), which dropped connections.
// total/used are cached instead: total is constant (cached at mount); used is
// refreshed off the AsyncTCP task (by the SD writer task after each job, see
// OutboundStaging::_sdwriter) so the value the upload path reads is bus-free.
// Approximate is fine - this only gates backend selection and the card has GB
// of headroom. cached_used starts 0 (optimistic: empty) until first refreshed;
// a genuinely-full card is still caught by the writer's checked ENOSPC.
inline uint64_t& _cached_total()    { static uint64_t v = 0; return v; }
inline uint64_t& _cached_used()     { static uint64_t v = 0; return v; }
inline uint32_t& _cached_space_ms() { static uint32_t v = 0; return v; }

// Called once at mount (off the hot path / boot, not the AsyncTCP task).
inline void init_space_cache() {
  if (!_detail::present_ref()) return;
  BusGuard _bg;
  _cached_total()    = (uint64_t)SD.totalBytes();
  _cached_used()     = (uint64_t)SD.usedBytes();
  const uint32_t now = millis();
  _cached_space_ms() = now ? now : 1;
}

// SLOW (FAT scan) - MUST only run off the AsyncTCP task. Called by the SD
// writer task after a job; throttled to at most once per 30 s so it doesn't
// add the scan cost to every upload.
inline void refresh_used_cache() {
  if (!_detail::present_ref()) return;
  const uint32_t now = millis();
  if (_cached_space_ms() != 0 && (now - _cached_space_ms()) < 30000) return;
  BusGuard _bg;
  _cached_used()     = (uint64_t)SD.usedBytes();
  _cached_space_ms() = now ? now : 1;
}
inline uint64_t  total_bytes()  { return _detail::present_ref() ? _cached_total() : 0; }
inline uint64_t  used_bytes()   { return _detail::present_ref() ? _cached_used()  : 0; }

// Negative exists()/open_read() results are routine (path probes for
// records that were never spilled, attachment misses), and each
// verify_or_disable() costs an SD-bus hardware query even on a healthy
// card, so a burst of misses used to pay that query per miss - the
// same starvation class dfbae86 gated out of the microStore failure
// callback. Throttle the lazy eject probe to one hardware query per
// window: a pulled card is still caught by the first probe after the
// window expires, and immediately by any write failure.
inline void verify_or_disable_throttled() {
  static uint32_t last_ms = 0;
  const uint32_t now = millis();
  if (last_ms != 0 && (now - last_ms) < 5000) return;
  last_ms = now;
  verify_or_disable();
}
inline uint8_t   card_type()    { return _detail::card_type_ref(); }

// ---- File helpers used by the LXMF attachment routing. ----

// Make sure every directory in `path` exists, walking down from
// the root. SD.mkdir is single-level only.
inline bool ensure_parent_dirs(const char* path) {
  if (!_detail::present_ref()) return false;
  BusGuard _bg;                        // SD.exists / SD.mkdir on the shared HSPI bus
  String p = path;
  int slash = 0;
  // Walk forward through each "/segment", mkdir-ing the prefix.
  while ((slash = p.indexOf('/', slash + 1)) > 0) {
    String prefix = p.substring(0, slash);
    if (!SD.exists(prefix)) {
      if (!SD.mkdir(prefix)) {
        WARNINGF("SDCard: mkdir failed: %s", prefix.c_str());
        return false;
      }
    }
  }
  return true;
}

// Write `data` to `path` atomically-ish. Returns bytes written, or
// Path existence. Returns false uniformly whether the file is
// genuinely absent or the card has been pulled, so the caller can't
// tell those cases apart from the bool alone - but on a negative
// result we run the throttled eject probe so the presence state
// catches up within one throttle window of a pull.
inline bool exists(const char* path) {
  if (!_detail::present_ref()) return false;
  BusGuard _bg;                        // SD.exists (+ verify_or_disable) on the shared HSPI bus
  const bool found = SD.exists(path);
  if (!found) verify_or_disable_throttled();
  return found;
}

// Open `path` for reading. Caller checks the result truthiness and
// is responsible for close(). Used by the attachment download
// endpoint to stream big blobs without loading them into RAM. On
// open-failure (file not found OR card ejected) we run the throttled
// eject probe so the presence flag flips when the card has genuinely
// gone - preventing the SPA from showing SD as mounted long after a
// mid-read eject.
inline File open_read(const char* path) {
  if (!_detail::present_ref()) return File();
  BusGuard _bg;                        // SD.open on the shared HSPI bus. NOTE: the
  // returned handle's later reads (download streaming) must also be guarded
  // by the caller with SDCard::BusGuard around each read.
  File f = SD.open(path, FILE_READ);
  if (!f) verify_or_disable_throttled();
  return f;
}
inline const char* card_type_name() {
  switch (_detail::card_type_ref()) {
    case CARD_MMC:  return "MMC";
    case CARD_SD:   return "SD";
    case CARD_SDHC: return "SDHC";
    case CARD_NONE: return "absent";
    default:        return "unknown";
  }
}

}  // namespace SDCard
}  // namespace Storage
