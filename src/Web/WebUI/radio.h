// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h -
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_radio_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["frequency"]        = (uint32_t)lora_freq;
      doc["bandwidth"]        = (uint32_t)lora_bw;
      doc["spreading_factor"] = lora_sf;
      doc["coding_rate"]      = lora_cr;
      doc["tx_power"]         = lora_txp;
      // Current airtime caps. Settable via POST /api/radio/airtime; not
      // EEPROM-persisted yet, so they default to the firmware values
      // in Config.h after every boot. Surfaced here so the SPA can show
      // them in the same form the user uses to pick a region preset.
      doc["airtime_limit_pct"]          = (int)(st_airtime_limit * 100);
      doc["longterm_airtime_limit_pct"] = (int)(lt_airtime_limit * 100);
      doc["have_conf"]    = eeprom_have_conf();
      doc["radio_online"] = radio_online;
      doc["op_mode"]      = (op_mode == MODE_TNC) ? "tnc" : "host";
      // Soft limits - actual modem-side validation happens on the next boot.
      doc["limits"]["sf_min"]  = 5;
      doc["limits"]["sf_max"]  = 12;
      doc["limits"]["cr_min"]  = 5;
      doc["limits"]["cr_max"]  = 8;
      doc["limits"]["txp_max"] = 22;
      send_json(req, 200, doc);
    }

    // Discovery::State holds the persistent master toggle, default
    // announce interval, and default stamp cost (/reticulum/discovery.json).
    // Discovery::Identity holds the long-lived keypair that signs every
    // announce (/reticulum/network_identity.bin). These three handlers
    // are read/write surfaces for the Discovery settings tab.

    static void handle_radio_telemetry(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["period_ms"] = Telemetry::Radio::sample_period_ms();
      doc["capacity"]  = Telemetry::Radio::history_capacity();
      doc["size"]      = Telemetry::Radio::history_size();
      // Echo cw_min/cw_max alongside the history so the SPA can colour
      // the CW-band line against its valid range without a second call.
      doc["cw_max_band"] = 4;  // CSMA_CW_BANDS - matches firmware Misc
      JsonArray arr = doc["samples"].to<JsonArray>();
      Telemetry::Radio::fill_history(arr);
      send_json(req, 200, doc);
    }

    // Network (WiFi/transport) telemetry history - aggregate tx/rx byte rate
    // across the non-LoRa interfaces. Same shape as the radio history so the
    // SPA can reuse its chart for backbone traffic.
    static void handle_network_telemetry(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["period_ms"] = Telemetry::Network::sample_period_ms();
      doc["capacity"]  = Telemetry::Network::history_capacity();
      doc["size"]      = Telemetry::Network::history_size();
      JsonArray arr = doc["samples"].to<JsonArray>();
      Telemetry::Network::fill_history(arr);
      send_json(req, 200, doc);
    }

    static void handle_radio_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;
      // Accept the long field names the SPA uses. (Earlier versions used
      // short names freq_hz/bw_hz/sf/cr/txp - those are gone.)
      // airtime_limit_pct + longterm_airtime_limit_pct are optional -
      // when present they're persisted to EEPROM alongside the band
      // params and applied in-RAM immediately. Missing fields keep the
      // current (loaded-or-default) values.
      uint32_t freq = (uint32_t)(body["frequency"]        | 0);
      uint32_t bw   = (uint32_t)(body["bandwidth"]        | 0);
      int      sf   = (int)     (body["spreading_factor"] | -1);
      int      cr   = (int)     (body["coding_rate"]      | -1);
      int      txp  = (int)     (body["tx_power"]         | -1);
      // Per-field validation so the error names which field failed.
      char msg[160];
      if (freq < 100000000u || freq > 2500000000u) {
        snprintf(msg, sizeof(msg), "Frequency must be 100 MHz – 2.5 GHz (got %lu Hz). Tip: pick a region preset.", (unsigned long)freq);
        send_error_with_message(req, 400, "invalid_radio_params", msg); return;
      }
      if (bw < 7800 || bw > 500000) {
        snprintf(msg, sizeof(msg), "Bandwidth must be 7.8 kHz – 500 kHz (got %lu Hz). Common: 125000.", (unsigned long)bw);
        send_error_with_message(req, 400, "invalid_radio_params", msg); return;
      }
      if (sf < 5 || sf > 12) {
        snprintf(msg, sizeof(msg), "Spreading factor must be 5–12 (got %d). Common: 7 for speed, 11 for range.", sf);
        send_error_with_message(req, 400, "invalid_radio_params", msg); return;
      }
      if (cr < 5 || cr > 8) {
        snprintf(msg, sizeof(msg), "Coding rate must be 5–8 (4/5 through 4/8). Got %d. Common: 5.", cr);
        send_error_with_message(req, 400, "invalid_radio_params", msg); return;
      }
      if (txp < 0 || txp > 22) {
        snprintf(msg, sizeof(msg), "TX power must be 0–22 dBm (got %d). Regulatory limit varies by region.", txp);
        send_error_with_message(req, 400, "invalid_radio_params", msg); return;
      }
      // Direct EEPROM write, mirroring eeprom_conf_save() but bypassing
      // its hw_ready+radio_online guard. On a fresh device the radio is
      // offline because no saved config exists, which is the very state
      // this endpoint is meant to resolve. Address layout matches
      // ROM.h ADDR_CONF_{SF,CR,TXP,BW,FREQ,OK}.
      eeprom_update(eeprom_addr(ADDR_CONF_SF),  (uint8_t)sf);
      eeprom_update(eeprom_addr(ADDR_CONF_CR),  (uint8_t)cr);
      eeprom_update(eeprom_addr(ADDR_CONF_TXP), (uint8_t)txp);
      eeprom_update(eeprom_addr(ADDR_CONF_BW)+0,  (uint8_t)(bw >> 24));
      eeprom_update(eeprom_addr(ADDR_CONF_BW)+1,  (uint8_t)(bw >> 16));
      eeprom_update(eeprom_addr(ADDR_CONF_BW)+2,  (uint8_t)(bw >> 8));
      eeprom_update(eeprom_addr(ADDR_CONF_BW)+3,  (uint8_t)bw);
      eeprom_update(eeprom_addr(ADDR_CONF_FREQ)+0, (uint8_t)(freq >> 24));
      eeprom_update(eeprom_addr(ADDR_CONF_FREQ)+1, (uint8_t)(freq >> 16));
      eeprom_update(eeprom_addr(ADDR_CONF_FREQ)+2, (uint8_t)(freq >> 8));
      eeprom_update(eeprom_addr(ADDR_CONF_FREQ)+3, (uint8_t)freq);
      // Optional airtime fields - only touch EEPROM when caller sent them
      // (vs no-op). Stored as integer percentage (0..99) and converted to
      // fraction on load. 0xFF sentinel is reserved for "never set".
      if (body["airtime_limit_pct"].is<int>()) {
        const int v = body["airtime_limit_pct"];
        if (v < 0 || v > 99) {
          snprintf(msg, sizeof(msg),
                   "airtime_limit_pct must be 0-99 (got %d). 0 disables the cap.", v);
          send_error_with_message(req, 400, "invalid_radio_params", msg); return;
        }
        eeprom_update(eeprom_addr(ADDR_CONF_AIRTIME), (uint8_t)v);
        st_airtime_limit = (float)v / 100.0f;
      }
      if (body["longterm_airtime_limit_pct"].is<int>()) {
        const int v = body["longterm_airtime_limit_pct"];
        if (v < 0 || v > 99) {
          snprintf(msg, sizeof(msg),
                   "longterm_airtime_limit_pct must be 0-99 (got %d). 0 disables the cap.", v);
          send_error_with_message(req, 400, "invalid_radio_params", msg); return;
        }
        eeprom_update(eeprom_addr(ADDR_CONF_LT_AIRTIME), (uint8_t)v);
        lt_airtime_limit = (float)v / 100.0f;
      }
      eeprom_update(eeprom_addr(ADDR_CONF_OK), CONF_OK_BYTE);

      Common::PsramJsonDocument doc;
      doc["status"]  = "saved";
      doc["restart"] = true;
      respond_and_reboot(req, doc);
    }

    static void handle_radio_reset(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;
      eeprom_update(eeprom_addr(ADDR_CONF_OK), 0x00);
      Common::PsramJsonDocument doc;
      doc["status"]  = "cleared";
      doc["restart"] = true;
      respond_and_reboot(req, doc);
    }

    // POST /api/radio/airtime - set short / long airtime duty-cycle caps
    // at runtime. Body: { "airtime_limit_pct": <int>,
    //                     "longterm_airtime_limit_pct": <int>,
    //                     "identity_code": "abc123" (only if no bearer) }
    // Either or both limit fields can be omitted to leave that one alone.
    // Setting a field to 0 disables that limit (which is illegal in
    // regulated bands - caller's responsibility). Values are NOT persisted
    // across reboots yet; the firmware default in Config.h applies on next
    // boot. (EEPROM persistence is a future addition.)
    static void handle_radio_airtime(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!require_physical_auth(req, body)) return;

      const bool has_st = body["airtime_limit_pct"].is<int>();
      const bool has_lt = body["longterm_airtime_limit_pct"].is<int>();
      if (!has_st && !has_lt) {
        send_error_with_message(req, 400, "no_fields",
          "Body must include airtime_limit_pct and/or longterm_airtime_limit_pct (integer 0-99).");
        return;
      }
      if (has_st) {
        const int v = body["airtime_limit_pct"];
        if (v < 0 || v > 99) {
          char msg[120];
          snprintf(msg, sizeof(msg),
                   "airtime_limit_pct must be 0-99 (got %d). 0 disables the cap.", v);
          send_error_with_message(req, 400, "invalid_limit", msg); return;
        }
        st_airtime_limit = (v == 0) ? 0.0f : ((float)v / 100.0f);
      }
      if (has_lt) {
        const int v = body["longterm_airtime_limit_pct"];
        if (v < 0 || v > 99) {
          char msg[120];
          snprintf(msg, sizeof(msg),
                   "longterm_airtime_limit_pct must be 0-99 (got %d). 0 disables the cap.", v);
          send_error_with_message(req, 400, "invalid_limit", msg); return;
        }
        lt_airtime_limit = (v == 0) ? 0.0f : ((float)v / 100.0f);
      }
      // Recompute the lock state right away so the response reflects the
      // new configuration. Without this the API would lie about what's
      // active until the next 1-second airtime update tick.
      airtime_lock = false;
      if (st_airtime_limit != 0.0f && airtime          >= st_airtime_limit) airtime_lock = true;
      if (lt_airtime_limit != 0.0f && longterm_airtime >= lt_airtime_limit) airtime_lock = true;

      Common::PsramJsonDocument doc;
      doc["airtime_limit_pct"]          = (int)(st_airtime_limit * 100);
      doc["longterm_airtime_limit_pct"] = (int)(lt_airtime_limit * 100);
      doc["airtime_locked"]             = airtime_lock;
      doc["persisted"]                  = false;   // RAM-only for now
      send_json(req, 200, doc);
      NOTICEF("Airtime caps set via API: st=%d%% lt=%d%% lock=%d",
              (int)(st_airtime_limit*100), (int)(lt_airtime_limit*100), (int)airtime_lock);
    }

