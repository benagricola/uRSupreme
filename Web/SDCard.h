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

namespace Web {
namespace SDCard {

namespace _detail {
  inline bool&     present_ref()   { static bool v = false; return v; }
  inline SPIClass*& spi_ref()      { static SPIClass* v = nullptr; return v; }
  inline uint8_t&  card_type_ref() { static uint8_t v = CARD_NONE; return v; }
}

// One-shot init. Safe to call when HAS_SD == false at the board level
// (just no-ops). Returns true if a card was detected and mounted.
inline bool begin() {
  // Compile-out path for boards without an SD slot. The pin constants
  // SD_* are only defined on T-Beam Supreme variants in Boards.h, so
  // gate everything on the same board check the .ino uses.
#if defined(BOARD_MODEL) && (BOARD_MODEL == BOARD_TBEAM_S_V1 || BOARD_MODEL == BOARD_TBEAM_S_LR_V1)
  // Dedicated SPI bus — HSPI is unused on this platform so we own it.
  _detail::spi_ref() = new SPIClass(HSPI);
  _detail::spi_ref()->begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, *_detail::spi_ref(), 4000000UL)) {
    NOTICE("SDCard: no card detected (or mount failed)");
    _detail::present_ref() = false;
    return false;
  }
  const uint8_t ct = SD.cardType();
  _detail::card_type_ref() = ct;
  if (ct == CARD_NONE) {
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
  NOTICEF("SDCard: mounted %s, total=%llu bytes used=%llu bytes",
          ct_name,
          (unsigned long long)SD.totalBytes(),
          (unsigned long long)SD.usedBytes());
  return true;
#else
  return false;
#endif
}

inline bool      present()      { return _detail::present_ref(); }
inline uint64_t  total_bytes()  { return _detail::present_ref() ? (uint64_t)SD.totalBytes() : 0; }
inline uint64_t  used_bytes()   { return _detail::present_ref() ? (uint64_t)SD.usedBytes()  : 0; }
inline uint8_t   card_type()    { return _detail::card_type_ref(); }
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
}  // namespace Web
