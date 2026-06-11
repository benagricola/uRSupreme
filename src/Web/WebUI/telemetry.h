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

      const std::string collector =
          (const char*)(body["collector"] | c.collector_hex.c_str());
      if (!collector.empty()) {
        if (collector.size() != 32 ||
            collector.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
          send_error_with_message(req, 400, "bad_collector",
            "Collector address must be 32 hex characters.");
          return;
        }
      }
      uint32_t interval_s = body["interval_s"] | c.interval_s;
      if (interval_s < LXMF::TelemetrySender::MIN_INTERVAL_S) {
        interval_s = LXMF::TelemetrySender::MIN_INTERVAL_S;
      }
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
      if (enabled && collector.empty()) {
        send_error_with_message(req, 400, "missing_collector",
          "Set a collector address before enabling telemetry.");
        return;
      }

      c.enabled             = enabled;
      c.identity            = identity;
      c.collector_hex       = collector;
      c.interval_s          = interval_s;
      c.include.battery     = body["battery"]     | c.include.battery;
      c.include.location    = body["location"]    | c.include.location;
      c.include.environment = body["environment"] | c.include.environment;
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
