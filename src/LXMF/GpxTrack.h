// GPX track file helpers for live position shares. A live-share message gets a
// .gpx attachment created with its first point; every later live sample appends
// one <trkpt> in constant time by seeking back over the closing tags and
// re-writing them - no whole-file rewrite, no in-RAM point buffer. The file is
// valid GPX after every append, so the web app renders it like any received
// .gpx (it detects GPX by the application/gpx+xml mime, not the .bin name).
//
// These run on the inbound LXMF delivery path (the main loop), so SD writes go
// through the shared Storage::SdWriter task, off the loop; only the one-time
// parent-dir mkdir stays inline (bounded, idempotent). Flash (microStore)
// writes stay inline: it is the fast, predictable backend and routing it
// off-loop would add cross-task filesystem access. Both backends support seek +
// mid-file write for the constant-time append.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <string>
#include <SD.h>
#include <microStore/FileSystem.h>
#include "../Clock/Manager.h"
#include "../Storage/SDCard.h"
#include "../Storage/Streaming.h"
#include "../Storage/SdWriter.h"

extern microStore::FileSystem filesystem;

namespace LXMF {
namespace GpxTrack {

inline constexpr char HEADER[] =
    "<?xml version=\"1.0\"?>\n<gpx version=\"1.1\" creator=\"uRSupreme\"><trk><trkseg>\n";
inline constexpr char CLOSE[] = "</trkseg></trk></gpx>\n";
// Cap on points in one live track. Bounds the file (~50 KB at 1000) and the
// per-append cost stays constant regardless. A longer share stops growing the
// track rather than rewriting to drop the oldest point.
inline constexpr uint32_t MAX_POINTS = 1000;

inline size_t fmt_trkpt(char* out, size_t cap, double lat, double lon) {
  const int n = snprintf(out, cap, "<trkpt lat=\"%.6f\" lon=\"%.6f\"/>\n", lat, lon);
  return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

// Create a new GPX containing the first point. The size is deterministic, so it
// is returned immediately even though the SD write commits off-loop; returns 0
// on a format error or a rejected/failed enqueue.
//
// Best-effort by design: a non-zero return means the write was queued, NOT that
// it is on disk. A later off-loop write failure (card pulled mid-share) is not
// reported to this caller - it surfaces only in the aggregate sd_job_errors
// counter, and the track simply stops rendering. This is acceptable for an
// auto-generated live-share track; making it durable would require blocking the
// LXMF delivery loop on the card, which is exactly what the shared writer avoids.
inline size_t create(const char* path, bool use_sd, double lat, double lon) {
  char pt[64];
  if (!fmt_trkpt(pt, sizeof(pt), lat, lon)) return 0;
  char body[256];
  const int n = snprintf(body, sizeof(body), "%s%s%s", HEADER, pt, CLOSE);
  if (n <= 0 || (size_t)n >= sizeof(body)) return 0;
  if (use_sd) {
    // The parent-dir mkdir stays inline (bounded, idempotent); the writer opens
    // the POSIX /sd path and commits the body off the loop.
    if (!Storage::SDCard::ensure_parent_dirs(path)) return 0;
    return Storage::SdWriter::write(path, (const uint8_t*)body, (uint32_t)n,
             Storage::SdWriter::Op::Truncate, 0, Storage::SdWriter::Kind::GpxCreate)
           ? (size_t)n : 0;
  }
  return Storage::Streaming::write_from_buffer(path, false, (const uint8_t*)body, (size_t)n)
         == (size_t)n ? (size_t)n : 0;
}

// Append one point by seeking back over the closing tags and re-writing them.
// Constant time in the file size. The SD write commits off-loop: the payload is
// the new point followed by a fresh CLOSE, and the writer seeks back clen bytes
// from EOF to overwrite the old CLOSE, so the file is valid GPX after every
// append. Returns true on success (for SD, on a successful enqueue).
inline bool append(const char* path, bool use_sd, double lat, double lon) {
  char pt[64];
  if (!fmt_trkpt(pt, sizeof(pt), lat, lon)) return false;
  const size_t plen = strlen(pt);
  const size_t clen = sizeof(CLOSE) - 1;   // exclude the NUL
  if (use_sd) {
    char buf[64 + sizeof(CLOSE)];          // new point + closing tags, one write
    memcpy(buf, pt, plen);
    memcpy(buf + plen, CLOSE, clen);
    return Storage::SdWriter::write(path, (const uint8_t*)buf,
             (uint32_t)(plen + clen), Storage::SdWriter::Op::AppendSeek,
             (uint32_t)clen, Storage::SdWriter::Kind::GpxAppend);
  }
  microStore::File f = filesystem.open(path, microStore::File::ModeReadWrite);
  if (!f) return false;
  const size_t sz = f.size();
  if (sz < clen) { f.close(); return false; }
  f.seek((uint32_t)(sz - clen), microStore::SeekModeSet);
  const bool ok = f.write((const uint8_t*)pt, plen) == plen
               && f.write((const uint8_t*)CLOSE, clen) == clen;
  f.close();
  return ok;
}

// A unique, human-friendly download name for a live track. With the wall
// clock set the name carries the start time (track-YYYYMMDD-HHMMSS.gpx in
// UTC); otherwise it falls back to the message sequence (track-<seq>.gpx).
// Either form is unique, so saving one track in a browser never overwrites
// another. The on-disk filename stays the seq-based <hash>_<seq>.bin; this
// only sets the download/display name the browser sees.
inline std::string download_name(uint32_t seq) {
  if (Clock::Manager::is_calibrated()) {
    const time_t t = (time_t)Clock::Manager::now_epoch();
    struct tm tmv;
    if (gmtime_r(&t, &tmv) != nullptr) {
      char buf[40];
      if (strftime(buf, sizeof(buf), "track-%Y%m%d-%H%M%S.gpx", &tmv) > 0)
        return std::string(buf);
    }
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "track-%08x.gpx", (unsigned)seq);
  return std::string(buf);
}

}  // namespace GpxTrack
}  // namespace LXMF
