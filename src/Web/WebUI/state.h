// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h -
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_state(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      // One-shot snapshot the SPA fetches on first connect (and on
      // SSE-reconnect after a long drop). Returns the conversation list,
      // both announce rings, and the since markers the SPA passes to the
      // SSE handler so deltas resume cleanly without duplicating events.
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string& requested = caller;  // session identity (bearer token); no {id} in path
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a) { send_error(req, 404, "unknown_identity"); return; }

      Common::PsramJsonDocument doc;
      // Identity self-info.
      JsonObject me = doc["identity"].to<JsonObject>();
      me["id"]                   = a->id;
      me["display_name"]         = a->display_name;
      me["address"]              = a->address_hex();
      me["announce_interval_ms"] = a->announce_interval_ms;
      {
        uint32_t next_in = 0;
        if (a->announce_interval_ms > 0) {
          uint32_t elapsed = millis() - a->last_announce_ms;
          next_in = (elapsed >= a->announce_interval_ms) ? 0 : (a->announce_interval_ms - elapsed);
        }
        me["next_announce_in_ms"] = next_in;
      }
      // Compose-popover telemetry defaults: the SPA seeds the attach
      // popover from identity.telemetry on first open, which can
      // happen before the Settings tab ever fetches /api/identity.
      fill_identity_telemetry(me, a);

      // Conversation list: group recent inbox+outbox messages by peer.
      // Each conversation gets last_msg/last_ts/direction and a message
      // slice. The SPA can request more on demand via the existing
      // /inbox?since= and /outbox?since= endpoints.
      JsonArray convs = doc["conversations"].to<JsonArray>();
      // Walk inbox + outbox once, keeping the latest record per peer plus
      // the full message slice. Build a small lookup map<peer_hex, idx>.
      struct ConvAccum {
        std::string peer_hex;
        std::vector<LXMF::MessageRecord> msgs;  // chronological
      };
      std::vector<ConvAccum> convs_list;
      auto upsert = [&](const LXMF::MessageRecord& m) {
        std::string ph = m.peer_hash.toHex();
        for (auto& c : convs_list) {
          if (c.peer_hex == ph) { c.msgs.push_back(m); return; }
        }
        convs_list.push_back({ph, {m}});
      };
      // Iterate the deques directly + take the trailing 64-record
      // window. Tail slice rather than a vector copy of the entire
      // ring - the conversations endpoint fires on every page load.
      auto take_tail = [](const LXMF::MessageRing& ring,
                           const std::function<void(const LXMF::MessageRecord&)>& fn) {
        constexpr size_t W = 64;
        const size_t skip = (ring.size() > W) ? (ring.size() - W) : 0;
        size_t idx = 0;
        for (const auto& m : ring) {
          if (idx++ < skip) continue;
          fn(m);
        }
      };
      if (a->inbox)  take_tail(a->inbox->ring(),  upsert);
      if (a->outbox) take_tail(a->outbox->ring(), upsert);
      // Order by (boot_epoch, received_ms) - see LXMFTypes.h. boot_epoch
      // makes the tuple monotonic across reboots; received_ms breaks
      // ties within a boot. ts is for display only.
      auto less_key = [](const LXMF::MessageRecord& x, const LXMF::MessageRecord& y){
        if (x.boot_epoch != y.boot_epoch) return x.boot_epoch < y.boot_epoch;
        return x.received_ms < y.received_ms;
      };
      for (auto& c : convs_list) {
        std::sort(c.msgs.begin(), c.msgs.end(), less_key);
      }
      std::sort(convs_list.begin(), convs_list.end(),
                [&](const ConvAccum& x, const ConvAccum& y){
                  if (x.msgs.empty()) return false;
                  if (y.msgs.empty()) return true;
                  return less_key(y.msgs.back(), x.msgs.back());
                });
      for (const auto& c : convs_list) {
        JsonObject co = convs.add<JsonObject>();
        co["peer"] = c.peer_hex;
        if (!c.msgs.empty()) {
          const auto& last = c.msgs.back();
          co["last_ts"]          = last.ts;
          co["last_boot_epoch"]  = last.boot_epoch;
          co["last_received_ms"] = last.received_ms;
          co["last_body"]        = last.content;
          co["last_in"]          = last.incoming;
        }
        JsonArray msgs = co["messages"].to<JsonArray>();
        for (const auto& m : c.msgs) {
          JsonObject mo = msgs.add<JsonObject>();
          mo["seq"]         = m.seq;
          mo["ts"]          = m.ts;
          mo["boot_epoch"]  = m.boot_epoch;
          mo["received_ms"] = m.received_ms;
          mo["title"]       = m.title;
          mo["body"]        = m.content;
          if (m.has_telemetry) mo["tel"] = true;
          if (m.telemetry.size() > 0) {
            Telemetry::Telemeter::decode_into(mo["tele"].to<JsonObject>(),
                                              m.telemetry.data(), m.telemetry.size());
          }
          mo["in"]          = m.incoming;
          mo["sig_ok"]      = m.signature_ok;
          mo["status"]      = LXMF::outbox_status_name(m.status);
          // Delivery-stamp verdict - present only when a stamp policy
          // applied to this message. See emit_messages_array.
          if (m.stamp_checked) {
            mo["stamp_ok"] = m.stamp_valid;
            if (m.stamp_value >= 0) mo["stamp_value"] = m.stamp_value;
          }
          if (!m.attachments.empty()) {
            JsonArray atts = mo["attachments"].to<JsonArray>();
            for (const auto& a : m.attachments) {
              JsonObject o = atts.add<JsonObject>();
              o["tag"]      = a.tag;
              o["size"]     = a.size;
              o["filename"] = a.filename;
              if (!a.display_name.empty()) o["display_name"] = a.display_name;
              if (!a.mime.empty()) o["mime"] = a.mime;
            }
          }
        }
      }

      // Announce ring snapshot. Realtime updates flow via WebSocket
      // (announce_seen / path_seen events) - this is just the initial
      // catch-up at SPA load.
      JsonArray announces = doc["announces"].to<JsonArray>();
      for (const auto& rec : LXMF::AnnounceLog::announces()) {
        JsonObject o = announces.add<JsonObject>();
        o["dest"]          = rec.destination.toHex();
        o["display_name"]  = rec.display_name;
        o["aspect"]        = rec.aspect;
        o["received_ms"]   = rec.received_ms;
        o["age_ms"]        = millis() - rec.received_ms;
      }

      JsonArray paths = doc["paths"].to<JsonArray>();
      for (const auto& rec : LXMF::AnnounceLog::paths()) {
        JsonObject o = paths.add<JsonObject>();
        o["dest"]          = rec.destination.toHex();
        o["display_name"]  = rec.display_name;
        o["aspect"]        = rec.aspect;
        o["received_ms"]   = rec.received_ms;
        o["age_ms"]        = millis() - rec.received_ms;
      }

      // Display-time clock anchor is delivered via the WS `hello` frame
      // (see Web::WS::set_hello_extras in WebUI::start). No need to ship
      // it on every /state fetch - the SPA only ever sets state.clockAnchor
      // once per WS session, and `hello` lands before the SPA mounts the
      // conversation list.
      send_json(req, 200, doc);
    }

    // Verify that this request has physical-presence authority - either a
    // valid bearer token (which required a button press to obtain) or a
    // current identity_code in the request body.
