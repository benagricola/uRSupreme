// OLED messenger presets.
//
// GET  /api/messenger/presets  - current list + caps.
// POST /api/messenger/presets  - replace the whole list (it is tiny;
//                                item CRUD would be more states than
//                                data). Persists into the screen
//                                identity's own directory.
//
// Both verbs require the caller to BE the screen identity: the
// presets are that identity's private data (see Messenger.h), so no
// other account on the device may read or edit them. A preset may
// have an empty destination - that is a template awaiting a
// recipient; it shows in the editor but not on the device.
//
// State lives in LXMF::Messenger; this file only translates HTTP.

    static bool messenger_presets_allowed(AsyncWebServerRequest* req) {
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return false;   // 401 already sent
      const LXMF::LXMFIdentity* scr = LXMF::LXMFGateway::screen_identity();
      if (scr == nullptr) {
        send_error_with_message(req, 409, "no_screen_identity",
          "Turn on the device screen for an identity first.");
        return false;
      }
      if (scr->id != caller) {
        send_error_with_message(req, 403, "screen_identity_only",
          "Only the screen identity can manage device messages.");
        return false;
      }
      return true;
    }

    static void fill_messenger_presets(Common::PsramJsonDocument& doc) {
      doc["max_presets"]     = (uint32_t)LXMF::Messenger::MAX_PRESETS;
      doc["max_label_len"]   = (uint32_t)LXMF::Messenger::MAX_LABEL_LEN;
      doc["max_content_len"] = (uint32_t)LXMF::Messenger::MAX_CONTENT_LEN;
      JsonArray arr = doc["presets"].to<JsonArray>();
      for (const auto& p : LXMF::Messenger::presets_snapshot()) {
        JsonObject o = arr.add<JsonObject>();
        o["label"]   = p.label;
        o["dest"]    = p.dest_hex;
        o["content"] = p.content;
        JsonObject t = o["telemetry"].to<JsonObject>();
        t["location"]    = p.tel_location;
        t["environment"] = p.tel_environment;
        t["battery"]     = p.tel_battery;
        t["compass"]     = p.tel_compass;
        t["share_s"]     = p.tel_share_s;
        t["rate_s"]      = p.tel_rate_s;
      }
    }

    static void handle_messenger_presets_get(AsyncWebServerRequest* req) {
      if (!messenger_presets_allowed(req)) return;
      Common::PsramJsonDocument doc;
      fill_messenger_presets(doc);
      send_json(req, 200, doc);
    }

    static void handle_messenger_presets_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (!messenger_presets_allowed(req)) return;
      JsonArrayConst arr = body["presets"].as<JsonArrayConst>();
      if (arr.isNull()) {
        send_error_with_message(req, 400, "missing_presets",
          "Body must include a `presets` array.");
        return;
      }
      if (arr.size() > LXMF::Messenger::MAX_PRESETS) {
        send_error_with_message(req, 400, "too_many_presets",
          "At most 8 presets fit on the screen.");
        return;
      }
      std::vector<LXMF::Messenger::Preset> next;
      for (JsonObjectConst o : arr) {
        LXMF::Messenger::Preset p;
        p.label    = (const char*)(o["label"]   | "");
        p.dest_hex = (const char*)(o["dest"]    | "");
        p.content  = (const char*)(o["content"] | "");
        if (o["telemetry"].is<JsonObjectConst>()) {
          JsonObjectConst t = o["telemetry"];
          p.tel_location    = (bool)(t["location"]    | false);
          p.tel_environment = (bool)(t["environment"] | false);
          p.tel_battery     = (bool)(t["battery"]     | false);
          p.tel_compass     = (bool)(t["compass"]     | false);
          p.tel_share_s     = (uint32_t)(t["share_s"] | 0);
          p.tel_rate_s      = (uint32_t)(t["rate_s"]  | 60);
          if (p.tel_share_s > 24 * 3600) p.tel_share_s = 24 * 3600;
          if (p.tel_rate_s < 15)   p.tel_rate_s = 15;
          if (p.tel_rate_s > 3600) p.tel_rate_s = 3600;
        }
        if (p.label.empty() || p.label.size() > LXMF::Messenger::MAX_LABEL_LEN) {
          send_error_with_message(req, 400, "bad_label",
            "Each preset needs a label of 1 to 24 characters.");
          return;
        }
        // Empty destination = template (kept in the editor, hidden on
        // the device until a recipient is chosen).
        if (!p.dest_hex.empty() &&
            (p.dest_hex.size() != 32 ||
             p.dest_hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)) {
          send_error_with_message(req, 400, "bad_dest",
            "Recipient addresses are 32 hex characters.");
          return;
        }
        if (p.content.empty() || p.content.size() > LXMF::Messenger::MAX_CONTENT_LEN) {
          send_error_with_message(req, 400, "bad_content",
            "Each preset needs a message of 1 to 200 bytes.");
          return;
        }
        next.push_back(std::move(p));
      }
      LXMF::Messenger::replace_presets(std::move(next));
      LXMF::Messenger::persist(filesystem);
      Common::PsramJsonDocument doc;
      fill_messenger_presets(doc);
      send_json(req, 200, doc);
    }
