// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_transport_toggle(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      if (!body["enabled"].is<bool>()) {
        send_error_with_message(req, 400, "missing_enabled",
            "Request must include {enabled: true|false}.");
        return;
      }
      bool enabled = body["enabled"].as<bool>();
      RNS::Reticulum::transport_enabled(enabled);
      // Persist for next boot. Tiny JSON file alongside the rest of
      // the gateway's state in /lxmf.
      Common::PsramJsonDocument persist;
      persist["enabled"] = enabled;
      String s; serializeJson(persist, s);
      filesystem.writeFile("/lxmf/transport.json",
                           (const uint8_t*)s.c_str(), s.length());
      NOTICEF("WebUI: transport_enabled set to %s by %s",
              enabled ? "true" : "false", caller.c_str());
      Common::PsramJsonDocument doc;
      doc["enabled"] = enabled;
      send_json(req, 200, doc);
    }

    // POST /api/system/kiss { enabled: bool }
    // Flips the KISS-framed-serial-output toggle in RAM and persists
    // it to the radio-config EEPROM byte ADDR_CONF_KISS_OUT (0x00=off,
    // 0x01=on). No reboot — takes effect on the next call to
    // serial_write(). Auth: bearer token only; the user is in front
    // of the serial monitor when they want to flip this.
    static void handle_kiss_toggle(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      if (!body["enabled"].is<bool>()) {
        send_error_with_message(req, 400, "missing_enabled",
            "Request must include {enabled: true|false}.");
        return;
      }
      bool enabled = body["enabled"].as<bool>();
      kiss_serial_output = enabled;
      eeprom_update(eeprom_addr(ADDR_CONF_KISS_OUT), enabled ? 0x01 : 0x00);
      NOTICEF("WebUI: kiss_serial_output set to %s",
              enabled ? "true" : "false");
      Common::PsramJsonDocument doc;
      doc["enabled"] = enabled;
      send_json(req, 200, doc);
    }

    // POST /api/system/reboot { } — clean, no-side-effect reboot trigger.
    // Bearer-auth gated like the other /api/system/* routes. Optional
    // {delay_ms: N} in the body lets the caller widen/shorten the window
    // between response and reset; default 2 s is enough for the client
    // to read the response without hanging the dev terminal.
    static void handle_reboot(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      uint32_t delay_ms = 2000;
      if (body["delay_ms"].is<uint32_t>()) {
        delay_ms = body["delay_ms"].as<uint32_t>();
        if (delay_ms < 100)   delay_ms = 100;
        if (delay_ms > 30000) delay_ms = 30000;
      }
      NOTICEF("WebUI: reboot requested via API (delay=%ums)", (unsigned)delay_ms);
      Common::PsramJsonDocument doc;
      doc["status"] = "rebooting";
      respond_and_reboot(req, doc, delay_ms);
    }

    // GET /api/time — returns the current calibrated time, the source
    // that set it, and the source-priority/enable config. Open to any
    // authenticated session (the time itself is also exposed via
    // /api/info → clock.now_ms, so this just adds source detail).
    static void fill_system_block(JsonObject root) {
      // ---- storage ----
      {
        JsonObject st = root["storage"].to<JsonObject>();
        const size_t total = filesystem.storageSize();
        const size_t avail = Storage::flash_free();
        JsonObject fl = st["flash"].to<JsonObject>();
        fl["total_bytes"] = (uint32_t)total;
        fl["free_bytes"]  = (uint32_t)avail;
        fl["used_bytes"]  = (uint32_t)((total > avail) ? (total - avail) : 0);
        JsonObject sd = st["sd"].to<JsonObject>();
        sd["present"] = Storage::SDCard::present();
        sd["status"]  = Storage::SDCard::last_status();
        if (Storage::SDCard::present()) {
          sd["card_type"]   = Storage::SDCard::card_type_name();
          sd["total_bytes"] = (uint64_t)Storage::SDCard::total_bytes();
          sd["used_bytes"]  = (uint64_t)Storage::SDCard::used_bytes();
        }
        const auto rs = Storage::SDCard::rail_state();
        if (rs.captured) {
          JsonObject rails = sd["rails"].to<JsonObject>();
          rails["bldo1_on"] = rs.bldo1_on;
          rails["bldo1_mV"] = rs.bldo1_mV;
          rails["bldo2_on"] = rs.bldo2_on;
          rails["bldo2_mV"] = rs.bldo2_mV;
        }
      }
      // ---- rtc (hardware chip diagnostic) ----
      {
        JsonObject rtc = root["rtc"].to<JsonObject>();
        const auto rs = Sensors::PCF8563::debug_snapshot();
        rtc["available"] = Sensors::PCF8563::available();
        rtc["vl_set"]    = rs.vl_set;
        rtc["unix_ms"]   = (uint64_t)(rs.epoch * 1000.0);
      }
      // ---- sensors ----
      JsonObject sensors = root["sensors"].to<JsonObject>();
      fill_sensor_block(sensors, "gps");
      fill_sensor_block(sensors, "environment");
      fill_sensor_block(sensors, "magnetometer");
      fill_sensor_block(sensors, "imu");
      // ---- outbound staging caps + storage config ----
      {
        const auto caps = Storage::OutboundStaging::current_caps();
        JsonObject oc = root["outbound_caps"].to<JsonObject>();
        oc["max_bytes"]        = (uint32_t)caps.max_bytes;
        oc["backend"]          = Storage::OutboundStaging::backend_name(caps.chosen_backend);
        oc["flash_free_bytes"] = (uint32_t)caps.flash_free;
        oc["psram_free_bytes"] = (uint32_t)caps.psram_free;
        oc["sd_present"]       = caps.sd_present;
        if (caps.sd_present) oc["sd_free_bytes"] = (uint32_t)caps.sd_free;
      }
      // ---- user-facing storage config ----
      // Append to the already-populated `storage` object built at line
      // 1204 — using .to<JsonObject>() here overwrites the .flash /
      // .sd sub-objects (regression that made the SPA think the SD
      // card was absent). .as<JsonObject>() returns the existing
      // object so the new fields land alongside the legacy ones.
      {
        const auto& sc = Storage::Config::current();
        JsonObject st = root["storage"].as<JsonObject>();
        st["user_max_send_bytes"]      = (uint32_t)std::min<size_t>(sc.user_max_send_bytes,    0xFFFFFFFFu);
        st["user_max_receive_bytes"]   = (uint32_t)std::min<size_t>(sc.user_max_receive_bytes, 0xFFFFFFFFu);
        st["effective_max_send_bytes"] = (uint32_t)std::min<size_t>(Storage::Config::effective_max_send(),    0xFFFFFFFFu);
        st["effective_max_recv_bytes"] = (uint32_t)std::min<size_t>(Storage::Config::effective_max_receive(), 0xFFFFFFFFu);
      }
      // ---- battery (detailed) ----
      {
        const auto b = Telemetry::Battery::current();
        if (b.pmu_present) {
          JsonObject bo = root["battery"].to<JsonObject>();
          bo["percent"]      = b.percent;
          bo["state"]        = Telemetry::Battery::state_name(b.state);
          bo["voltage_v"]    = b.voltage_v;
          bo["vbus_present"] = b.vbus_present;
          if (b.vbus_present)     bo["vbus_voltage_v"] = b.vbus_voltage_v;
          if (b.has_pmu_temp)     bo["pmu_temp_c"]     = b.pmu_temp_c;
          if (b.has_discharge_ma) bo["discharge_ma"]   = b.discharge_ma;
          if (b.has_slope) {
            bo["slope_mv_per_min"] = b.slope_mv_per_min;
            bo["slope_window_ms"]  = (uint32_t)b.slope_window_ms;
          }
        }
      }
    }

    // GET /api/inbox_config — default retention for NEW chats. The
    // server's source of truth for newly-discovered peers; existing
    // peers' overrides live in /api/identities/{id}/conversations/{peer}/config.
    static void handle_factory_reset(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      // Physical-presence required, regardless of any active session —
      // factory reset wipes ALL identities and all messages, so the
      // proof-of-possession bar applies just like identity creation.
      const char* proof = body["identity_code"] | "";
      const char* code_err = explain_identity_code_failure(proof);
      if (code_err) {
        send_error_with_message(req, 401, "identity_code_required", code_err);
        return;
      }
      NOTICE("WebUI: factory reset triggered — wiping /lxmf and rebooting");

      // Walk every active identity, deregister + remove its files.
      // Iterate by value-copy because delete_identity mutates the slot.
      std::vector<LXMF::IdentityId> ids;
      for (const auto* a : LXMF::LXMFGateway::active_identities()) {
        if (a) ids.push_back(a->id);
      }
      for (const auto& id : ids) LXMF::LXMFGateway::delete_identity(id);

      // Belt-and-braces: remove anything else in /lxmf/.
      filesystem.remove("/lxmf/auth_tokens.json");
      filesystem.remove("/lxmf/button_unlock.json");
      // We deliberately do NOT touch the Reticulum transport identity
      // (stored at /transport_identity) or path_store — those are
      // device-level state, not per-identity secrets.

      Common::PsramJsonDocument doc;
      doc["status"]  = "wiped";
      doc["restart"] = true;
      respond_and_reboot(req, doc);
    }

    // DELETE /api/identities/{id}/conversations/{peer_hex}
    // Bearer-auth gated. Removes every inbox + outbox record for this
    // identity whose peer_hash matches peer_hex. Rewrites the JSONL
    // spool. Identity itself, keys, and other peers are untouched.
