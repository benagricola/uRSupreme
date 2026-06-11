// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h -
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_time_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      const double epoch = Clock::Manager::now_epoch();
      doc["calibrated"]  = Clock::Manager::is_calibrated();
      doc["unix_ms"]     = (uint64_t)(epoch * 1000.0);
      doc["source"]      = Clock::Manager::source_name(Clock::Manager::current_source());
      doc["uptime_ms"]   = (uint32_t)millis();
      JsonObject sources = doc["sources"].to<JsonObject>();
      using Clock::Manager::Source;
      for (uint8_t i = 1; i < Clock::Manager::SOURCE_COUNT; ++i) {
        const auto src = (Source)i;
        const auto& cfg = Clock::Manager::get_config(src);
        JsonObject s = sources[Clock::Manager::source_name(src)].to<JsonObject>();
        s["enabled"]    = cfg.enabled;
        s["priority"]   = cfg.priority;
        s["interval_s"] = cfg.interval_s;
      }
      send_json(req, 200, doc);
    }

    // POST /api/time {unix_ms} - adopt a browser-supplied time. Subject
    // to the Browser source's enabled+priority config; if a higher-
    // priority source has already set the time, the report is
    // recorded but not adopted, and the response indicates that.
    static void handle_time_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      if (!body["unix_ms"].is<uint64_t>() && !body["unix_ms"].is<double>()
          && !body["unix_ms"].is<long long>()) {
        send_error_with_message(req, 400, "missing_unix_ms",
          "Request body must include unix_ms (milliseconds since 1970).");
        return;
      }
      const double unix_ms = (double)(body["unix_ms"] | 0.0);
      if (unix_ms <= 0) {
        send_error_with_message(req, 400, "invalid_unix_ms",
          "unix_ms must be a positive number of milliseconds since 1970.");
        return;
      }
      const double epoch = unix_ms / 1000.0;
      const bool adopted = Clock::Manager::report_time(
        Clock::Manager::Source::Browser, epoch);
      Common::PsramJsonDocument doc;
      doc["adopted"]    = adopted;
      doc["calibrated"] = Clock::Manager::is_calibrated();
      doc["source"]     = Clock::Manager::source_name(Clock::Manager::current_source());
      doc["unix_ms"]    = (uint64_t)(Clock::Manager::now_epoch() * 1000.0);
      if (adopted) {
        NOTICEF("WebUI: time set from browser to epoch %.3f", epoch);
      }
      send_json(req, 200, doc);
    }

    // POST /api/time/sources {sources: {gps: {enabled, priority}, …}}
    // - update which time sources are enabled and their priority
    // ordering. Persisted to EEPROM. Sources not mentioned in the body
    // keep their existing config.
    static void handle_time_sources_set(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      if (!body["sources"].is<JsonObject>()) {
        send_error_with_message(req, 400, "missing_sources",
          "Request body must include {sources: {<source>: {enabled, priority}, …}}.");
        return;
      }
      JsonObject sources = body["sources"];
      for (JsonPair kv : sources) {
        Clock::Manager::Source src = Clock::Manager::source_from_name(kv.key().c_str());
        if (src == Clock::Manager::Source::None) continue;
        if (!kv.value().is<JsonObject>()) continue;
        JsonObject o = kv.value().as<JsonObject>();
        auto cfg = Clock::Manager::get_config(src);
        if (o["enabled"].is<bool>())   cfg.enabled    = o["enabled"].as<bool>();
        if (o["priority"].is<int>())   cfg.priority   = (uint8_t)o["priority"].as<int>();
        if (o["interval_s"].is<long>()) cfg.interval_s = (uint32_t)o["interval_s"].as<long>();
        Clock::Manager::set_config(src, cfg);
      }
      Clock::Manager::persist_config(filesystem);
      NOTICE("WebUI: time-source config updated");
      handle_time_get(req);
    }

    // GET /api/rtc - diagnostic snapshot of the on-board PCF8563.
    // Live I2C read; surfaces VL flag + raw regs so we can confirm
    // the hardware is wired and persisting.
    static void handle_rtc_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      const auto s = Sensors::PCF8563::debug_snapshot();
      doc["available"] = Sensors::PCF8563::available();
      doc["present"]   = s.present;
      doc["vl_set"]    = s.vl_set;
      JsonArray regs   = doc["regs"].to<JsonArray>();
      for (int i = 0; i < 7; ++i) regs.add(s.regs[i]);
      doc["unix_ms"]   = (uint64_t)(s.epoch * 1000.0);
      send_json(req, 200, doc);
    }

    // Build a single sensor's status block into `parent[kind]`. Same
    // shape as the /api/system_status sensor section so the SPA can
    // patch entries in place from a WS `sensor_update` event or read
    // them in bulk from the REST snapshot. Returns the timestamp of
    // the most recent reading (0 if no valid reading yet) so callers
    // can dedupe.
    static uint32_t fill_sensor_block(JsonObject parent, const char* kind) {
      JsonObject o = parent[kind].to<JsonObject>();
      if (strcmp(kind, "gps") == 0) {
        const Sensors::L76K::Fix f = Sensors::L76K::last_fix();
        o["model"]        = Sensors::L76K::model_name();
        o["available"]    = Sensors::L76K::has_serial();
        // GPS is presented as a sensor in the popover (enable/interval
        // controls alongside BME280/QMC6310/IMU), but its config is
        // owned by TimeManager since it doubles as a time source. Pull
        // those fields here so the SPA's sensor-config row can render
        // without a second fetch.
        {
          const auto gcfg = Clock::Manager::get_config(Clock::Manager::Source::GPS);
          o["enabled"]     = gcfg.enabled;
          o["interval_ms"] = (uint32_t)gcfg.interval_s * 1000UL;
        }
        o["valid"]        = f.valid;
        o["latitude"]     = f.latitude_deg;
        o["longitude"]    = f.longitude_deg;
        if (f.altitude_valid) o["altitude_m"] = f.altitude_m;
        o["speed_knots"]  = f.speed_knots;
        o["heading"]      = f.heading_deg;
        o["unix_ms"]      = (uint64_t)(f.unix_epoch * 1000.0);
        // Raw device-millis snapshots, NOT "X seconds ago" deltas. The
        // SPA subtracts these from its clock anchor every render tick
        // so "Last fix 12 s ago" labels tick up live without a refetch.
        // -1 sentinel = "never received."
        o["fix_received_ms"] = f.fix_received_ms == 0 ? -1 : (long)f.fix_received_ms;
        o["last_valid_fix_ms"] = f.last_valid_fix_ms == 0 ? -1 : (long)f.last_valid_fix_ms;
        o["last_byte_ms"]    = f.last_byte_ms    == 0 ? -1 : (long)f.last_byte_ms;
        o["powered"]      = Sensors::L76K::is_powered();
        switch (Sensors::L76K::pulse_state()) {
          case Sensors::L76K::PulseState::Acquiring: o["pulse_state"] = "acquiring"; break;
          default:                              o["pulse_state"] = "idle";      break;
        }
        return f.fix_received_ms;
      }
      if (strcmp(kind, "environment") == 0) {
        const Sensors::BME280::Reading r = Sensors::BME280::last_reading();
        // Driver supplies the chip name - so swapping for a BMP280 /
        // BME680 variant in the future is a one-line driver change.
        o["model"]       = Sensors::BME280::model_name();
        o["available"]   = Sensors::BME280::present();
        o["enabled"]     = Sensors::BME280::enabled();
        o["interval_ms"] = (uint32_t)Sensors::BME280::interval_ms();
        o["valid"]       = r.valid;
        if (r.valid) {
          o["temp_c"]       = r.temp_c;
          o["humidity_pct"] = r.humidity_pct;
          o["pressure_pa"]  = r.pressure_pa;
          o["taken_ms"]     = (uint32_t)r.taken_ms;
        }
        if (Sensors::BME280::present()) o["address"] = Sensors::BME280::address();
        return r.taken_ms;
      }
      if (strcmp(kind, "magnetometer") == 0) {
        const Sensors::QMC6310::Reading r = Sensors::QMC6310::last_reading();
        o["model"]       = Sensors::QMC6310::model_name();
        o["available"]   = Sensors::QMC6310::present();
        o["enabled"]     = Sensors::QMC6310::enabled();
        o["interval_ms"] = (uint32_t)Sensors::QMC6310::interval_ms();
        o["valid"]       = r.valid;
        if (r.valid) {
          o["heading_deg"] = r.heading_deg;
          o["x_uT"]        = r.x_uT;
          o["y_uT"]        = r.y_uT;
          o["z_uT"]        = r.z_uT;
          o["taken_ms"]    = (uint32_t)r.taken_ms;
        }
        if (Sensors::QMC6310::present()) o["address"] = Sensors::QMC6310::address();
        return r.taken_ms;
      }
      if (strcmp(kind, "imu") == 0) {
        const Sensors::QMI8658::Reading r = Sensors::QMI8658::last_reading();
        o["model"]       = Sensors::QMI8658::model_name();
        o["available"]   = Sensors::QMI8658::present();
        o["enabled"]     = Sensors::QMI8658::enabled();
        o["interval_ms"] = (uint32_t)Sensors::QMI8658::interval_ms();
        o["valid"]       = r.valid;
        if (r.valid) {
          o["accel_x_g"]  = r.accel_x_g;
          o["accel_y_g"]  = r.accel_y_g;
          o["accel_z_g"]  = r.accel_z_g;
          o["gyro_x_dps"] = r.gyro_x_dps;
          o["gyro_y_dps"] = r.gyro_y_dps;
          o["gyro_z_dps"] = r.gyro_z_dps;
          o["temp_c"]     = r.temp_c;
          o["taken_ms"]   = (uint32_t)r.taken_ms;
        }
        return r.taken_ms;
      }
      return 0;
    }

    // Build the system-status payload (storage / rtc / sensors /
    // outbound_caps / battery) into `root`. Single source of truth
    // shared between the WS `hello` frame and the periodic
    // `system_update` event. /api/system_status used to call this too
    // but the REST endpoint is retired - WS delivery is canonical.
    static void handle_gps_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      const Sensors::L76K::Fix f = Sensors::L76K::last_fix();
      doc["available"]   = Sensors::L76K::has_serial();
      doc["valid"]       = f.valid;
      doc["latitude"]    = f.latitude_deg;
      doc["longitude"]   = f.longitude_deg;
      if (f.altitude_valid) doc["altitude_m"] = f.altitude_m;
      if (f.hdop_valid)     doc["hdop"]       = f.hdop;
      doc["speed_knots"] = f.speed_knots;
      doc["heading"]     = f.heading_deg;
      doc["unix_ms"]     = (uint64_t)(f.unix_epoch * 1000.0);
      doc["fix_age_ms"]  = f.fix_received_ms == 0 ? -1
                            : (long)(millis() - f.fix_received_ms);
      doc["last_byte_ms"] = f.last_byte_ms == 0 ? -1
                            : (long)(millis() - f.last_byte_ms);
      send_json(req, 200, doc);
    }

