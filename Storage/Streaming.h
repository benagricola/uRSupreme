// Chunked file write that never materialises the whole payload in a
// single contiguous buffer. Caller supplies a reader callable that
// the helper invokes repeatedly with (dst, offset, want) until the
// declared total is satisfied. The reader returns the number of
// bytes it actually filled (or 0 to stop early on its end).
//
// Two backends behind one signature:
//   * use_sd=true  → Arduino SD library (SD.open / f.write / f.close)
//   * use_sd=false → microStore::filesystem (ModeWrite, truncate-then-write)
//
// Returns the byte count actually written; 0 on open failure, or any
// value < total on a short write. Callers compare against `total`
// to decide success.
//
// Used by inbound and outbound attachment persist in LXMFGateway so
// neither path has to keep an attachment-sized contiguous buffer in
// RAM/PSRAM around just to call the convenience writeFile() API.

#pragma once

#include <stdint.h>
#include <functional>
#include <SD.h>
#include <microStore/FileSystem.h>
#include "SDCard.h"

extern microStore::FileSystem filesystem;

namespace Storage {
namespace Streaming {

using ChunkReader = std::function<size_t(uint8_t* dst, size_t offset, size_t want)>;

inline constexpr size_t DEFAULT_CHUNK = 1024;

inline size_t write_streamed(const char* path, bool use_sd,
                             size_t total, ChunkReader reader,
                             size_t chunk_size = DEFAULT_CHUNK) {
  if (!path || !*path || !reader) return 0;
  if (chunk_size == 0) chunk_size = DEFAULT_CHUNK;
  uint8_t buf[DEFAULT_CHUNK];
  // Cap requested chunk to our stack buffer. Callers asking for a
  // larger chunk silently get DEFAULT_CHUNK; growing the stack frame
  // for one call site isn't worth it.
  if (chunk_size > sizeof(buf)) chunk_size = sizeof(buf);

  size_t off = 0;
  if (use_sd) {
    if (!SDCard::ensure_parent_dirs(path)) return 0;
    if (SDCard::exists(path)) SD.remove(path);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return 0;
    while (off < total) {
      const size_t want = (total - off) < chunk_size ? (total - off) : chunk_size;
      const size_t got = reader(buf, off, want);
      if (got == 0) break;
      const size_t w = f.write(buf, got);
      if (w != got) { off += w; break; }
      off += got;
      RNS::Utilities::OS::reset_watchdog();
    }
    f.close();
    return off;
  }
  // microStore path. writeFile() in the FS impl does remove-then-open
  // for truncate semantics; do the same so behaviour matches when the
  // file already exists.
  filesystem.remove(path);
  microStore::File f = filesystem.open(path, microStore::File::ModeWrite, true);
  if (!f) return 0;
  while (off < total) {
    const size_t want = (total - off) < chunk_size ? (total - off) : chunk_size;
    const size_t got = reader(buf, off, want);
    if (got == 0) break;
    const size_t w = f.write(buf, got);
    if (w != got) { off += w; break; }
    off += got;
    RNS::Utilities::OS::reset_watchdog();
  }
  f.close();
  return off;
}

// Convenience: stream from a contiguous source pointer. Removes the
// need for callers to write a trivial reader lambda when the source
// is already a flat buffer (inbound persist, where f.raw points into
// the wire-decode buffer).
inline size_t write_from_buffer(const char* path, bool use_sd,
                                const uint8_t* src, size_t total) {
  return write_streamed(path, use_sd, total,
    [src, total](uint8_t* dst, size_t off, size_t want) -> size_t {
      if (off >= total) return 0;
      const size_t take = (total - off) < want ? (total - off) : want;
      memcpy(dst, src + off, take);
      return take;
    });
}

}  // namespace Streaming
}  // namespace Storage
