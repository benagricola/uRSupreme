// Auto-extracted style: included from inside the Web::WebUI class body
// (no include guard, not a standalone translation unit).
//
// Map tiles for the web app's location view. Tiles live on the SD card
// in the standard slippy-map layout <maps_dir>/{z}/{x}/{y}.png and are
// streamed on demand through the shared crash-safe producer; the browser
// caches them (they are immutable). With no card or no tiles the route
// 404s and the map falls back to coordinates only. Tiles are generated
// off-device (see tools/) and copied to the card; the firmware only
// serves what is there.

    // Fixed tile root on the card. The settings layer makes this
    // configurable later; for now it is the path the generator writes to.
    static constexpr const char* MAP_TILES_DIR = "/maps";
    // Highest slippy zoom we probe/serve. z19 is ~building level; beyond
    // it the tile counts and card footprint become impractical.
    static constexpr int MAP_MAX_ZOOM = 19;

    // GET /api/map/config - is an SD tile set available, where, and which
    // zoom levels exist. One shallow dir probe per zoom, bounded by
    // MAP_MAX_ZOOM and only on this infrequent call.
    static void handle_map_config(AsyncWebServerRequest* req) {
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      Common::PsramJsonDocument doc;
      const bool card = Storage::SDCard::present();
      doc["sd_present"] = card;
      doc["maps_dir"]   = MAP_TILES_DIR;
      JsonArray zooms = doc["zooms"].to<JsonArray>();
      bool any = false;
      if (card) {
        for (int z = 0; z <= MAP_MAX_ZOOM; ++z) {
          char d[48];
          snprintf(d, sizeof(d), "%s/%d", MAP_TILES_DIR, z);
          if (Storage::SDCard::exists(d)) { zooms.add(z); any = true; }
        }
      }
      doc["sd_available"] = card && any;
      send_json(req, 200, doc);
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
      char path[64];
      snprintf(path, sizeof(path), "%s/%ld/%ld/%ld.png", MAP_TILES_DIR, z, x, y);
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
