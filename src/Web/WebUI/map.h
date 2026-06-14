// Auto-extracted style: included from inside the Web::WebUI class body
// (no include guard, not a standalone translation unit).
//
// Map tiles for the web app's location view. Tiles live on the SD card
// in the standard slippy-map layout <maps_dir>/{z}/{x}/{y}.png and are
// streamed on demand through the shared crash-safe producer; the browser
// caches them (they are immutable). With no card or no tiles the route
// 404s and the map falls back to coordinates only. Tiles are generated
// off-device (see tools/) and copied to the card; the firmware only
// serves what is there. Settings (source / dir / zoom / online URL)
// persist via Web::MapConfig.

    // Highest slippy zoom we probe for /api/map/config.
    static constexpr int MAP_MAX_ZOOM = Web::MapConfig::MAX_ZOOM;

    // GET /api/map/config - the persisted settings plus, for the SD source,
    // whether a card is present and which zoom levels have a directory. One
    // shallow dir probe per zoom, bounded by MAP_MAX_ZOOM, only on this call.
    static void handle_map_config_get(AsyncWebServerRequest* req) {
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const Web::MapConfig::Config& cfg = Web::MapConfig::config();
      Common::PsramJsonDocument doc;
      doc["mode"]         = Web::MapConfig::mode_str(cfg.mode);
      doc["format"]       = Web::MapConfig::format_str(cfg.format);
      doc["maps_dir"]     = cfg.maps_dir;
      doc["pmtiles"]      = cfg.pmtiles;
      doc["default_zoom"] = cfg.default_zoom;
      doc["online_url"]   = cfg.online_url;
      const bool card = Storage::SDCard::present();
      doc["sd_present"] = card;
      // Raster: which zoom-level dirs exist. Vector: whether the .pmtiles file
      // is present. Both probed only on this infrequent config call.
      JsonArray zooms = doc["zooms"].to<JsonArray>();
      bool raster_ok = false;
      if (card) {
        for (int z = 0; z <= MAP_MAX_ZOOM; ++z) {
          char d[80];
          snprintf(d, sizeof(d), "%s/%d", cfg.maps_dir.c_str(), z);
          if (Storage::SDCard::exists(d)) { zooms.add(z); raster_ok = true; }
        }
      }
      doc["sd_available"]      = card && raster_ok;
      const bool pm_ok = card && Storage::SDCard::exists(cfg.pmtiles.c_str());
      doc["pmtiles_available"] = pm_ok;
      // Report the basemap's own max zoom (header byte 101) so the web app
      // can overzoom past it (render the coarsest data scaled) instead of
      // going blank when zoomed in beyond the data.
      if (pm_ok) {
        File fp = Storage::SDCard::open_read(cfg.pmtiles.c_str());
        if (fp) {
          uint8_t hb[110]; size_t got;
          { Storage::SDCard::BusGuard _bg; got = fp.read(hb, sizeof(hb)); fp.close(); }
          if (got >= 102 && memcmp(hb, "PMTiles", 7) == 0) doc["pmtiles_maxzoom"] = (int)hb[101];
        }
      }
      send_json(req, 200, doc);
    }

    // POST /api/map/config - partial update of the map settings, persisted
    // to /map.json. Unknown/absent keys keep their current value.
    static void handle_map_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      if (require_auth(req).empty()) return;
      Web::MapConfig::Config& c = Web::MapConfig::config();
      if (body["mode"].is<const char*>())
        c.mode = Web::MapConfig::mode_from(body["mode"].as<const char*>(), c.mode);
      if (body["format"].is<const char*>())
        c.format = Web::MapConfig::format_from(body["format"].as<const char*>(), c.format);
      if (body["maps_dir"].is<const char*>())
        c.maps_dir = Web::MapConfig::sanitize_dir(body["maps_dir"].as<const char*>());
      if (body["pmtiles"].is<const char*>())
        c.pmtiles = Web::MapConfig::sanitize_file(body["pmtiles"].as<const char*>());
      if (body["default_zoom"].is<int>()) {
        int z = body["default_zoom"].as<int>();
        if (z < 1) z = 1;
        if (z > MAP_MAX_ZOOM) z = MAP_MAX_ZOOM;
        c.default_zoom = (uint8_t)z;
      }
      if (body["online_url"].is<const char*>()) {
        const char* u = body["online_url"].as<const char*>();
        if (u && *u) c.online_url = u;
      }
      Web::MapConfig::persist(filesystem);
      handle_map_config_get(req);   // echo the stored config back
    }

    // GET /api/map/tile/{z}/{x}/{y} - stream one PNG tile from the card.
    static void handle_map_tile(AsyncWebServerRequest* req) {
      // Auth walks the token table (shared with the gateway), so take the
      // RNS lock for just that; the SD I/O below stays out of the lock so a
      // concurrent inbound Resource transfer can't block tile serving (the
      // same scoping the attachment download uses).
      {
        RnsLockGuard _g;
        LXMF::IdentityId caller = require_auth(req);
        if (caller.empty()) return;
      }
      // z/x/y must be plain non-negative integers in slippy range. Parsing
      // them as ints (and rejecting everything else) also blocks any "../"
      // path-traversal attempt through the captured segments.
      auto parse_uint = [](const String& s, long max_excl, long* out) -> bool {
        if (s.isEmpty() || s.length() > 9) return false;
        for (size_t i = 0; i < s.length(); ++i)
          if (!isdigit((unsigned char)s[i])) return false;
        const long v = atol(s.c_str());
        if (v < 0 || v >= max_excl) return false;
        *out = v;
        return true;
      };
      long z = 0, x = 0, y = 0;
      if (!parse_uint(req->pathArg(0), MAP_MAX_ZOOM + 1, &z)) {
        send_error(req, 400, "bad_tile_coord");
        return;
      }
      const long axis = 1L << z;   // tiles per axis at this zoom
      if (!parse_uint(req->pathArg(1), axis, &x) ||
          !parse_uint(req->pathArg(2), axis, &y)) {
        send_error(req, 400, "bad_tile_coord");
        return;
      }
      if (!Storage::SDCard::present()) { send_error(req, 404, "no_sd"); return; }
      char path[96];
      snprintf(path, sizeof(path), "%s/%ld/%ld/%ld.png",
               Web::MapConfig::config().maps_dir.c_str(), z, x, y);
      if (!Storage::SDCard::exists(path)) { send_error(req, 404, "tile_not_found"); return; }
      auto fp = std::make_shared<File>(Storage::SDCard::open_read(path));
      if (!*fp) { send_error(req, 500, "tile_open_failed"); return; }
      const size_t total = (size_t)fp->size();
      AsyncWebServerResponse* resp = Web::FileStream::begin(
          req, total, "image/png",
          [fp](uint8_t* dst, size_t want) -> size_t {
            Storage::SDCard::BusGuard _bg;   // HSPI shared with the IMU pump
            return (size_t)fp->read(dst, want);
          },
          [fp]() { if (*fp) fp->close(); });
      if (!resp) {
        send_error_with_message(req, 503, "out_of_memory",
          "Low memory; tile skipped. Try again in a moment.");
        return;
      }
      // Same z/x/y always returns the same image: cache hard.
      resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
      req->send(resp);
    }

    // GET /api/map/basemap.pmtiles - serve the configured vector basemap
    // (.pmtiles) from SD with HTTP range support, which protomaps-leaflet
    // relies on (it reads the header, directory and tiles via Range). The
    // path ends in .pmtiles so protomaps-leaflet treats it as an archive to
    // range-read, not a {z}/{x}/{y} tile template. The body is the requested
    // byte slice, streamed from SD off-loop with the bus guard.
    static void handle_map_pmtiles(AsyncWebServerRequest* req) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      if (!Storage::SDCard::present()) { send_error(req, 404, "no_sd"); return; }
      const std::string path = Web::MapConfig::config().pmtiles;
      if (!Storage::SDCard::exists(path.c_str())) { send_error(req, 404, "pmtiles_not_found"); return; }
      auto fp = std::make_shared<File>(Storage::SDCard::open_read(path.c_str()));
      if (!*fp) { send_error(req, 500, "pmtiles_open_failed"); return; }
      const size_t total = (size_t)fp->size();
      // A single byte range ("bytes=start-end"); protomaps-leaflet always
      // sends one. No Range -> serve the whole file (rare).
      size_t start = 0, end = (total ? total - 1 : 0);
      bool ranged = false;
      if (req->hasHeader("Range")) {
        String r = req->header("Range");
        if (r.startsWith("bytes=")) {
          const int dash = r.indexOf('-', 6);
          if (dash > 6) {
            start = (size_t)strtoul(r.substring(6, dash).c_str(), nullptr, 10);
            String es = r.substring(dash + 1); es.trim();
            if (es.length()) end = (size_t)strtoul(es.c_str(), nullptr, 10);
            ranged = true;
          }
        }
      }
      if (total == 0 || start > end || start >= total) {
        AsyncWebServerResponse* r416 = req->beginResponse(416, "text/plain", "");
        char cr[40]; snprintf(cr, sizeof(cr), "bytes */%u", (unsigned)total);
        r416->addHeader("Content-Range", cr);
        req->send(r416);
        return;
      }
      if (end >= total) end = total - 1;
      fp->seek(start);
      auto rem = std::make_shared<size_t>(end - start + 1);
      AsyncWebServerResponse* resp = Web::FileStream::begin(
          req, *rem, "application/octet-stream",
          [fp, rem](uint8_t* dst, size_t want) -> size_t {
            if (*rem == 0) return 0;
            const size_t n = want < *rem ? want : *rem;
            Storage::SDCard::BusGuard _bg;   // HSPI shared with the IMU pump
            const size_t got = (size_t)fp->read(dst, n);
            *rem -= got;
            return got;
          },
          [fp]() { if (*fp) fp->close(); });
      if (!resp) {
        send_error_with_message(req, 503, "out_of_memory", "Low memory; try again in a moment.");
        return;
      }
      resp->addHeader("Accept-Ranges", "bytes");
      resp->addHeader("Cache-Control", "no-store");   // the basemap file can be replaced
      if (ranged) {
        resp->setCode(206);
        char cr[64];
        snprintf(cr, sizeof(cr), "bytes %u-%u/%u", (unsigned)start, (unsigned)end, (unsigned)total);
        resp->addHeader("Content-Range", cr);
      }
      req->send(resp);
    }

    // GET /api/map/download - status of the current (or last) download job:
    // phase, bytes written, content length, url and dest. Cheap; the browser
    // polls it while a job runs to drive the progress bar.
    static void handle_map_download_get(AsyncWebServerRequest* req) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      Common::PsramJsonDocument doc;
      Web::MapDownload::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    // POST /api/map/download - fetch a file from a URL straight to the SD
    // card on a background task. Body: {"url": "...", "dest": "..."}. `dest`
    // defaults to the configured vector basemap path. Only http:// and
    // https:// are accepted; one job at a time (409 while one runs).
    static void handle_map_download_post(AsyncWebServerRequest* req, JsonVariant& body) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      const char* url = body["url"].is<const char*>() ? body["url"].as<const char*>() : nullptr;
      if (!url || !*url ||
          (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) {
        send_error_with_message(req, 400, "bad_url",
          "Enter a full http:// or https:// link to the map file.");
        return;
      }
      if (!Storage::SDCard::present()) {
        send_error_with_message(req, 409, "sd_absent",
          "No SD card is inserted: nowhere to save the map.");
        return;
      }
      std::string dest = Web::MapConfig::config().pmtiles;
      if (body["dest"].is<const char*>()) {
        const char* d = body["dest"].as<const char*>();
        if (d && *d) dest = Web::MapConfig::sanitize_file(d);
      }
      if (!Web::MapDownload::start(url, dest)) {
        send_error_with_message(req, 409, "download_busy",
          "A map download is already running.");
        return;
      }
      Common::PsramJsonDocument doc;
      Web::MapDownload::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    // POST /api/map/download/cancel - ask the running job to stop. Always
    // 200; the next status poll reflects the cancelled (then idle) phase.
    static void handle_map_download_cancel(AsyncWebServerRequest* req) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      Web::MapDownload::cancel();
      Common::PsramJsonDocument doc;
      Web::MapDownload::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    // GET /api/map/extract - status of the region-extract job (phase, tile
    // count, bytes). The browser polls it while a job runs.
    static void handle_map_extract_get(AsyncWebServerRequest* req) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      Common::PsramJsonDocument doc;
      Web::MapExtract::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    // POST /api/map/extract - extract a region from a remote planet pmtiles
    // straight to the SD card. Body: {url, bbox:{w,s,e,n}, maxzoom, dest?}.
    // `url` is the planet archive (the browser resolves the latest build);
    // `dest` defaults to the configured basemap path. One map job at a time.
    static void handle_map_extract_post(AsyncWebServerRequest* req, JsonVariant& body) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      // No url -> the device resolves the latest Protomaps planet. A given
      // url (advanced) must be http(s)://.
      const char* url = body["url"].is<const char*>() ? body["url"].as<const char*>() : "";
      if (url && *url &&
          strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        send_error_with_message(req, 400, "bad_url",
          "Enter a full http:// or https:// planet link, or leave it blank.");
        return;
      }
      if (!body["bbox"].is<JsonObject>()) {
        send_error_with_message(req, 400, "bad_bbox", "An area is required.");
        return;
      }
      JsonObject bb = body["bbox"].as<JsonObject>();
      double w = bb["w"] | 999.0, s = bb["s"] | 999.0, e = bb["e"] | 999.0, n = bb["n"] | 999.0;
      if (w < -180 || w > 180 || e < -180 || e > 180 || s < -90 || s > 90 || n < -90 || n > 90 ||
          e <= w || n <= s) {
        send_error_with_message(req, 400, "bad_bbox", "The map area is not valid.");
        return;
      }
      int maxz = body["maxzoom"] | 0;
      if (maxz < 1 || maxz > Web::MapConfig::MAX_ZOOM) {
        send_error_with_message(req, 400, "bad_zoom", "Choose a zoom between 1 and 15.");
        return;
      }
      if (!Storage::SDCard::present()) {
        send_error_with_message(req, 409, "sd_absent",
          "No SD card is inserted: nowhere to save the map.");
        return;
      }
      if (Web::MapDownload::st().phase == Web::MapDownload::RUNNING || Web::MapExtract::active()) {
        send_error_with_message(req, 409, "map_job_busy",
          "A map download is already running.");
        return;
      }
      std::string dest = Web::MapConfig::config().pmtiles;
      if (body["dest"].is<const char*>()) {
        const char* d = body["dest"].as<const char*>();
        if (d && *d) dest = Web::MapConfig::sanitize_file(d);
      }
      if (!Web::MapExtract::start(url, dest, w, s, e, n, (uint8_t)maxz)) {
        send_error_with_message(req, 409, "map_job_busy", "A map job is already running.");
        return;
      }
      Common::PsramJsonDocument doc;
      Web::MapExtract::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    // POST /api/map/extract/cancel - ask the running extract to stop.
    static void handle_map_extract_cancel(AsyncWebServerRequest* req) {
      {
        RnsLockGuard _g;
        if (require_auth(req).empty()) return;
      }
      Web::MapExtract::cancel();
      Common::PsramJsonDocument doc;
      Web::MapExtract::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }
