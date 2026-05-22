// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_announces(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const auto& ring = LXMF::AnnounceLog::announces();
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["announces"].to<JsonArray>();
      uint32_t now = millis();
      for (auto it = ring.rbegin(); it != ring.rend(); ++it) {
        JsonObject obj = arr.add<JsonObject>();
        obj["dest"]         = it->destination.toHex();
        obj["display_name"] = it->display_name;
        obj["age_ms"]       = (uint32_t)(now - it->received_ms);
      }
      doc["count"] = (uint32_t)ring.size();
      send_json(req, 200, doc);
    }

    // Bulk migration: walk every active identity's attachment dir on
    // flash, copy each file to SD, delete the flash copy, and flip the
    // backend field on the matching inbox/outbox records. Idempotent —
    // running it twice in a row produces all-skipped on the second pass.
    static void handle_announce(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      if (!LXMF::LXMFGateway::announce(requested)) {
        send_error(req, 404, "unknown_identity");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"] = "announced";
      send_json(req, 200, doc);
    }

    static void handle_discovery_state_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const Discovery::State::Master& s = Discovery::State::current();
      Common::PsramJsonDocument doc;
      doc["enabled"]              = s.enabled;
      doc["default_interval_min"] = s.default_interval_min;
      doc["default_stamp_cost"]   = s.default_stamp_cost;
      doc["advertised_name"]      = s.advertised_name;
      doc["min_interval_min"]     = Discovery::State::MIN_INTERVAL_MIN;
      doc["max_interval_min"]     = Discovery::State::MAX_INTERVAL_MIN;
      doc["max_advertised_name_bytes"] = (uint32_t)Discovery::State::MAX_ADVERTISED_NAME_BYTES;
      // Runtime view of the announcer's progress since boot. Lets the
      // SPA tell the user "device last advertised N seconds ago" and
      // confirm the announcer is alive without needing serial access.
      const Discovery::Announcer::Status status = Discovery::Announcer::status();
      JsonObject ann = doc["announcer"].to<JsonObject>();
      ann["total_count"]       = status.total_announce_count;
      ann["last_any_ms"]       = status.last_any_announce_ms;
      ann["uptime_ms"]         = (uint32_t)millis();
      JsonObject per_if = ann["last_per_interface_ms"].to<JsonObject>();
      for (const auto& kv : status.per_interface_last_ms) {
        per_if[kv.first] = kv.second;
      }
      send_json(req, 200, doc);
    }

    static void handle_discovery_state_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const Discovery::State::Master& cur = Discovery::State::current();
      // Identity-code gate on the only direction that puts the device
      // ON-AIR: turning enabled false→true. Disabling, interval change,
      // and stamp cost change are bearer-only — they can never broaden
      // emission.
      bool wants_enable = false;
      if (body["enabled"].is<bool>()) {
        wants_enable = body["enabled"].as<bool>() && !cur.enabled;
      }
      if (wants_enable && !require_physical_auth(req, body)) return;

      if (body["enabled"].is<bool>()) {
        Discovery::State::set_enabled(body["enabled"].as<bool>());
      }
      if (body["default_interval_min"].is<int>()) {
        Discovery::State::set_default_interval_min(
            (uint32_t)body["default_interval_min"].as<int>());
      }
      if (body["default_stamp_cost"].is<int>()) {
        Discovery::State::set_default_stamp_cost(
            (uint32_t)body["default_stamp_cost"].as<int>());
      }
      if (body["advertised_name"].is<const char*>()) {
        std::string name = (const char*)body["advertised_name"];
        if (name.size() > Discovery::State::MAX_ADVERTISED_NAME_BYTES) {
          send_error_with_message(req, 413, "name_too_long",
            "Advertisement name exceeds the configured limit.");
          return;
        }
        Discovery::State::set_advertised_name(name);
      }
      handle_discovery_state_get(req);
    }

    static constexpr const char* LORA_IFACE_NAME = "LoRaInterface";

    static void handle_lora_discoverable_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      Discovery::Config::Entry e;
      const bool have = Discovery::Config::get(LORA_IFACE_NAME, &e);
      doc["name"]         = LORA_IFACE_NAME;
      doc["discoverable"] = have && e.discoverable;
      send_json(req, 200, doc);
    }

    static void handle_lora_discoverable_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const bool want = (bool)(body["discoverable"] | false);
      Discovery::Config::Entry e;
      const bool have = Discovery::Config::get(LORA_IFACE_NAME, &e);
      const bool enabling = want && !(have && e.discoverable);
      if (enabling && !require_physical_auth(req, body)) return;
      e.type         = Discovery::Config::Type::Lora;
      e.discoverable = want;
      if (!Discovery::Config::upsert(LORA_IFACE_NAME, e)) {
        send_error_with_message(req, 500, "persist_failed",
          "Could not persist LoRa discoverable flag.");
        return;
      }
      handle_lora_discoverable_get(req);
    }

    static void handle_discovery_identity_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["ready"] = Discovery::Identity::ready();
      if (Discovery::Identity::ready()) {
        doc["hash"] = Discovery::Identity::address_hex();
      }
      send_json(req, 200, doc);
    }

