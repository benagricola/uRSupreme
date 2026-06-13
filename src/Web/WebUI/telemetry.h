// Telemetry-to-collector config + manual trigger.
//
// GET  /api/telemetry/config  - current config + last-send status.
// POST /api/telemetry/config  - partial update, persists to
//                               /lxmf/telemetry.json.
// POST /api/telemetry/send    - pack + send one update now.
//
// State lives in LXMF::TelemetrySender; this file only translates
// HTTP to it.

    static void handle_telemetry_config_get(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      LXMF::TelemetrySender::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    static void handle_telemetry_config_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      auto& c = LXMF::TelemetrySender::config();

      // Collectors: accept a "collectors" array, or migrate a legacy
      // single "collector" string. Absent means keep the current list.
      std::vector<std::string> collectors = c.collectors;
      bool collectors_given = false;
      if (body["collectors"].is<JsonArray>()) {
        collectors_given = true;
        collectors.clear();
        for (JsonVariant v : body["collectors"].as<JsonArray>()) {
          const char* s = v.as<const char*>();
          if (s && *s) collectors.emplace_back(s);
        }
      } else if (body["collector"].is<const char*>()) {
        collectors_given = true;
        collectors.clear();
        const char* s = body["collector"].as<const char*>();
        if (s && *s) collectors.emplace_back(s);
      }
      if (collectors_given) {
        for (const std::string& h : collectors) {
          if (h.size() != 32 ||
              h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
            send_error_with_message(req, 400, "bad_collector",
              "Collector address must be 32 hex characters.");
            return;
          }
        }
      }
      // Diag mode sends a tiny single packet, so it gets a lower
      // interval floor for usable field-debug resolution.
      const bool diag = body["diag"] | c.diag;
      const uint32_t floor = diag ? LXMF::TelemetrySender::DIAG_MIN_INTERVAL_S
                                  : LXMF::TelemetrySender::MIN_INTERVAL_S;
      uint32_t interval_s = body["interval_s"] | c.interval_s;
      if (interval_s < floor) interval_s = floor;
      if (interval_s > LXMF::TelemetrySender::MAX_INTERVAL_S) {
        send_error_with_message(req, 400, "interval_too_large",
          "Interval must be no more than 7 days.");
        return;
      }
      const std::string identity = (const char*)(body["identity"] | c.identity.c_str());
      if (!identity.empty() && !LXMF::LXMFGateway::identity_by_id(identity)) {
        send_error_with_message(req, 400, "unknown_identity",
          "No such identity on this device.");
        return;
      }
      const bool enabled = body["enabled"] | c.enabled;
      if (enabled && collectors.empty()) {
        send_error_with_message(req, 400, "missing_collector",
          "Add a collector address before enabling telemetry.");
        return;
      }

      c.enabled             = enabled;
      c.identity            = identity;
      c.collectors          = collectors;
      c.interval_s          = interval_s;
      c.include.battery     = body["battery"]     | c.include.battery;
      c.include.location    = body["location"]    | c.include.location;
      c.include.environment = body["environment"] | c.include.environment;
      c.include.magnetic    = body["compass"]     | c.include.magnetic;
      c.diag                = diag;
      LXMF::TelemetrySender::persist(filesystem);

      Common::PsramJsonDocument doc;
      LXMF::TelemetrySender::fill_status(doc.to<JsonObject>());
      send_json(req, 200, doc);
    }

    static void handle_telemetry_send(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      (void)body;
      const bool ok = LXMF::TelemetrySender::send_now();
      Common::PsramJsonDocument doc;
      LXMF::TelemetrySender::fill_status(doc.to<JsonObject>());
      doc["accepted"] = ok;
      send_json(req, ok ? 200 : 409, doc);
    }

    // GET /api/telemetry/shares - the session identity's active live
    // shares, both directions. `grants` = peers allowed to request our
    // readings (we are the sender); `feeds` = peers we poll for theirs
    // (we are the receiver), with the latest decoded readings.
    static void handle_telemetry_shares_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const double now_epoch = LXMF::TelemetryShare::share_now_epoch(caller);
      Common::PsramJsonDocument doc;
      JsonArray grants = doc["grants"].to<JsonArray>();
      for (const auto& g : LXMF::TelemetryShare::grants()) {
        if (g.iden != caller || now_epoch > g.expires_epoch) continue;
        JsonObject o = grants.add<JsonObject>();
        o["peer"]    = g.peer.toHex();
        o["items"]   = g.items;
        o["left_s"]  = (uint32_t)(g.expires_epoch - now_epoch);
      }
      JsonArray feeds = doc["feeds"].to<JsonArray>();
      for (const auto& f : LXMF::TelemetryShare::feeds()) {
        if (f.iden != caller) continue;
        JsonObject o = feeds.add<JsonObject>();
        o["peer"]    = f.peer.toHex();
        o["every_s"] = f.every_s;
        o["left_s"]  = now_epoch > f.expires_epoch
                       ? 0 : (uint32_t)(f.expires_epoch - now_epoch);
        if (f.latest.size() > 0) {
          o["age_ms"] = (uint32_t)(millis() - f.latest_ms);
          Telemetry::Telemeter::decode_into(o["tele"].to<JsonObject>(),
                                            f.latest.data(), f.latest.size());
        }
      }
      send_json(req, 200, doc);
    }

    // POST /api/telemetry/shares/stop {peer, role:"grant"|"feed"}.
    // grant: stop answering this peer (they keep polling until their
    // window ends; unanswered requests make them give up). feed: stop
    // requesting from this peer (their grant just idles out).
    static void handle_telemetry_shares_stop(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string peer_hex = (const char*)(body["peer"] | "");
      RNS::Bytes peer = hex_to_bytes(peer_hex, LXMF::HASH_LEN);
      if (peer.size() != LXMF::HASH_LEN) {
        send_error_with_message(req, 400, "invalid_peer_hash",
                                "Peer address must be 32 hex characters.");
        return;
      }
      const std::string role = (const char*)(body["role"] | "");
      if (role == "grant")     LXMF::TelemetryShare::remove_grant(caller, peer);
      else if (role == "feed") LXMF::TelemetryShare::remove_feed(caller, peer);
      else {
        send_error_with_message(req, 400, "invalid_role",
                                "role must be grant or feed.");
        return;
      }
      handle_telemetry_shares_get(req);
    }
