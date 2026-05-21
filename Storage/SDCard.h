// SD card driver for the T-Beam Supreme.
//
// The board exposes a microSD slot wired to a dedicated SPI bus
// (SD_MISO=37, SD_MOSI=35, SD_CLK=36, SD_CS=47) — separate from the
// LoRa modem's SPI on pins 11-13, so we can mount the card without
// stepping on the radio. The bus is shared with nothing else on this
// platform, so we own SPIClass(HSPI) for it.
//
// Self-detecting: begin() probes for a card and sets `_present` to
// the result. If no card is inserted at boot, the driver stays inert
// — no errors, just present=false. We don't currently hot-detect a
// card inserted later; the user reboots after inserting a card.
//
// Responsibilities for now (slice 1 of #122):
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
#include "../Boards.h"

namespace Storage {
namespace SDCard {

namespace _detail {
  inline bool&     present_ref()    { static bool v = false; return v; }
  inline SPIClass*& spi_ref()       { static SPIClass* v = nullptr; return v; }
  inline uint8_t&  card_type_ref()  { static uint8_t v = CARD_NONE; return v; }
  // Last diagnostic line from begin() — surfaced via the API when
  // probe fails so the user can tell "no card in slot" from
  // "card present, mount failed" from "wrong filesystem", etc.
  inline String& last_status_ref()  { static String v = "not_probed"; return v; }
  // Edge flag flipped by verify_or_disable on an ejection trip.
  // WebUI::loop drains this and fires the storage_changed WS event
  // — keeps the SD-aware code free of the WebSocket dependency.
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
// live on the HSPI bus on this hardware (#120/#122). Whichever module
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
  if (!SD.begin(SD_CS, *bus)) {
    _detail::last_status_ref() = "sd_begin_failed";
    NOTICE("SDCard: SD.begin() failed — card absent, wrong pinout, or unsupported FS (try FAT32)");
    _detail::present_ref() = false;
    return false;
  }
  const uint8_t ct = SD.cardType();
  _detail::card_type_ref() = ct;
  if (ct == CARD_NONE) {
    _detail::last_status_ref() = "card_type_none";
    NOTICE("SDCard: SD.begin succeeded but cardType == CARD_NONE — slot empty");
    SD.end();
    _detail::present_ref() = false;
    return false;
  }
  _detail::present_ref() = true;
  const char* ct_name = ct == CARD_MMC  ? "MMC"
                     : ct == CARD_SD   ? "SD"
                     : ct == CARD_SDHC ? "SDHC"
                     : "unknown";
  _detail::last_status_ref() = "mounted";
  NOTICEF("SDCard: mounted %s, total=%llu bytes used=%llu bytes",
          ct_name,
          (unsigned long long)SD.totalBytes(),
          (unsigned long long)SD.usedBytes());
  return true;
#else
  _detail::last_status_ref() = "no_slot_on_board";
  return false;
#endif
}

// Diagnostic string from the most recent begin() — values:
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
// failure paths — if the underlying SD op fails AND a free-space
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
  if (SD.totalBytes() > 0) return false;
  NOTICE("SDCard: ejection detected on write failure — disabling");
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
inline uint64_t  total_bytes()  { return _detail::present_ref() ? (uint64_t)SD.totalBytes() : 0; }
inline uint64_t  used_bytes()   { return _detail::present_ref() ? (uint64_t)SD.usedBytes()  : 0; }
inline uint8_t   card_type()    { return _detail::card_type_ref(); }

// ---- File helpers used by the LXMF attachment routing. ----

// Make sure every directory in `path` exists, walking down from
// the root. SD.mkdir is single-level only.
inline bool ensure_parent_dirs(const char* path) {
  if (!_detail::present_ref()) return false;
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
// tell those cases apart from the bool alone — but on a probe that
// suggests the card is gone (SD.exists() returns false AND a
// totalBytes() check fails) we trip verify_or_disable so the
// presence state catches up. The cost of the extra probe is paid
// only on the negative path.
inline bool exists(const char* path) {
  if (!_detail::present_ref()) return false;
  const bool found = SD.exists(path);
  if (!found) verify_or_disable();
  return found;
}

// Open `path` for reading. Caller checks the result truthiness and
// is responsible for close(). Used by the attachment download
// endpoint to stream big blobs without loading them into RAM. On
// open-failure (file not found OR card ejected) we run
// verify_or_disable so the presence flag flips when the card has
// genuinely gone — preventing the SPA from showing SD as mounted
// long after a mid-read eject.
inline File open_read(const char* path) {
  if (!_detail::present_ref()) return File();
  File f = SD.open(path, FILE_READ);
  if (!f) verify_or_disable();
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
