// On-device PMTiles region extractor.
//
// Range-reads a remote Protomaps planet archive (build.protomaps.com, a
// ~127 GB v3 clustered PMTiles served over HTTPS with byte ranges) and
// writes a small regional .pmtiles to the SD card, so the device builds
// its own offline basemap with no second computer and no hosting. Only the
// bytes for the chosen area and zoom range are transferred.
//
// This is a port of go-pmtiles' extract, kept faithful to upstream:
//   - tile-id (Hilbert) math:  pmtiles/tile_id.go
//   - header + entry codec:    pmtiles/directory.go
//   - the extract algorithm:   pmtiles/extract.go (Extract, RelevantEntries,
//                              reencodeEntries)
//
// Divergences from upstream, all deliberate and bounded for the MCU:
//   - Relevance is a sorted tile-id list rasterized from a bounding box (the
//     only shape the pickers produce), not a roaring bitmap.
//   - The output uses internal_compression = none with a single flat root
//     directory (no leaf directories), so the device needs only gzip INFLATE
//     (in ROM) and never deflate. The root is read in one range when the map
//     opens; on local SD that is free.
//   - Source ranges are fetched un-merged (overfetch 0) and the connection
//     is reopened per range. Low-zoom tiles are contiguous in a clustered
//     planet, so a world extract is a single range.
//   - The region is capped (MAX_REGION_TILES); larger areas must use a lower
//     zoom. The job reports the tile count before any tile is fetched.
#pragma once

#include <Arduino.h>
#include <atomic>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <math.h>
#include <SD.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <Reticulum.h>
#include "../Common/PsramAllocator.h"
#include "../Storage/SDCard.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include <esp_crt_bundle.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern "C" {
  // ESP32-S3 ROM (esp32s3.rom.ld @ 0x4000084c): raw/zlib inflate. We feed
  // raw deflate (gzip framing stripped by hand) and let it write the whole
  // decompressed stream into a buffer sized from the gzip ISIZE trailer.
  size_t tinfl_decompress_mem_to_mem(void* pOut_buf, size_t out_buf_len,
                                     const void* pSrc_buf, size_t src_buf_len,
                                     int flags);
}

namespace Web {
namespace MapExtract {

// TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF: output buffer holds the whole
// decompressed stream (sized from ISIZE).
static const int    TINFL_NON_WRAPPING = 4;
static const size_t TINFL_FAILED       = (size_t)-1;

// Cap on tiles in one extract. Bounds the PSRAM entry tables and the
// download time; a larger area must drop to a lower zoom. World z0-7 is
// ~21.8k tiles, well under this.
static const uint32_t MAX_REGION_TILES = 100000;

// Planet TLS retries. Both the catalog resolve and opening the planet
// archive (reading its header) talk to a remote host over TLS, and the
// mbedTLS handshake needs a chunk of internal SRAM that this device runs
// short of. When the user starts a download while looking at the map, the
// browser is concurrently range-fetching tiles, and the handshake can lose
// that internal-SRAM race and fail ("could not find a planet build" at the
// catalog step, "fetch header failed" at the archive step). A few attempts
// with a short backoff let the tile burst drain and the memory free, so the
// fetch succeeds on a retry (reproduced: attempt 1 failed under load,
// attempt 2 succeeded). Both run on the off-loop extract task, so the
// backoff blocks nothing.
static const int      PLANET_TLS_TRIES      = 4;
static const uint32_t PLANET_TLS_BACKOFF_MS = 1500;

enum Phase : int { IDLE = 0, SCANNING = 1, WRITING = 2, DONE = 3, ERROR = 4, CANCELLED = 5 };

struct State {
  std::atomic<int>      phase{IDLE};
  std::atomic<uint32_t> tiles_total{0};
  std::atomic<uint64_t> bytes_total{0};   // tile-data bytes to fetch
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<bool>     cancel{false};
  std::string           url;
  std::string           dest;
  std::string           error;
  double                w = 0, s = 0, e = 0, n = 0;   // bbox, degrees (also the polygon's bounds)
  std::vector<double>   poly_lon, poly_lat;           // optional clip polygon; empty = use the bbox
  uint8_t               maxz = 0;
  TaskHandle_t          task = nullptr;
};
inline State& st() { static State s; return s; }

inline bool active() { int p = st().phase; return p == SCANNING || p == WRITING; }

inline const char* phase_str(int p) {
  switch (p) {
    case SCANNING:  return "scanning";
    case WRITING:   return "writing";
    case DONE:      return "done";
    case ERROR:     return "error";
    case CANCELLED: return "cancelled";
    default:        return "idle";
  }
}

// ---- PMTiles v3 primitives ------------------------------------------------

struct EntryV3 {
  uint64_t tile_id;
  uint64_t offset;
  uint32_t length;
  uint32_t run_length;
};
using EntryVec = Common::PsramVector<EntryV3>;
using SeenMap  = std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>,
                  std::equal_to<uint64_t>,
                  Common::PsramStdAllocator<std::pair<const uint64_t, uint64_t>>>;

// tile_id.go:7-30 - Hilbert rotate + ZxyToID.
inline void hilbert_rotate(uint32_t n, uint32_t& x, uint32_t& y, uint32_t rx, uint32_t ry) {
  if (ry == 0) {
    if (rx != 0) { x = n - 1 - x; y = n - 1 - y; }
    uint32_t t = x; x = y; y = t;
  }
}
inline uint64_t zxy_to_id(uint8_t z, uint32_t x, uint32_t y) {
  uint64_t acc = (((uint64_t)1 << (z * 2)) - 1) / 3;
  int n = (int)z - 1;
  for (uint32_t s = (z ? (uint32_t)1 << (z - 1) : 0); s > 0; s >>= 1) {
    uint32_t rx = s & x;
    uint32_t ry = s & y;
    acc += (uint64_t)((3 * rx) ^ ry) << n;
    hilbert_rotate(s, x, y, rx, ry);
    n--;
  }
  return acc;
}

// ---- varint (LEB128, unsigned) -------------------------------------------

inline void put_uvarint(Common::PsramVector<uint8_t>& out, uint64_t v) {
  while (v >= 0x80) { out.push_back((uint8_t)v | 0x80); v >>= 7; }
  out.push_back((uint8_t)v);
}
inline uint64_t get_uvarint(const uint8_t* p, size_t len, size_t& pos) {
  uint64_t result = 0; int shift = 0;
  while (pos < len) {
    uint8_t b = p[pos++];
    result |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) break;
    shift += 7;
  }
  return result;
}

// directory.go:325 DeserializeEntries (input already decompressed).
inline void deserialize_entries(const uint8_t* p, size_t len, EntryVec& out) {
  size_t pos = 0;
  uint64_t num = get_uvarint(p, len, pos);
  out.clear();
  out.resize(num);
  uint64_t last = 0;
  for (uint64_t i = 0; i < num; i++) { uint64_t d = get_uvarint(p, len, pos); out[i].tile_id = last + d; last += d; }
  for (uint64_t i = 0; i < num; i++) out[i].run_length = (uint32_t)get_uvarint(p, len, pos);
  for (uint64_t i = 0; i < num; i++) out[i].length     = (uint32_t)get_uvarint(p, len, pos);
  for (uint64_t i = 0; i < num; i++) {
    uint64_t t = get_uvarint(p, len, pos);
    if (i > 0 && t == 0) out[i].offset = out[i - 1].offset + out[i - 1].length;
    else                 out[i].offset = t - 1;
  }
}

// directory.go:275 SerializeEntries with NoCompression, on a contiguous slice.
inline void serialize_entries_range(const EntryV3* e, size_t cnt, Common::PsramVector<uint8_t>& out) {
  out.clear();
  put_uvarint(out, cnt);
  uint64_t last = 0;
  for (size_t i = 0; i < cnt; i++) { put_uvarint(out, e[i].tile_id - last); last = e[i].tile_id; }
  for (size_t i = 0; i < cnt; i++) put_uvarint(out, e[i].run_length);
  for (size_t i = 0; i < cnt; i++) put_uvarint(out, e[i].length);
  for (size_t i = 0; i < cnt; i++) {
    if (i > 0 && e[i].offset == e[i - 1].offset + e[i - 1].length) put_uvarint(out, 0);
    else put_uvarint(out, e[i].offset + 1);
  }
}

// directory.go:472-521 buildRootsLeaves + BuildDirectories, NoCompression.
// A flat uncompressed root would overrun a reader's ~16 KB initial fetch
// once it exceeds a few thousand entries; splitting into leaf directories
// keeps the root small (leaf pointers), and the reader fetches each leaf at
// its exact offset/length. Leaves sit past the metadata, beyond the initial
// fetch window, so they are never truncated.
inline void build_directories(const EntryVec& entries, int target_root_len,
                              Common::PsramVector<uint8_t>& root_out,
                              Common::PsramVector<uint8_t>& leaves_out) {
  root_out.clear(); leaves_out.clear();
  if (entries.size() < 16384) {
    serialize_entries_range(entries.data(), entries.size(), root_out);
    if ((int)root_out.size() <= target_root_len) return;   // single flat root
  }
  double leaf_size = (double)entries.size() / 3500.0;
  if (leaf_size < 4096) leaf_size = 4096;
  for (;;) {
    const size_t ls = (size_t)leaf_size;
    EntryVec root_entries;
    leaves_out.clear();
    Common::PsramVector<uint8_t> ser;
    for (size_t idx = 0; idx < entries.size(); idx += ls) {
      size_t end = idx + ls; if (end > entries.size()) end = entries.size();
      serialize_entries_range(entries.data() + idx, end - idx, ser);
      root_entries.push_back({entries[idx].tile_id, (uint64_t)leaves_out.size(), (uint32_t)ser.size(), 0});
      leaves_out.insert(leaves_out.end(), ser.begin(), ser.end());
    }
    serialize_entries_range(root_entries.data(), root_entries.size(), root_out);
    if ((int)root_out.size() <= target_root_len) return;
    leaf_size *= 1.2;
  }
}

// gzip -> raw bytes via the ROM inflater. Strips the gzip header, sizes the
// output from the ISIZE trailer, raw-inflates the deflate body. Returns the
// decompressed size, or 0 on failure. `*out` is a PSRAM buffer the caller frees.
inline size_t gunzip(const uint8_t* src, size_t n, uint8_t** out) {
  *out = nullptr;
  if (n < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8) return 0;
  uint8_t flg = src[3];
  size_t off = 10;
  if (flg & 4)  { if (off + 2 > n) return 0; uint16_t xl = src[off] | (src[off + 1] << 8); off += 2 + xl; }
  if (flg & 8)  { while (off < n && src[off] != 0) off++; off++; }   // FNAME
  if (flg & 16) { while (off < n && src[off] != 0) off++; off++; }   // FCOMMENT
  if (flg & 2)  { off += 2; }                                        // FHCRC
  if (off + 8 >= n) return 0;
  uint32_t isize = (uint32_t)src[n - 4] | ((uint32_t)src[n - 3] << 8) |
                   ((uint32_t)src[n - 2] << 16) | ((uint32_t)src[n - 1] << 24);
  if (isize == 0) return 0;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(isize, MALLOC_CAP_SPIRAM);
  if (!buf) return 0;
  size_t got = tinfl_decompress_mem_to_mem(buf, isize, src + off, n - off, TINFL_NON_WRAPPING);
  if (got == TINFL_FAILED || got != isize) { heap_caps_free(buf); return 0; }
  *out = buf;
  return got;
}

// ---- slippy bbox -> tile rectangle ---------------------------------------

inline uint32_t lon_to_x(double lon, uint8_t z) {
  double n = (double)(1u << z);
  long v = (long)floor((lon + 180.0) / 360.0 * n);
  if (v < 0) v = 0;
  long mx = (long)((1u << z) - 1);
  if (v > mx) v = mx;
  return (uint32_t)v;
}
inline uint32_t lat_to_y(double lat, uint8_t z) {
  if (lat >  85.05112878) lat =  85.05112878;
  if (lat < -85.05112878) lat = -85.05112878;
  double r = lat * M_PI / 180.0;
  double n = (double)(1u << z);
  long v = (long)floor((1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * n);
  if (v < 0) v = 0;
  long my = (long)((1u << z) - 1);
  if (v > my) v = my;
  return (uint32_t)v;
}
// Fractional tile coordinates (Web Mercator), for polygon scanline fill.
inline double lon_to_xf(double lon, uint8_t z) { return (lon + 180.0) / 360.0 * (double)(1u << z); }
inline double lat_to_yf(double lat, uint8_t z) {
  if (lat >  85.05112878) lat =  85.05112878;
  if (lat < -85.05112878) lat = -85.05112878;
  double r = lat * M_PI / 180.0;
  return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * (double)(1u << z);
}

// ---- HTTP range reads -----------------------------------------------------

inline bool read_exact(esp_http_client_handle_t c, uint8_t* dst, size_t len) {
  size_t got = 0; int idle = 0;
  while (got < len) {
    int r = esp_http_client_read(c, (char*)dst + got, len - got);
    if (r < 0) return false;
    if (r == 0) {
      if (esp_http_client_is_complete_data_received(c)) break;
      if (++idle > 4000) return false;   // ~20 s of no progress
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    idle = 0; got += r;
    RNS::Utilities::OS::reset_watchdog();
  }
  return got == len;
}

// Fetch [off, off+len) into a fresh PSRAM buffer (small reads). Caller frees.
inline uint8_t* fetch_range(esp_http_client_handle_t c, uint64_t off, uint64_t len) {
  char range[64];
  snprintf(range, sizeof(range), "bytes=%llu-%llu",
           (unsigned long long)off, (unsigned long long)(off + len - 1));
  esp_http_client_set_header(c, "Range", range);
  if (esp_http_client_open(c, 0) != ESP_OK) return nullptr;
  esp_http_client_fetch_headers(c);
  if (esp_http_client_get_status_code(c) != 206) { esp_http_client_close(c); return nullptr; }
  uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) { esp_http_client_close(c); return nullptr; }
  bool ok = read_exact(c, buf, len);
  esp_http_client_close(c);
  if (!ok) { heap_caps_free(buf); return nullptr; }
  return buf;
}

// Fetch [off,off+len) of a directory/metadata block, decompressed. `gz`
// selects gzip inflate vs verbatim. Returns a PSRAM buffer + size. Caller frees.
inline uint8_t* fetch_inflate(esp_http_client_handle_t c, uint64_t off, uint64_t len,
                              bool gz, size_t* out_len) {
  uint8_t* raw = fetch_range(c, off, len);
  if (!raw) return nullptr;
  if (!gz) { *out_len = len; return raw; }
  uint8_t* inflated = nullptr;
  size_t got = gunzip(raw, len, &inflated);
  heap_caps_free(raw);
  if (!got) return nullptr;
  *out_len = got;
  return inflated;
}

// ---- header parse --------------------------------------------------------

struct Header {
  uint64_t root_offset, root_length;
  uint64_t metadata_offset, metadata_length;
  uint64_t leaf_offset, leaf_length;
  uint64_t tile_data_offset, tile_data_length;
  uint8_t  clustered, internal_compression, tile_compression, tile_type;
  uint8_t  min_zoom, max_zoom;
};
inline uint64_t rd_u64(const uint8_t* d, int o) {
  uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)d[o + i] << (8 * i); return v;
}
inline bool parse_header(const uint8_t* d, Header& h) {
  if (memcmp(d, "PMTiles", 7) != 0 || d[7] > 3) return false;
  h.root_offset = rd_u64(d, 8);       h.root_length = rd_u64(d, 16);
  h.metadata_offset = rd_u64(d, 24);  h.metadata_length = rd_u64(d, 32);
  h.leaf_offset = rd_u64(d, 40);      h.leaf_length = rd_u64(d, 48);
  h.tile_data_offset = rd_u64(d, 56); h.tile_data_length = rd_u64(d, 64);
  h.clustered = d[96]; h.internal_compression = d[97];
  h.tile_compression = d[98]; h.tile_type = d[99];
  h.min_zoom = d[100]; h.max_zoom = d[101];
  return true;
}
inline void put_u64(uint8_t* b, int o, uint64_t v) { for (int i = 0; i < 8; i++) b[o + i] = (uint8_t)(v >> (8 * i)); }
inline void put_u32(uint8_t* b, int o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (uint8_t)(v >> (8 * i)); }

// ---- the extract job ------------------------------------------------------

inline void _finish(int phase, const char* err, esp_http_client_handle_t c, File* f) {
  State& s = st();
  if (f && *f) { Storage::SDCard::BusGuard g; f->close(); }
  if (c) esp_http_client_cleanup(c);
  if (err && *err) s.error = err;
  if (phase != DONE && !s.dest.empty()) { Storage::SDCard::BusGuard g; SD.remove(s.dest.c_str()); }
  s.phase = phase;
  s.task = nullptr;
  vTaskDelete(nullptr);
}

// Last planet URL resolved this session. Once the catalog has been read
// once, a later flaky fetch falls back to this rather than failing the
// download: old planet builds stay served for weeks, so a slightly stale
// URL still works. Session-only (no persistence) to avoid an on-disk schema
// change; the reported failure is a same-session retry.
inline std::string& cached_planet_url() { static std::string u; return u; }

// One catalog fetch. The build catalog is a chronological JSON array; the
// last "key" is the newest archive. The browser cannot read it (no CORS),
// so the device does. Returns the full https URL, or empty on failure.
inline std::string fetch_planet_catalog() {
  const char* CATALOG = "https://build-metadata.protomaps.dev/builds.json";
  const char* BASE    = "https://build.protomaps.com/";
  esp_http_client_config_t cfg = {};
  cfg.url = CATALOG; cfg.timeout_ms = 15000; cfg.buffer_size = 4096;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return "";
  std::string out;
  if (esp_http_client_open(c, 0) == ESP_OK) {
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) == 200) {
      Common::PsramVector<char> body;
      char tmp[2048]; int r;
      while ((r = esp_http_client_read(c, tmp, sizeof(tmp))) > 0) {
        body.insert(body.end(), tmp, tmp + r);
        if (body.size() > (2u << 20)) break;   // sane cap
        RNS::Utilities::OS::reset_watchdog();
      }
      // last `"key":"....pmtiles"` is the newest build
      const char* NEEDLE = "\"key\":\"";
      const size_t nlen = 7;
      for (size_t i = body.size() >= nlen ? body.size() - nlen : 0; ; i--) {
        if (i + nlen <= body.size() && memcmp(body.data() + i, NEEDLE, nlen) == 0) {
          size_t p = i + nlen, q = p;
          while (q < body.size() && body[q] != '"') q++;
          std::string key(body.data() + p, q - p);
          if (key.find(".pmtiles") != std::string::npos) out = std::string(BASE) + key;
          break;
        }
        if (i == 0) break;
      }
    }
  }
  esp_http_client_close(c); esp_http_client_cleanup(c);
  return out;
}

// Resolve the newest Protomaps planet build, retrying the catalog fetch: the
// TLS handshake can lose the internal-SRAM race under concurrent tile
// serving, and a short backoff lets that pressure subside. On total failure
// fall back to the URL we resolved earlier this session, if any.
inline std::string resolve_latest_planet() {
  for (int attempt = 0; attempt < PLANET_TLS_TRIES; attempt++) {
    std::string u = fetch_planet_catalog();
    if (!u.empty()) { cached_planet_url() = u; return u; }
    if (attempt + 1 < PLANET_TLS_TRIES) {
      vTaskDelay(pdMS_TO_TICKS(PLANET_TLS_BACKOFF_MS));
      RNS::Utilities::OS::reset_watchdog();
    }
  }
  return cached_planet_url();
}

inline void task_fn(void*) {
  State& s = st();

  // A blank url means "the latest Protomaps planet"; resolve it first.
  if (s.url.empty()) {
    std::string planet = resolve_latest_planet();
    if (planet.empty()) _finish(ERROR, "could not find a planet build", nullptr, nullptr);
    s.url = planet;
  }

  esp_http_client_config_t cfg = {};
  cfg.url = s.url.c_str();
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 4096;
  cfg.keep_alive_enable = true;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  // 1. header. Open the archive and read its 127-byte header, retrying the
  //    same way the catalog resolve does: the TLS handshake here can also
  //    lose the internal-SRAM race under concurrent tile serving ("fetch
  //    header failed"). A failed attempt drops the client and backs off so
  //    the load can drain before the next handshake.
  esp_http_client_handle_t c = nullptr;
  uint8_t* hb = nullptr;
  for (int attempt = 0; attempt < PLANET_TLS_TRIES; attempt++) {
    c = esp_http_client_init(&cfg);
    if (c) {
      hb = fetch_range(c, 0, 127);
      if (hb) break;                        // header read OK
      esp_http_client_cleanup(c); c = nullptr;   // drop the failed connection
    }
    if (attempt + 1 < PLANET_TLS_TRIES) {
      vTaskDelay(pdMS_TO_TICKS(PLANET_TLS_BACKOFF_MS));
      RNS::Utilities::OS::reset_watchdog();
    }
  }
  if (!c) _finish(ERROR, "http init failed", nullptr, nullptr);
  if (!hb) _finish(ERROR, "fetch header failed", c, nullptr);
  Header h{}; bool okh = parse_header(hb, h); heap_caps_free(hb);
  if (!okh) _finish(ERROR, "not a pmtiles archive", c, nullptr);
  if (!h.clustered) _finish(ERROR, "source not clustered", c, nullptr);
  if (h.tile_type != 1) _finish(ERROR, "source is not vector (mvt)", c, nullptr);
  if (h.internal_compression != 1 && h.internal_compression != 2)
    _finish(ERROR, "unsupported dir compression", c, nullptr);
  const bool gz = (h.internal_compression == 2);

  uint8_t maxz = s.maxz;
  if (maxz > h.max_zoom) maxz = h.max_zoom;
  const uint8_t minz = 0;

  // 2. relevance: the set of wanted tiles (z 0..maxz) as a sorted tile-id
  //    list. With a clip polygon, scanline-fill it per zoom so only tiles
  //    inside the shape are kept (no surrounding sea); otherwise fill the
  //    bbox rectangle. The cap guards both against runaway areas.
  Common::PsramVector<uint64_t> rel;
  {
    const long axis_max = (1L << maxz);
    const bool use_poly = (s.poly_lon.size() >= 3 && s.poly_lon.size() == s.poly_lat.size());
    uint64_t count = 0;
    for (uint8_t z = minz; z <= maxz; z++) {
      if (use_poly) {
        // project the polygon to fractional tile space at this zoom
        const size_t nv = s.poly_lon.size();
        Common::PsramVector<double> px, py;
        px.reserve(nv); py.reserve(nv);
        double ymin = 1e18, ymax = -1e18;
        for (size_t i = 0; i < nv; i++) {
          double X = lon_to_xf(s.poly_lon[i], z), Y = lat_to_yf(s.poly_lat[i], z);
          px.push_back(X); py.push_back(Y);
          if (Y < ymin) ymin = Y; if (Y > ymax) ymax = Y;
        }
        long y0 = (long)floor(ymin), y1 = (long)floor(ymax);
        if (y0 < 0) y0 = 0; if (y1 > axis_max - 1) y1 = axis_max - 1;
        Common::PsramVector<double> xs;
        for (long y = y0; y <= y1; y++) {
          const double yc = (double)y + 0.5;
          xs.clear();
          for (size_t i = 0, j = nv - 1; i < nv; j = i++) {
            double yi = py[i], yj = py[j];
            if ((yi <= yc && yj > yc) || (yj <= yc && yi > yc)) {
              double t = (yc - yi) / (yj - yi);
              xs.push_back(px[i] + t * (px[j] - px[i]));
            }
          }
          std::sort(xs.begin(), xs.end());
          for (size_t k = 0; k + 1 < xs.size(); k += 2) {
            long xL = (long)floor(xs[k]), xR = (long)floor(xs[k + 1]);
            if (xL < 0) xL = 0; if (xR > axis_max - 1) xR = axis_max - 1;
            for (long x = xL; x <= xR; x++) {
              if (++count > MAX_REGION_TILES) _finish(ERROR, "area too large; lower the zoom", c, nullptr);
              rel.push_back(zxy_to_id(z, (uint32_t)x, (uint32_t)y));
            }
          }
        }
      } else {
        uint32_t x0 = lon_to_x(s.w, z), x1 = lon_to_x(s.e, z);
        uint32_t y0 = lat_to_y(s.n, z), y1 = lat_to_y(s.s, z);  // n -> smaller y
        if (x1 < x0) { uint32_t t = x0; x0 = x1; x1 = t; }
        if (y1 < y0) { uint32_t t = y0; y0 = y1; y1 = t; }
        count += (uint64_t)(x1 - x0 + 1) * (uint64_t)(y1 - y0 + 1);
        if (count > MAX_REGION_TILES) _finish(ERROR, "area too large; lower the zoom", c, nullptr);
        for (uint32_t x = x0; x <= x1; x++)
          for (uint32_t y = y0; y <= y1; y++)
            rel.push_back(zxy_to_id(z, x, y));
      }
    }
    std::sort(rel.begin(), rel.end());
    rel.erase(std::unique(rel.begin(), rel.end()), rel.end());
  }
  if (rel.empty()) _finish(ERROR, "no tiles in this area", c, nullptr);
  auto rel_contains = [&](uint64_t id) -> bool {
    return std::binary_search(rel.begin(), rel.end(), id);
  };
  auto rel_intersects = [&](uint64_t lo, uint64_t hi) -> bool {
    auto it = std::lower_bound(rel.begin(), rel.end(), lo);
    return it != rel.end() && *it < hi;
  };

  s.phase = SCANNING;
  const uint64_t last_tile = zxy_to_id(maxz + 1, 0, 0);

  // RelevantEntries (extract.go:31).
  EntryVec tiles;
  auto relevant = [&](const EntryVec& dir, EntryVec& leaves_out) {
    for (size_t idx = 0; idx < dir.size(); idx++) {
      const EntryV3& e = dir[idx];
      if (e.run_length == 0) {
        uint64_t hi = (idx == dir.size() - 1) ? last_tile : dir[idx + 1].tile_id;
        if (rel_intersects(e.tile_id, hi)) leaves_out.push_back(e);
      } else if (e.run_length == 1) {
        if (rel_contains(e.tile_id)) tiles.push_back(e);
      } else {
        uint64_t cur_id = e.tile_id; uint32_t cur_run = 0;
        for (uint64_t y = e.tile_id; y < e.tile_id + e.run_length; y++) {
          if (rel_contains(y)) { if (cur_run == 0) { cur_run = 1; cur_id = y; } else cur_run++; }
          else { if (cur_run > 0) tiles.push_back({cur_id, e.offset, e.length, cur_run}); cur_run = 0; }
        }
        if (cur_run > 0) tiles.push_back({cur_id, e.offset, e.length, cur_run});
      }
    }
  };

  // 3. root + 4. relevant leaves
  {
    size_t rlen = 0;
    uint8_t* rd = fetch_inflate(c, h.root_offset, h.root_length, gz, &rlen);
    if (!rd) _finish(ERROR, "fetch root dir failed", c, nullptr);
    EntryVec root; deserialize_entries(rd, rlen, root); heap_caps_free(rd);
    EntryVec leaves;
    relevant(root, leaves);
    for (const auto& leaf : leaves) {
      if (s.cancel) _finish(CANCELLED, nullptr, c, nullptr);
      size_t llen = 0;
      uint8_t* ld = fetch_inflate(c, h.leaf_offset + leaf.offset, leaf.length, gz, &llen);
      if (!ld) _finish(ERROR, "fetch leaf dir failed", c, nullptr);
      EntryVec leafdir; deserialize_entries(ld, llen, leafdir); heap_caps_free(ld);
      EntryVec sub;
      relevant(leafdir, sub);
      if (!sub.empty()) _finish(ERROR, "nested leaf dirs unsupported", c, nullptr);
    }
  }
  if (tiles.empty()) _finish(ERROR, "no tiles in this area", c, nullptr);
  std::sort(tiles.begin(), tiles.end(),
            [](const EntryV3& a, const EntryV3& b) { return a.tile_id < b.tile_id; });

  // 5. reencode in place (extract.go:92): contiguous dst offsets, dedup
  //    shared source tiles; collect source byte ranges to copy.
  struct SrcDst { uint64_t src, dst, len; };
  Common::PsramVector<SrcDst> ranges;
  uint64_t addressed = 0, dst = 0, tile_contents = 0;
  {
    SeenMap seen;
    for (auto& e : tiles) {
      uint64_t src = e.offset;
      auto it = seen.find(src);
      if (it != seen.end()) {
        e.offset = it->second;
      } else {
        if (!ranges.empty() && ranges.back().src + ranges.back().len == src)
          ranges.back().len += e.length;
        else
          ranges.push_back({src, dst, (uint64_t)e.length});
        seen[src] = dst;
        e.offset = dst;
        dst += e.length;
      }
      addressed += e.run_length;
    }
    tile_contents = seen.size();
  }
  const uint64_t tiledata_length = dst;

  rel.clear(); rel.shrink_to_fit();   // relevance no longer needed

  // 6. directories: a small root (leaf pointers when needed) + leaf section,
  //    both uncompressed. Root kept <= 16 KB for readers' initial fetch.
  Common::PsramVector<uint8_t> root_bytes, leaf_bytes;
  build_directories(tiles, 16384 - 127, root_bytes, leaf_bytes);

  // 7. metadata (decompress; stored uncompressed to match internal=none)
  size_t meta_len = 0;
  uint8_t* meta = fetch_inflate(c, h.metadata_offset, h.metadata_length, gz, &meta_len);
  if (!meta) _finish(ERROR, "fetch metadata failed", c, nullptr);

  // 8. new header
  uint8_t hdr[127]; memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, "PMTiles", 7); hdr[7] = 3;
  const uint64_t out_root_off = 127;
  const uint64_t out_meta_off = out_root_off + root_bytes.size();
  const uint64_t out_leaf_off = out_meta_off + meta_len;
  const uint64_t out_tile_off = out_leaf_off + leaf_bytes.size();
  put_u64(hdr, 8, out_root_off);   put_u64(hdr, 16, root_bytes.size());
  put_u64(hdr, 24, out_meta_off);  put_u64(hdr, 32, meta_len);
  put_u64(hdr, 40, out_leaf_off);  put_u64(hdr, 48, leaf_bytes.size());
  put_u64(hdr, 56, out_tile_off);  put_u64(hdr, 64, tiledata_length);
  put_u64(hdr, 72, addressed);     put_u64(hdr, 80, tiles.size());
  put_u64(hdr, 88, tile_contents);
  hdr[96] = 1;                      // clustered
  hdr[97] = 1;                      // internal compression: none
  hdr[98] = h.tile_compression;     // tiles copied verbatim
  hdr[99] = h.tile_type;
  hdr[100] = minz; hdr[101] = maxz;
  put_u32(hdr, 102, (uint32_t)(int32_t)(s.w * 1e7));
  put_u32(hdr, 106, (uint32_t)(int32_t)(s.s * 1e7));
  put_u32(hdr, 110, (uint32_t)(int32_t)(s.e * 1e7));
  put_u32(hdr, 114, (uint32_t)(int32_t)(s.n * 1e7));
  hdr[118] = minz;
  put_u32(hdr, 119, (uint32_t)(int32_t)((s.w + s.e) / 2 * 1e7));
  put_u32(hdr, 123, (uint32_t)(int32_t)((s.s + s.n) / 2 * 1e7));

  // 9. write placeholder header, root, metadata, then stream tile bytes.
  s.tiles_total = (uint32_t)tiles.size();
  s.bytes_total = tiledata_length;
  s.bytes_done = 0;
  s.phase = WRITING;

  { Storage::SDCard::BusGuard g; if (SD.exists(s.dest.c_str())) SD.remove(s.dest.c_str()); }
  if (!Storage::SDCard::ensure_parent_dirs(s.dest.c_str())) { heap_caps_free(meta); _finish(ERROR, "mkdir failed", c, nullptr); }
  File f;
  { Storage::SDCard::BusGuard g; f = SD.open(s.dest.c_str(), FILE_WRITE); }
  if (!f) { heap_caps_free(meta); _finish(ERROR, "open dest failed", c, nullptr); }
  {
    Storage::SDCard::BusGuard g;
    uint8_t zero[127]; memset(zero, 0, sizeof(zero));
    f.write(zero, sizeof(zero));
    f.write(root_bytes.data(), root_bytes.size());
    f.write(meta, meta_len);
    if (!leaf_bytes.empty()) f.write(leaf_bytes.data(), leaf_bytes.size());
  }
  heap_caps_free(meta);

  const size_t CHUNK = 8192;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
  if (!buf) _finish(ERROR, "out of memory", c, &f);
  uint64_t written = 0;
  for (const auto& rg : ranges) {
    if (s.cancel) { heap_caps_free(buf); _finish(CANCELLED, nullptr, c, &f); }
    char range[64];
    snprintf(range, sizeof(range), "bytes=%llu-%llu",
             (unsigned long long)(h.tile_data_offset + rg.src),
             (unsigned long long)(h.tile_data_offset + rg.src + rg.len - 1));
    esp_http_client_set_header(c, "Range", range);
    if (esp_http_client_open(c, 0) != ESP_OK) { heap_caps_free(buf); _finish(ERROR, "tile range open failed", c, &f); }
    esp_http_client_fetch_headers(c);
    if (esp_http_client_get_status_code(c) != 206) { esp_http_client_close(c); heap_caps_free(buf); _finish(ERROR, "tile range not 206", c, &f); }
    uint64_t remaining = rg.len; int idle = 0;
    while (remaining > 0) {
      if (s.cancel) { esp_http_client_close(c); heap_caps_free(buf); _finish(CANCELLED, nullptr, c, &f); }
      size_t want = remaining < CHUNK ? (size_t)remaining : CHUNK;
      int r = esp_http_client_read(c, (char*)buf, want);
      if (r < 0) { esp_http_client_close(c); heap_caps_free(buf); _finish(ERROR, "tile read error", c, &f); }
      if (r == 0) {
        if (esp_http_client_is_complete_data_received(c)) break;
        if (++idle > 4000) { esp_http_client_close(c); heap_caps_free(buf); _finish(ERROR, "tile stalled", c, &f); }
        vTaskDelay(pdMS_TO_TICKS(5));
        continue;
      }
      idle = 0;
      size_t wn; { Storage::SDCard::BusGuard g; wn = f.write(buf, r); }
      if ((int)wn != r) { esp_http_client_close(c); heap_caps_free(buf); _finish(ERROR, "SD write failed", c, &f); }
      remaining -= r; written += r; s.bytes_done = written;
      RNS::Utilities::OS::reset_watchdog();
    }
    esp_http_client_close(c);
    if (remaining != 0) { heap_caps_free(buf); _finish(ERROR, "tile short read", c, &f); }
  }
  heap_caps_free(buf);
  { Storage::SDCard::BusGuard g; f.close(); }

  // 10. write the real header last, so a cancelled job never leaves a
  //     valid-looking archive over incomplete tiles.
  {
    Storage::SDCard::BusGuard g;
    File hf = SD.open(s.dest.c_str(), "r+");
    if (!hf) _finish(ERROR, "reopen for header failed", c, nullptr);
    hf.seek(0);
    hf.write(hdr, sizeof(hdr));
    hf.close();
  }

  // Sidecar: the clip polygon used, so the manage UI can show the area's true
  // shape (not just its bbox) when deciding whether to delete it. Written next
  // to the archive as <id>.json. Best-effort; a bbox-only area has no polygon.
  if (!s.poly_lon.empty()) {
    std::string side = s.dest;
    size_t dot = side.rfind(".pmtiles");
    if (dot != std::string::npos) side = side.substr(0, dot) + ".json";
    std::string j = "{\"polygon\":[";
    char num[48];
    for (size_t i = 0; i < s.poly_lon.size(); i++) {
      snprintf(num, sizeof(num), "%s[%.6f,%.6f]", i ? "," : "", s.poly_lon[i], s.poly_lat[i]);
      j += num;
    }
    j += "]}";
    Storage::SDCard::BusGuard g;
    File sf = SD.open(side.c_str(), FILE_WRITE);
    if (sf) { sf.write((const uint8_t*)j.data(), j.size()); sf.close(); }
  }

  esp_http_client_cleanup(c);
  s.phase = DONE;
  s.task = nullptr;
  vTaskDelete(nullptr);
}

// Start an extract of bbox [w,s,e,n] (degrees) at zooms 0..maxz from the
// planet at `url` into SD `dest`. An optional clip polygon (lon[]/lat[],
// >=3 matching points) keeps only tiles inside the shape. False if a job is
// already running or the task could not spawn.
inline bool start(const std::string& url, const std::string& dest,
                  double w, double s_, double e, double n, uint8_t maxz,
                  const std::vector<double>& poly_lon = {},
                  const std::vector<double>& poly_lat = {}) {
  State& s = st();
  if (active()) return false;
  s.url = url; s.dest = dest; s.error.clear();
  s.w = w; s.s = s_; s.e = e; s.n = n; s.maxz = maxz;
  s.poly_lon = poly_lon; s.poly_lat = poly_lat;
  s.tiles_total = 0; s.bytes_total = 0; s.bytes_done = 0;
  s.cancel = false; s.phase = SCANNING;
  // 16 KiB stack: HTTP + TLS handshake + inflate + std::sort; the big tables
  // live in PSRAM, not on the stack.
  const BaseType_t ok = xTaskCreatePinnedToCore(task_fn, "mapext", 16384, nullptr, 4, &s.task, 1);
  if (ok != pdPASS) { s.task = nullptr; s.phase = ERROR; s.error = "task spawn failed"; return false; }
  return true;
}

inline void cancel() { State& s = st(); if (active()) s.cancel = true; }

inline void fill_status(JsonObject o) {
  const State& s = st();
  const int p = s.phase;
  o["phase"]       = phase_str(p);
  o["active"]      = active();
  o["tiles_total"] = s.tiles_total.load();
  o["bytes_total"] = (double)s.bytes_total.load();
  o["bytes_done"]  = (double)s.bytes_done.load();
  o["dest"]        = s.dest;
  if (p == ERROR) o["error"] = s.error;
}

}  // namespace MapExtract
}  // namespace Web
