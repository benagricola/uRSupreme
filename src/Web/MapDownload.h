// Device-side map download: fetch a vector .pmtiles file from a URL
// straight to the SD card, on a dedicated background task so the main
// loop never blocks on the network or the SD
// bus. One job at a time, cancellable, with byte progress. The browser
// polls /api/map/download for status while a job runs.
//
// HTTP works with no TLS. HTTPS uses the IDF certificate bundle when the
// build has it (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE); without it, only
// http:// URLs connect. Sourcing the .pmtiles is the user's job (a
// regional Protomaps extract, or a self-hosted file); see tools/.
#pragma once

#include <Arduino.h>
#include <atomic>
#include <string>
#include <SD.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <Reticulum.h>
#include "../Storage/SDCard.h"

#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
#include <esp_crt_bundle.h>
#endif

namespace Web {
namespace MapDownload {

enum Phase : int { IDLE = 0, RUNNING = 1, DONE = 2, ERROR = 3, CANCELLED = 4 };

struct State {
  std::atomic<int>    phase{IDLE};
  std::atomic<size_t> written{0};
  std::atomic<long>   total{-1};      // content-length, -1 if unknown
  std::atomic<bool>   cancel{false};
  std::string         url;
  std::string         dest;
  std::string         error;          // set before phase = ERROR
  TaskHandle_t        task = nullptr;
};
inline State& st() { static State s; return s; }

inline const char* phase_str(int p) {
  switch (p) {
    case RUNNING:   return "running";
    case DONE:      return "done";
    case ERROR:     return "error";
    case CANCELLED: return "cancelled";
    default:        return "idle";
  }
}

inline void _finish(int phase, const char* err, esp_http_client_handle_t c,
                    File* f, uint8_t* buf, bool remove_dest) {
  State& s = st();
  if (f && *f) { Storage::SDCard::BusGuard g; f->close(); }
  if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
  if (buf) heap_caps_free(buf);
  if (remove_dest && !s.dest.empty()) { Storage::SDCard::BusGuard g; SD.remove(s.dest.c_str()); }
  if (err && *err) s.error = err;
  s.phase = phase;
  s.task = nullptr;
  vTaskDelete(nullptr);
}

inline void task_fn(void*) {
  State& s = st();
  // PSRAM chunk buffer: the web buffers must not churn internal SRAM.
  const size_t CHUNK = 8192;
  uint8_t* buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) buf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_8BIT);
  if (!buf) _finish(ERROR, "out of memory", nullptr, nullptr, nullptr, false);

  if (!Storage::SDCard::present()) _finish(ERROR, "no SD card", nullptr, nullptr, buf, false);

  esp_http_client_config_t cfg = {};
  cfg.url = s.url.c_str();
  cfg.timeout_ms = 20000;
  cfg.buffer_size = 4096;
#if defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE)
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) _finish(ERROR, "http init failed", nullptr, nullptr, buf, false);
  if (esp_http_client_open(c, 0) != ESP_OK) _finish(ERROR, "connect failed", c, nullptr, buf, false);
  const long content_len = esp_http_client_fetch_headers(c);
  const int status = esp_http_client_get_status_code(c);
  if (status != 200) {
    char e[40]; snprintf(e, sizeof(e), "http %d", status);
    _finish(ERROR, e, c, nullptr, buf, false);
  }
  s.total = content_len;   // -1 if the server sent no length / is chunked

  // Fresh file: remove any old one, ensure the parent dir, open for write.
  { Storage::SDCard::BusGuard g; if (SD.exists(s.dest.c_str())) SD.remove(s.dest.c_str()); }
  if (!Storage::SDCard::ensure_parent_dirs(s.dest.c_str())) _finish(ERROR, "mkdir failed", c, nullptr, buf, false);
  File f;
  { Storage::SDCard::BusGuard g; f = SD.open(s.dest.c_str(), FILE_WRITE); }
  if (!f) _finish(ERROR, "open dest failed", c, nullptr, buf, false);

  size_t written = 0;
  // Bail out if the peer stalls or closes early: a clean FIN short of the
  // content length leaves read() returning 0 with the data "incomplete",
  // which would otherwise spin forever. STALL_LIMIT * 5 ms ~= 20 s of no
  // progress before we give up.
  const int STALL_LIMIT = 4000;
  int stalls = 0;
  for (;;) {
    if (s.cancel) _finish(CANCELLED, nullptr, c, &f, buf, true);
    const int r = esp_http_client_read(c, (char*)buf, CHUNK);
    if (r < 0) _finish(ERROR, "read error", c, &f, buf, true);
    if (r == 0) {
      if (esp_http_client_is_complete_data_received(c)) break;
      if (++stalls > STALL_LIMIT) _finish(ERROR, "download stalled", c, &f, buf, true);
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    stalls = 0;
    size_t w;
    { Storage::SDCard::BusGuard g; w = f.write(buf, r); }
    if ((int)w != r) _finish(ERROR, "SD write failed", c, &f, buf, true);
    written += (size_t)r;
    s.written = written;
    RNS::Utilities::OS::reset_watchdog();
  }
  _finish(DONE, nullptr, c, &f, buf, false);
}

// Start a download of `url` -> SD `dest`. Returns false if a job is already
// running or the task could not be spawned.
inline bool start(const std::string& url, const std::string& dest) {
  State& s = st();
  if (s.phase == RUNNING) return false;
  s.url = url; s.dest = dest; s.error.clear();
  s.written = 0; s.total = -1; s.cancel = false; s.phase = RUNNING;
  // 12 KiB stack: HTTP + SD writes, with headroom for a TLS handshake.
  const BaseType_t ok = xTaskCreatePinnedToCore(task_fn, "mapdl", 12288, nullptr, 4, &s.task, 1);
  if (ok != pdPASS) { s.task = nullptr; s.phase = ERROR; s.error = "task spawn failed"; return false; }
  return true;
}

inline void cancel() { State& s = st(); if (s.phase == RUNNING) s.cancel = true; }

inline void fill_status(JsonObject o) {
  const State& s = st();
  const int p = s.phase;
  o["phase"]   = phase_str(p);
  o["active"]  = (p == RUNNING);
  o["written"] = (double)s.written.load();
  o["total"]   = s.total.load();      // -1 when unknown
  o["url"]     = s.url;
  o["dest"]    = s.dest;
  if (p == ERROR) o["error"] = s.error;
}

}  // namespace MapDownload
}  // namespace Web
