// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h -
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

    // POST /api/inbox_config - body = { "default_retention": { "kind", "value" } }.
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

    // GET /api/storage/config - current user-facing transfer caps
    // alongside the effective (clamped-to-backing-store) values the
    // SPA should bind its sliders' upper bound to.
    static void handle_storage_config_get(AsyncWebServerRequest* req) {
      // No RnsLockGuard: this reads only Storage state (caps + SD presence) and
      // the cached flash free-space, none of which touch Reticulum. This is the
      // one place the free-space block scan is allowed to run (off the rns_lock,
      // on the web task) - refresh the cache here so every rns_lock-holding
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

    // POST /api/storage/config - body = {"user_max_send_bytes":uint,
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

    // POST /api/sensors/config - body = {"sensor":"environment|
    // magnetometer|imu|gps", "enabled":bool, "interval_s":uint, plus an
    // optional feature object: "trend" (environment) {"enabled":bool,
    // "interval_s":uint} or "motion_wake" (imu) {"enabled":bool}.
    // Applies the override to the running driver and persists to
    // /lxmf/sensors.json so it survives reboot. For gps interval_s is the
    // LOCATION cadence (receiver power); for the I2C sensors a bare
    // interval_s is ignored, only the feature interval applies. The
    // clock-sync cadence lives in /api/time/sources.
    static void handle_sensors_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const char* key       = body["sensor"]      | "";
      const bool  enabled   = body["enabled"]     | true;
      const uint32_t iv_s   = (uint32_t)(body["interval_s"] | 60);
      if (!*key) {
        send_error_with_message(req, 400, "missing_sensor",
          "Body must include `sensor` (one of environment, magnetometer, imu, gps).");
        return;
      }
      // Optional time-series feature config: "trend" for environment,
      // "motion_wake" for imu. Absent leaves the feature untouched.
      Sensors::SensorConfig::FeatureUpdate feat;
      const char* feat_key = (strcmp(key, "environment") == 0) ? "trend"
                           : (strcmp(key, "imu") == 0)         ? "motion_wake"
                                                              : nullptr;
      if (feat_key && body[feat_key].is<JsonObjectConst>()) {
        JsonObjectConst f = body[feat_key].as<JsonObjectConst>();
        feat.present    = true;
        feat.enabled    = f["enabled"] | true;
        feat.interval_s = (uint32_t)(f["interval_s"] | 0);
      }
      if (iv_s > 7 * 24 * 3600UL || feat.interval_s > 7 * 24 * 3600UL) {
        send_error_with_message(req, 400, "interval_too_large",
          "Interval must be no more than 7 days.");
        return;
      }
      if (!Sensors::SensorConfig::update_one(filesystem, key, enabled, iv_s, feat)) {
        send_error_with_message(req, 400, "unknown_sensor",
          "Unknown sensor key. Expected environment, magnetometer, imu, or gps.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]     = "applied";
      doc["sensor"]     = key;
      doc["enabled"]    = enabled;
      doc["interval_s"] = iv_s;
      send_json(req, 200, doc);
    }

    // GET /api/power - the current power / idle-display preferences. Consumed
    // by the web app's Power tab; the firmware reads these live from
    // Common::PowerConfig, so a POST takes effect without a reboot.
    static void handle_power_config_get(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      const auto& c = Common::PowerConfig::current();
      Common::PsramJsonDocument doc;
      doc["blank_enabled"]      = c.blank_enabled;
      doc["blank_timeout_s"]    = c.blank_timeout_s;
      doc["wake_threshold_mg"]  = c.wake_threshold_mg;
      doc["heartbeat_enabled"]  = c.heartbeat_enabled;
      doc["gps_motion_retry_s"] = c.gps_motion_retry_s;
      send_json(req, 200, doc);
    }

    // POST /api/power - body = any subset of {blank_enabled, blank_timeout_s,
    // wake_threshold_mg, heartbeat_enabled, gps_motion_retry_s}. Absent keys
    // keep their current value; out-of-range values are clamped on save.
    static void handle_power_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PowerConfig::Config c = Common::PowerConfig::current();
      if (body["blank_enabled"].is<bool>())          c.blank_enabled      = body["blank_enabled"].as<bool>();
      if (body["blank_timeout_s"].is<uint32_t>())    c.blank_timeout_s    = body["blank_timeout_s"].as<uint32_t>();
      if (body["wake_threshold_mg"].is<uint32_t>())  c.wake_threshold_mg  = (uint16_t)body["wake_threshold_mg"].as<uint32_t>();
      if (body["heartbeat_enabled"].is<bool>())      c.heartbeat_enabled  = body["heartbeat_enabled"].as<bool>();
      if (body["gps_motion_retry_s"].is<uint32_t>()) c.gps_motion_retry_s = body["gps_motion_retry_s"].as<uint32_t>();
      Common::PowerConfig::set(filesystem, c);
      handle_power_config_get(req);
    }

    // GET /api/propagation - the device-wide propagation config plus the
    // registry of propagation nodes discovered from lxmf.propagation
    // announces (for the web app's node picker). The firmware reads the config
    // live, so a POST takes effect without a reboot.
    static void handle_propagation_get(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      const auto& c = LXMF::Propagation::current();
      Common::PsramJsonDocument doc;
      doc["enabled"]          = c.enabled;
      doc["pn_hash"]          = c.pn_hash;
      doc["sync_interval_s"]  = c.sync_interval_s;
      doc["retain_on_node"]   = c.retain_on_node;
      doc["use_when_offline"] = c.use_when_offline;
      JsonObject sy = doc["sync"].to<JsonObject>();
      sy["state"]         = LXMF::Propagation::Sync::state_name();
      sy["last_received"] = LXMF::Propagation::Sync::ctx().last_received;
      sy["last_error"]    = LXMF::Propagation::Sync::ctx().last_error;
      uint32_t lsm = LXMF::Propagation::Sync::ctx().last_sync_ms;
      sy["last_sync_age_s"] = lsm ? (int)((millis() - lsm) / 1000) : -1;
      JsonArray arr = doc["nodes"].to<JsonArray>();
      uint32_t now = millis();
      for (const auto& n : LXMF::Propagation::nodes()) {
        JsonObject o = arr.add<JsonObject>();
        o["hash"]            = n.hash.toHex();
        o["name"]            = n.name;
        o["active"]          = n.active;
        o["stamp_cost"]      = n.stamp_cost;
        o["per_transfer_kb"] = n.per_transfer_kb;
        o["per_sync"]        = n.per_sync;
        o["age_s"]           = (now - n.last_seen_ms) / 1000;
      }
      send_json(req, 200, doc);
    }

    // POST /api/propagation - body = any subset of {enabled, pn_hash,
    // sync_interval_s, retain_on_node, use_when_offline}. Absent keys keep
    // their current value; the hash is clamped to a valid length on save.
    static void handle_propagation_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      LXMF::Propagation::Config c = LXMF::Propagation::current();
      if (body["enabled"].is<bool>())             c.enabled          = body["enabled"].as<bool>();
      if (body["pn_hash"].is<const char*>())      c.pn_hash          = body["pn_hash"].as<const char*>();
      if (body["sync_interval_s"].is<uint32_t>()) c.sync_interval_s  = body["sync_interval_s"].as<uint32_t>();
      if (body["retain_on_node"].is<bool>())      c.retain_on_node   = body["retain_on_node"].as<bool>();
      if (body["use_when_offline"].is<bool>())    c.use_when_offline = body["use_when_offline"].as<bool>();
      LXMF::Propagation::set(filesystem, c);
      handle_propagation_get(req);
    }

    // POST /api/propagation/sync - manually trigger an inbound sync from the
    // configured propagation node. 409 if disabled, no node, or already syncing.
    static void handle_propagation_sync(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      bool started = LXMF::Propagation::Sync::start();
      Common::PsramJsonDocument doc;
      doc["started"] = started;
      doc["state"]   = LXMF::Propagation::Sync::state_name();
      if (!started) doc["error"] = "propagation off, no node set, or already syncing";
      send_json(req, started ? 200 : 409, doc);
    }

    // POST /api/storage/migrate_flash_to_sd - move flash-resident
    // attachments onto the SD card to free device flash.
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
      NOTICEF("Storage: flash→SD migration done - moved=%u skipped=%u failed=%u bytes=%llu",
              (unsigned)result.moved, (unsigned)result.skipped,
              (unsigned)result.failed, (unsigned long long)result.bytes);
      send_json(req, 200, doc);
    }

