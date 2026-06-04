// Move persisted attachments from internal flash (LittleFS) to the SD
// card. Triggered from the SPA's storage popover when the user wants
// to reclaim flash space after inserting a card.
//
// Per file: copy bytes flash→SD in chunks, verify the destination size
// matches, delete the flash original. After each identity's directory
// is swept, walk inbox + outbox records and flip backend="flash"
// entries to "sd" so the SPA's SD-unavailable warning logic stays in
// sync with where the bytes actually live.
//
// Best-effort: if SD runs out of space or a file fails to copy, the
// original is left on flash and the operation continues with the
// next file.

#pragma once

#include <Arduino.h>
#include <SD.h>
#include <microStore/FileSystem.h>
#include "../LXMF/LXMFGateway.h"
#include "../Storage/SDCard.h"

namespace Storage {
namespace Migration {

struct Result {
  size_t   moved   = 0;
  size_t   skipped = 0;   // already on SD, or nothing to do
  size_t   failed  = 0;
  uint64_t bytes   = 0;
  size_t   records_updated = 0;
};

inline bool _copy_flash_to_sd_chunked(const std::string& path, size_t expected) {
  microStore::File src = filesystem.open(path.c_str(), microStore::File::ModeRead);
  if (!src) return false;
  if (Storage::SDCard::exists(path.c_str())) SD.remove(path.c_str());
  if (!Storage::SDCard::ensure_parent_dirs(path.c_str())) { src.close(); return false; }
  File dst = SD.open(path.c_str(), FILE_WRITE);
  if (!dst) { src.close(); return false; }
  uint8_t buf[1024];
  size_t copied = 0;
  while (copied < expected) {
    const size_t want = std::min((size_t)sizeof(buf), expected - copied);
    const size_t got = src.read(buf, want);
    if (got == 0) break;
    const size_t w = dst.write(buf, got);
    if (w != got) break;
    copied += got;
    RNS::Utilities::OS::reset_watchdog();
  }
  src.close();
  dst.close();
  if (copied != expected) {
    SD.remove(path.c_str());
    return false;
  }
  return true;
}

inline Result run() {
  Result r;
  if (!Storage::SDCard::present()) return r;
  // Walk every identity's attachments/ directory on flash. Each
  // identity owns its own subtree; we do not touch files outside it.
  for (auto* ap : LXMF::LXMFGateway::active_identities()) {
    if (!ap) continue;
    auto& a = *ap;
    const std::string att_dir = a.dir() + "/attachments";
    if (!filesystem.isDirectory(att_dir.c_str())) continue;
    auto entries = filesystem.listDirectory(att_dir.c_str());
    for (const auto& name : entries) {
      const std::string full = att_dir + "/" + name;
      if (!filesystem.exists(full.c_str())) continue;
      // Determine source size + skip-if-already-on-SD up front.
      microStore::File probe = filesystem.open(full.c_str(), microStore::File::ModeRead);
      if (!probe) { ++r.failed; continue; }
      const size_t flash_size = probe.size();
      probe.close();
      if (Storage::SDCard::exists(full.c_str())) {
        // Already on SD — drop the flash copy and count it as a skip.
        // (If the SD copy size disagrees, leave flash alone and flag
        // it as a failure for the user to investigate.)
        File sd_probe = SD.open(full.c_str(), FILE_READ);
        const size_t sd_size = sd_probe ? sd_probe.size() : 0;
        if (sd_probe) sd_probe.close();
        if (sd_size == flash_size) {
          filesystem.remove(full.c_str());
          ++r.skipped;
          continue;
        }
        ++r.failed;
        continue;
      }
      // Copy + verify + delete original.
      if (!_copy_flash_to_sd_chunked(full, flash_size)) {
        ++r.failed;
        continue;
      }
      filesystem.remove(full.c_str());
      ++r.moved;
      r.bytes += flash_size;
    }
    // Flip backend strings on the records that pointed at flash so the
    // SD-unavailable warning fires correctly if the card is later
    // removed. Both inbox + outbox can carry attachment metadata.
    if (a.inbox)  r.records_updated += a.inbox ->update_attachment_backends("flash", "sd");
    if (a.outbox) r.records_updated += a.outbox->update_attachment_backends("flash", "sd");
  }
  return r;
}

} // namespace Migration
} // namespace Storage
