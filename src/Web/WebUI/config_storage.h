// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_inbox_config_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const auto& cfg = LXMF::InboxConfig::current();
      Common::PsramJsonDocument doc;
      JsonObject r = doc["default_retention"].to<JsonObject>();
      r["kind"]  = LXMF::retention_kind_name(cfg.default_retention.kind);
      r["value"] = cfg.default_retention.value;
      send_json(req, 200, doc);
    }

    // POST /api/inbox_config — body = { "default_retention": { "kind", "value" } }.
    // kind: "none" | "time" | "count". value: seconds (time), messages (count),
    // ignored (none). 10-year TTL ceiling for sanity; count is capped to the
    // hard ring size.
    static void handle_inbox_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      JsonObjectConst r = body["default_retention"].as<JsonObjectConst>();
      if (r.isNull()) {
        send_error_with_message(req, 400, "missing_default_retention",
          "Body must include default_retention: { kind, value }.");
        return;
      }
      LXMF::Retention next;
      next.kind  = LXMF::retention_kind_from_str(r["kind"] | "none");
      next.value = (uint32_t)(r["value"] | 0);
      if (next.kind == LXMF::Retention::Kind::Time
          && next.value > 10UL * 365UL * 86400UL) {
        send_error_with_message(req, 400, "ttl_too_large",
          "Retention time must be no more than 10 years.");
        return;
      }
      if (next.kind == LXMF::Retention::Kind::Count
          && next.value > LXMF::LXMFInbox::DEFAULT_RAM_CAPACITY) {
        send_error_with_message(req, 400, "count_too_large",
          "Per-chat message count cannot exceed the per-identity ring capacity.");
        return;
      }
      LXMF::InboxConfig::set_default_retention(filesystem, next);
      LXMF::LXMFGateway::apply_inbox_config_to_all();
      handle_inbox_config_get(req);
    }

    // GET /api/storage/config — current user-facing transfer caps
    // alongside the effective (clamped-to-backing-store) values the
    // SPA should bind its sliders' upper bound to.
    static void handle_storage_config_get(AsyncWebServerRequest* req) {
      // No RnsLockGuard: this reads only Storage state (caps + SD presence) and
      // the cached flash free-space, none of which touch Reticulum. This is the
      // one place the free-space block scan is allowed to run (off the rns_lock,
      // on the web task) — refresh the cache here so every rns_lock-holding
      // reader downstream (receive cap, /api/info storage block) gets a warm,
      // non-blocking Storage::flash_free(). The esp_littlefs semaphore makes the
      // scan FS-safe without an external lock.
      if (require_auth(req).empty()) return;
      Storage::flash_free_refresh();
      const auto& cfg = Storage::Config::current();
      Common::PsramJsonDocument doc;
      doc["user_max_send_bytes"]      = (uint32_t)std::min<size_t>(cfg.user_max_send_bytes,    0xFFFFFFFFu);
      doc["user_max_receive_bytes"]   = (uint32_t)std::min<size_t>(cfg.user_max_receive_bytes, 0xFFFFFFFFu);
      doc["effective_max_send_bytes"] = (uint32_t)std::min<size_t>(Storage::Config::effective_max_send(),    0xFFFFFFFFu);
      doc["effective_max_recv_bytes"] = (uint32_t)std::min<size_t>(Storage::Config::effective_max_receive(), 0xFFFFFFFFu);
      doc["sd_present"]               = Storage::SDCard::present();
      send_json(req, 200, doc);
    }

    // POST /api/storage/config — body = {"user_max_send_bytes":uint,
    // "user_max_receive_bytes":uint}. 0 means "restore default". The
    // saved value is the user preference; the effective value (what
    // actually gates transfers) is clamped to backing-store free
    // space + the RNS protocol ceiling at enforcement time.
    static void handle_storage_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const auto& current = Storage::Config::current();
      const uint32_t snd = body["user_max_send_bytes"]    | (uint32_t)current.user_max_send_bytes;
      const uint32_t rcv = body["user_max_receive_bytes"] | (uint32_t)current.user_max_receive_bytes;
      Storage::Config::set(filesystem, (size_t)snd, (size_t)rcv);
      handle_storage_config_get(req);
    }

    // POST /api/sensors/config — body = {"sensor":"environment|magnetometer|imu",
    // "enabled":bool, "interval_s":uint}. Applies the override to the
    // running driver and persists to /lxmf/sensors.json so it survives
    // reboot. GPS isn't routed through here — its enable/interval are
    // bound to the time-source priority list (see /api/time/sources).
    static void handle_sensors_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const char* key       = body["sensor"]      | "";
      const bool  enabled   = body["enabled"]     | true;
      const uint32_t iv_s   = (uint32_t)(body["interval_s"] | 60);
      if (!*key) {
        send_error_with_message(req, 400, "missing_sensor",
          "Body must include `sensor` (one of environment, magnetometer, imu).");
        return;
      }
      if (iv_s > 7 * 24 * 3600UL) {
        send_error_with_message(req, 400, "interval_too_large",
          "Interval must be no more than 7 days.");
        return;
      }
      if (!Sensors::SensorConfig::update_one(filesystem, key, enabled, iv_s)) {
        send_error_with_message(req, 400, "unknown_sensor",
          "Unknown sensor key. Expected bme280, magnetometer, or imu.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]     = "applied";
      doc["sensor"]     = key;
      doc["enabled"]    = enabled;
      doc["interval_s"] = iv_s;
      send_json(req, 200, doc);
    }

    // GET /api/gps — last RMC fix. Returns valid flag, position,
    // speed/heading, UTC, and how recent the fix was. Auth-gated so
    // attackers on the LAN can't passively scrape location.
    static void handle_migrate_flash_to_sd(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      if (!Storage::SDCard::present()) {
        send_error_with_message(req, 409, "sd_absent",
          "No SD card is inserted: nothing to migrate to.");
        return;
      }
      const auto result = Storage::Migration::run();
      Common::PsramJsonDocument doc;
      doc["moved"]            = (uint32_t)result.moved;
      doc["skipped"]          = (uint32_t)result.skipped;
      doc["failed"]           = (uint32_t)result.failed;
      doc["bytes"]            = (uint64_t)result.bytes;
      doc["records_updated"]  = (uint32_t)result.records_updated;
      NOTICEF("Storage: flash→SD migration done — moved=%u skipped=%u failed=%u bytes=%llu",
              (unsigned)result.moved, (unsigned)result.skipped,
              (unsigned)result.failed, (unsigned long long)result.bytes);
      send_json(req, 200, doc);
    }

