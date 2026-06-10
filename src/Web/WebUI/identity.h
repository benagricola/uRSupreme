// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_info(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      Common::PsramJsonDocument doc;
      doc["fw_version"] = FW_VERSION_STRING;
      doc["uptime_ms"]  = (uint32_t)millis();
      doc["bootstrap"]  = bootstrap_mode;
      doc["identity_code_pending"] = !id_code().hex6.empty() && !id_code().consumed
                              && millis() < id_code().expires_ms;
      // TTL for a newly-generated identity code (constant compiled into
      // the firmware). Exposed so the SPA's identity-code hint text
      // can render the actual duration instead of hard-coding a
      // number that drifts whenever IDENTITY_CODE_TTL_MS changes.
      doc["identity_code_ttl_ms"] = (uint32_t)IDENTITY_CODE_TTL_MS;
      // mDNS hostname (no .local suffix) advertised on STA. The SPA
      // uses this for the post-WiFi-save redirect so the user doesn't
      // have to hunt the device's new DHCP-assigned IP after a softAP
      // → STA transition. Always emitted (even in softAP) since the
      // softAP's bootstrap UI is exactly when the SPA needs to know
      // where the device will be after reboot.
      doc["mdns_hostname"] = wr_hostname;
      // WiFi mode + transitional state for the SPA's redirect logic:
      //   "off"        → WiFi off entirely
      //   "ap"         → softAP (bootstrap or runtime force)
      //   "sta"        → connected to a configured network, AP not up
      //   "sta_wait"   → STA configured but not yet associated, AP not up
      //   "apsta"      → boot grace or post-provision grace: AP up AND STA up
      //   "apsta_wait" → boot or post-provision: AP up, STA still negotiating
      const bool sta_up = (wr_wifi_status == WL_CONNECTED);
      doc["wifi_mode"] = (wifi_mode == WR_WIFI_OFF)   ? "off"
                       : (wifi_mode == WR_WIFI_AP)    ? "ap"
                       : (wifi_mode == WR_WIFI_APSTA) ? (sta_up ? "apsta" : "apsta_wait")
                       : (sta_up ? "sta" : "sta_wait");
      // Convenience boolean for "the softAP at 10.0.0.1 is also reachable
      // right now" — true in any APSTA phase, false elsewhere.
      doc["ap_active"] = (wifi_mode == WR_WIFI_AP || wifi_mode == WR_WIFI_APSTA);
      // Heap diagnostics deliberately do NOT live here. /api/info is the
      // unauthed bootstrap endpoint (login screen + redirect probe), so
      // exposing internal-SRAM internals would be an unauthenticated
      // information leak. They moved to the bearer-gated /api/diag/mem.
      // Last transient status message (Common::Status). Lets the SPA
      // surface the WiFi countdown / lifecycle in a topbar tag without
      // its own polling endpoint. Omitted entirely when the ring is
      // empty or the latest entry has expired.
      {
        char status_buf[Common::Status::MAX_MESSAGE_LEN];
        if (Common::Status::latest(status_buf, sizeof(status_buf))) {
          doc["last_status"] = status_buf;
        }
      }
      // Time state lives on the WS `hello` frame now — the login
      // screen doesn't show it, and post-login the SPA only consumes
      // the WS-pinned clock anchor. Removed from /api/info.
      JsonArray accts = doc["identities"].to<JsonArray>();
      for (const auto* a : LXMF::LXMFGateway::active_identities()) {
        JsonObject obj = accts.add<JsonObject>();
        obj["id"]           = a->id;
        obj["display_name"] = a->display_name;
        obj["address"]      = a->address_hex();
      }
      // Radio + transport status. Surfaced here so the SPA can render a
      // single status indicator without an extra round-trip on every
      // page load.
      JsonObject radio = doc["radio"].to<JsonObject>();
      radio["online"]           = radio_online;
      // Compile-time radio-chip identity. The MODEM macro is set per
      // PIO env (SX1262 for ttgo-t-beam-supreme, LR11XX for the LR1121
      // variant, etc.) — surfacing it here lets the SPA show "this is
      // the LR1121 device" so the user can tell two co-located T-Beams
      // apart at a glance without checking IPs.
      #if MODEM == SX1262
        radio["model"] = "SX1262";
      #elif MODEM == SX1276
        radio["model"] = "SX1276";
      #elif MODEM == SX1278
        radio["model"] = "SX1278";
      #elif MODEM == SX1280
        radio["model"] = "SX1280";
      #elif MODEM == LR11XX
        radio["model"] = "LR1121";
      #else
        radio["model"] = "(unknown)";
      #endif
      radio["have_conf"]        = eeprom_have_conf();
      radio["frequency"]        = (uint32_t)lora_freq;
      radio["bandwidth"]        = (uint32_t)lora_bw;
      radio["spreading_factor"] = lora_sf;
      radio["coding_rate"]      = lora_cr;
      radio["tx_power"]         = lora_txp;
      // Surface radio-init failure cause when it happened. Most common
      // cause is a wrong-variant flash — see startRadio() in
      // RNode_Firmware.ino. The SPA can show a banner / settings panel
      // warning when error_reason is present.
      if (radio_error_reason) {
        radio["error_reason"]   = radio_error_reason;
      }
      if (radio_error_expected_chip) {
        radio["expected_chip"]  = radio_error_expected_chip;
      }
      // Activity counters so callers can verify the radio is actually
      // doing work (not just "online"). stat_rx / stat_tx are packet
      // counts since boot — poll twice with a delta to see if the
      // radio is actively transmitting/receiving. airtime is the
      // short-term channel utilisation fraction (0..1).
      JsonObject stats = radio["stats"].to<JsonObject>();
      // Reticulum stack counts — these get incremented on every packet
      // through Transport::outbound / Transport::inbound and are the most
      // accurate "is the radio doing work" signal. The legacy stat_rx /
      // stat_tx variables from the upstream RNode firmware are unused
      // (never incremented in this codebase).
      stats["rx_packets"]         = (uint32_t)RNS::Transport::packets_received();
      stats["tx_packets"]         = (uint32_t)RNS::Transport::packets_sent();
      stats["destinations_seen"]  = (uint32_t)RNS::Transport::destinations_added();
      stats["last_rssi_dbm"]      = last_rssi;
      stats["last_snr_raw"]       = last_snr_raw;
      stats["noise_floor_dbm"]    = noise_floor;
      stats["airtime_pct"]          = (int)(airtime * 100);
      stats["longterm_airtime_pct"] = (int)(longterm_airtime * 100);
      // Duty-cycle caps. 0 means "no limit" (illegal in regulated bands).
      // The default at boot is 0.01 (1%) to stay within ETSI 868 MHz limits.
      stats["airtime_limit_pct"]    = (int)(st_airtime_limit * 100);
      stats["longterm_airtime_limit_pct"] = (int)(lt_airtime_limit * 100);
      // True iff TX is currently being blocked by the airtime lock —
      // useful for "why isn't my message going out" diagnostics.
      stats["airtime_locked"]       = (bool)airtime_lock;
      // LoRa egress pressure diagnostics. queue_* is the hardware TX
      // ring; tx_hold_bytes is the flow-control overflow held in PSRAM when
      // the ring is full (1fbadee); lora_tx_dropped counts packets dropped
      // because even the hold queue was full. A handshake/data packet
      // backing up here while announces churn is the starvation signature.
      stats["tx_ring_height"]       = (uint32_t)queue_height;
      stats["tx_ring_bytes"]        = (uint32_t)queued_bytes;
      stats["tx_hold_bytes"]        = (uint32_t)tx_hold_bytes;
      stats["tx_dropped_ring_full"] = (uint32_t)lora_tx_dropped;
      // WiFi state. Lets the SPA show the current mode (STA / softAP)
      // in the connection popover, and decide whether to expose the
      // "switch to softAP" button — that button has no point when
      // already in AP mode.
      JsonObject wifi = doc["wifi"].to<JsonObject>();
      wifi["mode"]          = (wifi_mode == WR_WIFI_AP) ? "ap"
                            : (wifi_mode == WR_WIFI_STA) ? "sta" : "off";
      wifi["connected"]     = (wr_wifi_status == WL_CONNECTED);
      wifi["runtime_softap"] = wr_runtime_softap;
      wifi["ip"]            = WiFi.localIP().toString().c_str();
      if (wifi_mode == WR_WIFI_STA && WiFi.SSID().length() > 0) {
        wifi["ssid"]        = WiFi.SSID().c_str();
        // Authentication mode of the currently-connected AP. ESP-IDF
        // populates this on each STA connect; map to short tokens the
        // SPA can render directly without translating an enum int.
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
          const char* auth = "unknown";
          switch (info.authmode) {
            case WIFI_AUTH_OPEN:             auth = "open"; break;
            case WIFI_AUTH_WEP:              auth = "WEP"; break;
            case WIFI_AUTH_WPA_PSK:          auth = "WPA"; break;
            case WIFI_AUTH_WPA2_PSK:         auth = "WPA2"; break;
            case WIFI_AUTH_WPA_WPA2_PSK:     auth = "WPA/WPA2"; break;
            case WIFI_AUTH_WPA2_ENTERPRISE:  auth = "WPA2-EAP"; break;
            case WIFI_AUTH_WPA3_PSK:         auth = "WPA3"; break;
            case WIFI_AUTH_WPA2_WPA3_PSK:    auth = "WPA2/WPA3"; break;
            case WIFI_AUTH_WAPI_PSK:         auth = "WAPI"; break;
            default: break;
          }
          wifi["auth"] = auth;
          wifi["rssi_dbm"] = (int)info.rssi;
        }
      } else if (wifi_mode == WR_WIFI_AP) {
        wifi["ssid"]        = (const char*)bt_devname;
        wifi["ap_ip"]       = WiFi.softAPIP().toString().c_str();
        // The bootstrap softAP is brought up open (no PSK) in
        // wifi_remote_start_ap — surface that so the SPA can warn
        // anyone seeing the bootstrap network on their phone.
        wifi["auth"]        = "open";
      }
      // Server-enforced limits. The SPA mirrors these to its compose
      // UI so the user gets immediate feedback when they hit a cap,
      // and the firmware doesn't have to send 413s for typos. Single
      // source of truth lives in LXMFMinimal.h; this block exports
      // it to the client.
      JsonObject limits = doc["limits"].to<JsonObject>();
      limits["max_body_bytes"]            = (uint32_t)LXMF::LXMF_MAX_BODY_BYTES;
      limits["max_title_bytes"]           = (uint32_t)LXMF::LXMF_MAX_TITLE_BYTES;
      limits["max_attachments"]           = (uint32_t)LXMF::LXMF_MAX_ATTACHMENTS;
      limits["max_attachment_name_bytes"] = (uint32_t)LXMF::LXMF_MAX_ATTACHMENT_NAME;
      limits["max_attachment_mime_bytes"] = (uint32_t)LXMF::LXMF_MAX_ATTACHMENT_MIME;

      // Build-time transport capabilities. The SPA hides config UI for
      // any transport that reports false so users never see knobs that
      // can't take effect on this firmware.
      JsonObject transports = doc["transports"].to<JsonObject>();
      transports["lora"]        = true;
#if HAS_WIFI && defined(TCP_TRANSPORT)
      transports["tcp_client"]  = true;
      transports["tcp_server"]  = true;
#else
      transports["tcp_client"]  = false;
      transports["tcp_server"]  = false;
#endif
      transports["udp"]         = false;
      transports["bluetooth"]   = false;
      // /api/info is the lightweight always-polled endpoint covering
      // radio + transport + WiFi + a battery summary (percent+state
      // for the topbar icon). Storage / sensors / outbound caps and
      // the detailed battery block (voltage/slope/vbus) live on
      // /api/system_status. Each datum has exactly one home.
      emit_battery_summary(doc);

      JsonObject transport = doc["transport"].to<JsonObject>();
      transport["enabled"]      = RNS::Reticulum::transport_enabled();
      // Forwarding/link-request counters live on /api/diag/transport.
      // Serial/diagnostic toggles surfaced so the SPA can render the
      // current state on its Connectivity tab without an extra round
      // trip.
      doc["kiss_serial_output"] = kiss_serial_output;
      send_json(req, 200, doc);
    }

    static void handle_login(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      const char* acc_str = body["identity_id"] | "";
      const char* pw_str  = body["password"]   | "";
      if (!*acc_str) {
        send_error_with_message(req, 400, "missing_identity_id",
          "Identity ID is required.");
        return;
      }
      if (!*pw_str) {
        send_error_with_message(req, 400, "missing_password",
          "Password is required.");
        return;
      }
      LXMF::IdentityId iden_id = acc_str;
      if (!LXMF::LXMFGateway::identity_by_id(iden_id)) {
        send_error_with_message(req, 404, "unknown_identity",
          "No identity with that ID exists on this device.");
        return;
      }
      // Knowledge-factor only — physical-presence button gesture is
      // explicitly NOT a path to login because a stolen device could
      // otherwise be unlocked by anyone with hands on it.
      if (!LXMF::LXMFGateway::check_password(iden_id, pw_str, PasswordHash::verify)) {
        send_error_with_message(req, 401, "invalid_password",
          "Incorrect password for that identity.");
        return;
      }
      std::string token = AuthTokens::issue(iden_id);
      if (token.empty()) {
        send_error(req, 500, "token_issue_failed");
        return;
      }
      // Fire an announce now so peers learn this identity immediately
      // rather than waiting up to one auto-announce interval. Cheap
      // (just packs + queues a Packet); failure here doesn't fail the
      // login since the periodic announce loop will catch up.
      LXMF::LXMFGateway::announce(iden_id);

      Common::PsramJsonDocument doc;
      doc["token"]         = token;
      doc["identity_id"]    = iden_id;
      doc["expires_in_s"]  = AuthTokens::DEFAULT_TTL_S;
      send_json(req, 200, doc);
    }

    static void handle_logout(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      String h = req->header("Authorization");
      if (h.startsWith("Bearer ")) {
        String hex_str = h.substring(7);
        hex_str.trim();
        AuthTokens::revoke(std::string(hex_str.c_str()));
      }
      req->send(204, "text/plain", "");
    }

    static void handle_create_identity(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      const char* name      = body["display_name"]     | "";
      const char* proof_str = body["identity_code"]    | "";
      const char* pw        = body["password"]         | "";
      const char* pw_conf   = body["password_confirm"] | "";
      if (!*name) { send_error(req, 400, "missing_display_name"); return; }
      if (strlen(pw) < PasswordHash::MIN_PASSWORD_LEN) {
        send_error(req, 400, "password_too_short");
        return;
      }
      if (strcmp(pw, pw_conf) != 0) {
        send_error(req, 400, "password_mismatch");
        return;
      }
      // Physical-presence proof required for identity creation —
      // the password becomes the login factor afterwards, but creating
      // identities on a stolen / network-reachable device should not be
      // possible without someone physically present at the device.
      const char* code_err = explain_identity_code_failure(proof_str);
      if (code_err) {
        send_error_with_message(req, 401, "identity_code_required", code_err);
        return;
      }
      LXMF::IdentityId iden_id = LXMF::LXMFGateway::create_identity(
          name, pw, PasswordHash::derive, PasswordHash::new_salt);
      if (iden_id.empty()) {
        send_error(req, 500, "create_failed");
        return;
      }
      std::string token = AuthTokens::issue(iden_id);
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(iden_id);
      Common::PsramJsonDocument doc;
      doc["id"]      = iden_id;
      doc["address"] = a ? a->address_hex() : "";
      doc["token"]   = token;
      send_json(req, 200, doc);
    }

    static void handle_get_identity(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string& requested = caller;  // session identity (bearer token); no {id} in path
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a) { send_error(req, 404, "unknown_identity"); return; }
      Common::PsramJsonDocument doc;
      doc["id"]                   = a->id;
      doc["display_name"]         = a->display_name;
      doc["address"]              = a->address_hex();
      doc["announce_interval_ms"]         = a->announce_interval_ms;
      doc["persist_outbound_attachments"] = a->persist_outbound_attachments;
      doc["stamp_cost"]                   = a->stamp_cost;
      doc["enforce_stamps"]               = a->enforce_stamps;
      doc["inbox_size"]                   = a->inbox  ? (uint32_t)a->inbox->size()  : 0;
      doc["outbox_size"]                  = a->outbox ? (uint32_t)a->outbox->size() : 0;
      // Time until the next *auto* announce. 0 when auto-announce is
      // disabled or when the timer has already elapsed (next loop tick
      // will announce). Lets the SPA drive a countdown badge.
      uint32_t next_in = 0;
      if (a->announce_interval_ms > 0) {
        uint32_t elapsed = millis() - a->last_announce_ms;
        next_in = (elapsed >= a->announce_interval_ms) ? 0 : (a->announce_interval_ms - elapsed);
      }
      doc["next_announce_in_ms"]  = next_in;
      send_json(req, 200, doc);
    }

    static void handle_delete_identity(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string& requested = caller;  // session identity (bearer token); no {id} in path
      AuthTokens::revoke_for_identity(requested);
      if (!LXMF::LXMFGateway::delete_identity(requested)) {
        send_error(req, 404, "unknown_identity");
        return;
      }
      req->send(204, "text/plain", "");
    }

    // Toggle Reticulum transport mode on or off at runtime. Persists
    // the choice to /lxmf/transport.json so it survives reboots.
    // Requires an authenticated session — anyone with a token can flip
    // it for now, since we don't have per-identity admin yet.
    static void handle_identity_settings(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string& requested = caller;  // session identity (bearer token); no {id} in path
      // Per-identity settings. POST accepts any combination of fields;
      // GET-like behaviour returns the current state at the end.
      if (body["announce_interval_ms"].is<JsonVariant>()) {
        uint32_t ms = (uint32_t)(body["announce_interval_ms"] | 0);
        // 0 == disabled. Otherwise clamp to a sane minimum (10s) so a
        // typo can't melt the radio's duty-cycle budget.
        if (ms != 0 && ms < 10000) ms = 10000;
        if (!LXMF::LXMFGateway::set_announce_interval(requested, ms)) {
          send_error(req, 404, "unknown_identity");
          return;
        }
      }
      if (body["persist_outbound_attachments"].is<JsonVariant>()) {
        const bool on = (bool)body["persist_outbound_attachments"];
        if (!LXMF::LXMFGateway::set_persist_outbound_attachments(requested, on)) {
          send_error(req, 404, "unknown_identity");
          return;
        }
      }
      if (body["stamp_cost"].is<JsonVariant>()) {
        // 0 (or null) disables. Valid required costs are 1-254 — the
        // same range upstream's set_inbound_stamp_cost accepts. Reject
        // out-of-range instead of silently clamping so a typo'd "2540"
        // doesn't quietly become something else.
        const int cost = (int)(body["stamp_cost"] | 0);
        if (cost < 0 || cost > 254) {
          send_error_with_message(req, 400, "invalid_stamp_cost",
            "Stamp cost must be 0 (off) or between 1 and 254.");
          return;
        }
        if (!LXMF::LXMFGateway::set_stamp_cost(requested, (uint8_t)cost)) {
          send_error(req, 404, "unknown_identity");
          return;
        }
      }
      if (body["enforce_stamps"].is<JsonVariant>()) {
        const bool on = (bool)body["enforce_stamps"];
        if (!LXMF::LXMFGateway::set_enforce_stamps(requested, on)) {
          send_error(req, 404, "unknown_identity");
          return;
        }
      }
      if (body["display_name"].is<JsonVariant>()) {
        // Trim leading/trailing whitespace; reject empty so peers always
        // see a meaningful label. The 96-byte clamp keeps the encoded
        // announce inside the 128-byte LXMFMinimal announce buffer.
        std::string n = (const char*)(body["display_name"] | "");
        size_t s = n.find_first_not_of(" \t\r\n");
        size_t e = n.find_last_not_of(" \t\r\n");
        n = (s == std::string::npos) ? std::string() : n.substr(s, e - s + 1);
        if (n.empty()) {
          send_error_with_message(req, 400, "invalid", "display_name must not be empty");
          return;
        }
        if (n.size() > 96) n.resize(96);
        if (!LXMF::LXMFGateway::set_display_name(requested, n)) {
          send_error(req, 404, "unknown_identity");
          return;
        }
      }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      Common::PsramJsonDocument doc;
      doc["id"]                            = requested;
      doc["display_name"]                  = a ? a->display_name : std::string();
      doc["announce_interval_ms"]          = a ? a->announce_interval_ms : 0;
      doc["persist_outbound_attachments"]  = a ? a->persist_outbound_attachments : true;
      doc["stamp_cost"]                    = a ? a->stamp_cost : 0;
      doc["enforce_stamps"]                = a ? a->enforce_stamps : false;
      send_json(req, 200, doc);
    }

    // Filter predicate: true → emit, false → skip. Used by the inbox /
    // outbox endpoints to surface either "most recent N" or "all
    // records with seq > since" without copying the underlying deque.
    using MessageFilter = std::function<bool(const LXMF::MessageRecord&)>;
