// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h -
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
    // backend field on the matching inbox/outbox records. Idempotent -
    // running it twice in a row produces all-skipped on the second pass.
    static void handle_announce(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string& requested = caller;  // session identity (bearer token); no {id} in path
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
      // and stamp cost change are bearer-only - they can never broaden
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
    static constexpr const char* UDP_IFACE_NAME  = "UDPInterface";

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

    // Full LoRa interface config: mode + IFAC (+ discoverable). The mode
    // and IFAC apply at the next boot (the interface is constructed once
    // at startup from this entry), so the SET response carries
    // reboot_required so the SPA can prompt. The IFAC netkey is a secret
    // and is never returned by GET.
    static void handle_lora_config_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      Discovery::Config::Entry e;
      const bool have = Discovery::Config::get(LORA_IFACE_NAME, &e);
      doc["name"] = LORA_IFACE_NAME;
      // Effective mode: the configured value, or the ACCESS_POINT default
      // when unset (mode_default flags which one the SPA is showing). AP is
      // the default so a LoRa bridge doesn't flood backbone announces.
      const uint8_t eff = (have && e.mode) ? e.mode
                                           : RNS::Type::Interface::MODE_ACCESS_POINT;
      doc["mode"]         = Discovery::Config::mode_to_str(eff);
      doc["mode_default"] = !(have && e.mode);
      doc["discoverable"] = have && e.discoverable;
      JsonObject ifac = doc["ifac"].to<JsonObject>();
      ifac["configured"] = have && e.ifac_size > 0;
      ifac["netname"]    = have ? e.ifac_netname : std::string();
      ifac["size_bits"]  = have ? (int)e.ifac_size * 8 : 0;
      send_json(req, 200, doc);
    }

    static void handle_lora_config_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Discovery::Config::Entry e;
      Discovery::Config::get(LORA_IFACE_NAME, &e);
      e.type = Discovery::Config::Type::Lora;
      bool reboot_needed = false;

      // mode (optional). Reject an unknown non-empty string.
      if (body["mode"].is<const char*>()) {
        const uint8_t m = Discovery::Config::mode_from_str(body["mode"] | "");
        if (m == 0) {
          send_error_with_message(req, 400, "invalid_mode",
            "mode must be one of: full, gateway, access_point, pointtopoint, roaming, boundary.");
          return;
        }
        if (m != e.mode) { e.mode = m; reboot_needed = true; }
      }

      // IFAC (optional). ifac_size<=0 or empty netname clears it (open
      // interface). Setting a key is security-relevant -> physical auth.
      if (body["ifac_size"].is<int>() || body["ifac_netname"].is<const char*>()) {
        const int bits = body["ifac_size"] | 0;
        const std::string netname = (const char*)(body["ifac_netname"] | "");
        const std::string netkey  = (const char*)(body["ifac_netkey"]  | "");
        if (bits <= 0 || netname.empty()) {
          if (e.ifac_size != 0) reboot_needed = true;
          e.ifac_size = 0; e.ifac_netname.clear(); e.ifac_netkey.clear();
        } else {
          if (!require_physical_auth(req, body)) return;
          // GET never returns the netkey, so an edit that doesn't re-supply
          // it keeps the existing one. Require a key when enabling fresh.
          if (netkey.empty() && e.ifac_netkey.empty()) {
            send_error_with_message(req, 400, "ifac_netkey_required",
              "Provide ifac_netkey when enabling IFAC on this interface.");
            return;
          }
          e.ifac_netname = netname;
          if (!netkey.empty()) e.ifac_netkey = netkey;
          e.ifac_size = (uint16_t)(bits / 8);
          reboot_needed = true;
        }
      }

      // discoverable (optional). Enabling requires physical presence, same
      // rule as handle_lora_discoverable_set.
      if (body["discoverable"].is<bool>()) {
        const bool want = body["discoverable"].as<bool>();
        const bool enabling = want && !e.discoverable;
        if (enabling && !require_physical_auth(req, body)) return;
        e.discoverable = want;
      }

      if (!Discovery::Config::upsert(LORA_IFACE_NAME, e)) {
        send_error_with_message(req, 500, "persist_failed",
          "Could not persist LoRa interface config.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]          = "updated";
      doc["reboot_required"] = reboot_needed;
      send_json(req, 200, doc);
    }

    // Full UDP interface config: mode + IFAC (+ discoverable). Mirrors the
    // LoRa pair above so the UDP segment can be made private (IFAC) without
    // hand-editing /reticulum/interfaces.json. Applies on the next boot.
    // The netkey is write-only (GET never returns it).
    static void handle_udp_config_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      Discovery::Config::Entry e;
      const bool have = Discovery::Config::get(UDP_IFACE_NAME, &e);
      doc["name"] = UDP_IFACE_NAME;
      // UDP registers with the FULL default (see RNode_Firmware.ino) - a
      // DISCOVER_PATHS_FOR mode here would rebroadcast path requests onto LoRa.
      const uint8_t eff = (have && e.mode) ? e.mode
                                           : RNS::Type::Interface::MODE_FULL;
      doc["mode"]         = Discovery::Config::mode_to_str(eff);
      doc["mode_default"] = !(have && e.mode);
      doc["discoverable"] = have && e.discoverable;
      JsonObject ifac = doc["ifac"].to<JsonObject>();
      ifac["configured"] = have && e.ifac_size > 0;
      ifac["netname"]    = have ? e.ifac_netname : std::string();
      ifac["size_bits"]  = have ? (int)e.ifac_size * 8 : 0;
      send_json(req, 200, doc);
    }

    static void handle_udp_config_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Discovery::Config::Entry e;
      Discovery::Config::get(UDP_IFACE_NAME, &e);
      e.type = Discovery::Config::Type::Udp;
      bool reboot_needed = false;

      // mode (optional). Reject an unknown non-empty string.
      if (body["mode"].is<const char*>()) {
        const uint8_t m = Discovery::Config::mode_from_str(body["mode"] | "");
        if (m == 0) {
          send_error_with_message(req, 400, "invalid_mode",
            "mode must be one of: full, gateway, access_point, pointtopoint, roaming, boundary.");
          return;
        }
        if (m != e.mode) { e.mode = m; reboot_needed = true; }
      }

      // IFAC (optional). ifac_size<=0 or empty netname clears it (open
      // interface). Setting a key is security-relevant -> physical auth.
      if (body["ifac_size"].is<int>() || body["ifac_netname"].is<const char*>()) {
        const int bits = body["ifac_size"] | 0;
        const std::string netname = (const char*)(body["ifac_netname"] | "");
        const std::string netkey  = (const char*)(body["ifac_netkey"]  | "");
        if (bits <= 0 || netname.empty()) {
          if (e.ifac_size != 0) reboot_needed = true;
          e.ifac_size = 0; e.ifac_netname.clear(); e.ifac_netkey.clear();
        } else {
          if (!require_physical_auth(req, body)) return;
          if (netkey.empty() && e.ifac_netkey.empty()) {
            send_error_with_message(req, 400, "ifac_netkey_required",
              "Provide ifac_netkey when enabling IFAC on this interface.");
            return;
          }
          e.ifac_netname = netname;
          if (!netkey.empty()) e.ifac_netkey = netkey;
          e.ifac_size = (uint16_t)(bits / 8);
          reboot_needed = true;
        }
      }

      // discoverable (optional). Enabling requires physical presence.
      if (body["discoverable"].is<bool>()) {
        const bool want = body["discoverable"].as<bool>();
        const bool enabling = want && !e.discoverable;
        if (enabling && !require_physical_auth(req, body)) return;
        e.discoverable = want;
      }

      if (!Discovery::Config::upsert(UDP_IFACE_NAME, e)) {
        send_error_with_message(req, 500, "persist_failed",
          "Could not persist UDP interface config.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]          = "updated";
      doc["reboot_required"] = reboot_needed;
      send_json(req, 200, doc);
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

