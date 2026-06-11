// OLED messenger presets.
//
// GET  /api/messenger/presets  - current list + caps.
// POST /api/messenger/presets  - replace the whole list (it is tiny;
//                                item CRUD would be more states than
//                                data). Persists to /lxmf/messenger.json.
//
// State lives in LXMF::Messenger; this file only translates HTTP.

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
        o["gps"]     = p.gps;
      }
    }

    static void handle_messenger_presets_get(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      fill_messenger_presets(doc);
      send_json(req, 200, doc);
    }

    static void handle_messenger_presets_post(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
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
        p.gps      = (bool)(o["gps"] | false);
        if (p.label.empty() || p.label.size() > LXMF::Messenger::MAX_LABEL_LEN) {
          send_error_with_message(req, 400, "bad_label",
            "Each preset needs a label of 1 to 24 characters.");
          return;
        }
        if (p.dest_hex.size() != 32 ||
            p.dest_hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) {
          send_error_with_message(req, 400, "bad_dest",
            "Each preset needs a 32-hex-character destination.");
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
