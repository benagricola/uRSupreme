// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_wifi_scan(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      // Scan results aren't sensitive; gate only the configure write.
      int n = WiFi.scanNetworks();
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["networks"].to<JsonArray>();
      for (int i = 0; i < n; i++) {
        JsonObject obj = arr.add<JsonObject>();
        obj["ssid"]    = WiFi.SSID(i);
        obj["rssi"]    = WiFi.RSSI(i);
        obj["secure"]  = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
      WiFi.scanDelete();
      send_json(req, 200, doc);
    }

    static void handle_wifi_configure(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;
      String ssid = body["ssid"] | "";
      String psk  = body["psk"]  | "";
      if (ssid.length() == 0 || ssid.length() > 32) {
        send_error(req, 400, "invalid_ssid");
        return;
      }
      if (psk.length() > 32) {
        send_error(req, 400, "invalid_psk");
        return;
      }
      // Refuse mid-flight provisioning if a previous request is still
      // parked — otherwise we'd race two concurrent /api/wifi/configure
      // calls trying to drive the same phase machine.
      if (wr_pending.req != nullptr || wr_pending.pending) {
        send_error(req, 409, "provision_in_progress");
        return;
      }
      // Write SSID, PSK, and STA mode to EEPROM. Shared with the Improv
      // serial provisioning path so both end up with identical EEPROM
      // layout — the helper lives in Remote.h.
      wifi_remote_eeprom_write_sta_creds(ssid.c_str(), psk.c_str());

      // Two cases:
      //   - AP up (wifi_mode == AP or APSTA): the SPA reached us over
      //     the AP-side socket, so we can hold the request open and
      //     respond when STA comes up. No reboot. Phase machine in
      //     Remote.h drives the transition.
      //   - STA only (wifi_mode == STA): the SPA reached us over the
      //     current STA network; if we change SSID inline the socket
      //     dies and the response never lands. Save EEPROM and
      //     reboot — boot policy lifts us back into APSTA, so the
      //     user's recovery channel is restored automatically.
      const bool ap_up = (wifi_mode == WR_WIFI_AP || wifi_mode == WR_WIFI_APSTA);
      if (ap_up) {
        strncpy(wr_pending.ssid, ssid.c_str(), 32); wr_pending.ssid[32] = 0;
        strncpy(wr_pending.psk,  psk.c_str(),  32); wr_pending.psk[32]  = 0;
        wr_pending.req           = req;
        wr_pending.requested_ms  = millis();
        wr_pending.pending       = true;
      } else {
        Common::PsramJsonDocument doc;
        doc["status"]  = "saved";
        doc["restart"] = true;
        respond_and_reboot(req, doc);
      }
    }

    // GET /api/wifi/saved — list the WiFi networks the device has
    // credentials for. Today the EEPROM layout holds only one entry
    // (ADDR_CONF_SSID/PSK), so this returns a 0- or 1-element array.
    // The endpoint exists so the SPA can render a stable "Saved
    // networks" UI; extending to multi-network storage later only
    // changes the body, not the URL.
    static void handle_wifi_saved_list(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["networks"].to<JsonArray>();
      char ssid[33] = {0};
      bool any = false;
      bool has_psk = false;
      for (uint8_t i = 0; i < 32; i++) {
        uint8_t c = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        if (c == 0xFF) c = 0x00;
        ssid[i] = (char)c;
        if (c != 0x00) any = true;
      }
      for (uint8_t i = 0; i < 32; i++) {
        uint8_t c = EEPROM.read(config_addr(ADDR_CONF_PSK + i));
        if (c == 0xFF) c = 0x00;
        if (c != 0x00) { has_psk = true; break; }
      }
      if (any) {
        JsonObject n = arr.add<JsonObject>();
        n["ssid"]    = ssid;
        n["has_psk"] = has_psk;
      }
      send_json(req, 200, doc);
    }

    // POST /api/wifi/forget — zero the saved SSID + PSK in EEPROM and
    // reboot. The device comes up with wifi unconfigured, which the
    // existing bootstrap path turns into the softAP captive-portal
    // for first-time setup. Identity-code auth required because this
    // disconnects every client.
    static void handle_wifi_forget(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;
      // Optional ssid arg lets a future multi-network UI target a
      // specific entry; for the single-entry EEPROM layout we just
      // wipe whatever's stored.
      const char* ssid_arg = body["ssid"] | "";
      char current[33] = {0};
      for (uint8_t i = 0; i < 32; i++) {
        uint8_t c = EEPROM.read(config_addr(ADDR_CONF_SSID + i));
        current[i] = (c == 0xFF) ? 0x00 : (char)c;
      }
      if (current[0] == 0x00) {
        send_error_with_message(req, 404, "no_saved_network",
          "No saved WiFi network to forget.");
        return;
      }
      if (*ssid_arg && strncmp(ssid_arg, current, 32) != 0) {
        send_error_with_message(req, 404, "ssid_not_saved",
          "Requested SSID is not in the saved-networks list.");
        return;
      }
      for (uint8_t i = 0; i < 33; i++) {
        eeprom_update(config_addr(ADDR_CONF_SSID + i), 0x00);
        eeprom_update(config_addr(ADDR_CONF_PSK  + i), 0x00);
      }
      // Drop to "no WiFi configured" so the next boot enters the
      // bootstrap softAP rather than retrying a network we just told
      // the user we forgot.
      wr_conf_save(WR_WIFI_OFF);
      NOTICEF("WiFi: forgot saved network '%s'", current);

      Common::PsramJsonDocument doc;
      doc["status"]  = "forgotten";
      doc["restart"] = true;
      respond_and_reboot(req, doc);
    }

    // POST /api/wifi/softap — switch the live WiFi stack from STA to
    // softAP without rebooting. EEPROM is untouched, so a reboot goes
    // back to whatever SSID is configured. Useful when the user has
    // lost the device on the LAN (router moved, address changed) and
    // wants to reconfigure without physically resetting. Gated by
    // bearer auth + identity_code (the existing physical-presence
    // pattern) since switching the AP drops every other client.
    static void handle_wifi_force_softap(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;
      if (wifi_mode == WR_WIFI_AP) {
        send_error_with_message(req, 409, "already_softap",
          "Device is already in softAP mode.");
        return;
      }
      // Defer the actual switch to the main loop — calling
      // wifi_remote_init() from the WebServer task races against
      // in-flight requests and any other WiFi-touching code.
      wr_force_softap_pending = true;
      Common::PsramJsonDocument doc;
      doc["status"] = "queued";
      doc["note"]   = "Switching to softAP. Reconnect to the device's bootstrap SSID.";
      send_json(req, 200, doc);
    }

