// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_path_lookup(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string to_hex = (const char*)(body["to"] | "");
      RNS::Bytes to = hex_to_bytes(to_hex, LXMF::HASH_LEN);
      if (to.size() != LXMF::HASH_LEN) {
        send_error(req, 400, "invalid_destination_hash");
        return;
      }
      bool known = RNS::Transport::has_path(to);
      Common::PsramJsonDocument doc;
      doc["known"] = known;
      if (!known) {
        RNS::Transport::request_path(to);
        doc["requested"] = true;
      }
      send_json(req, 200, doc);
    }

    // GET /api/paths/estimate?to=<32 hex>&bytes=<N>
    // Returns transmit-time estimate components for the destination,
    // letting the SPA render an ETA before the user hits send.
    //   kind: "local"   — destination is one of our own identities;
    //                     traffic loops in-device, ETA ≈ 0
    //         "path"    — we have a path table entry; eta_ms is
    //                     ((bytes*8 / bitrate)*(hops+1) + first_hop)*1000
    //         "unknown" — no path yet; SPA should request_path or
    //                     show "ETA unknown"
    //
    // Bytes is the total LXMF wire size, which the SPA computes from
    // the message body + sum-of-attachments + a ~120-byte overhead
    // (hash + signature + msgpack framing).
    static void handle_path_estimate(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      const std::string to_hex = std::string(req->arg("to").c_str());
      const uint32_t bytes = (uint32_t)std::strtoul(req->arg("bytes").c_str(),
                                                    nullptr, 10);
      RNS::Bytes to = hex_to_bytes(to_hex, LXMF::HASH_LEN);
      if (to.size() != LXMF::HASH_LEN) {
        send_error_with_message(req, 400, "invalid_destination_hash",
          "Destination must be 32 hex characters.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["bytes"] = bytes;

      if (LXMF::LXMFGateway::is_own_destination(to)) {
        doc["kind"]   = "local";
        doc["eta_ms"] = 0;
        send_json(req, 200, doc);
        return;
      }
      if (!RNS::Transport::has_path(to)) {
        doc["kind"]    = "unknown";
        doc["eta_ms"]  = nullptr;
        send_json(req, 200, doc);
        return;
      }
      uint32_t       bitrate = RNS::Transport::next_hop_interface_bitrate(to);
      uint8_t        hops    = RNS::Transport::hops_to(to);
      const double   fh_to   = RNS::Transport::first_hop_timeout(to);
      // Fallback path: microReticulum's path-table entries can come
      // back "degraded" after a serialise→deserialise round-trip in
      // the typed store (interface ref looked up by hash fails, or
      // the cached announce packet doesn't unpack). In that state
      // `has_path` is still true (the row exists) but `hops_to`
      // returns PATHFINDER_M and `next_hop_interface_bitrate` returns
      // 0 — leaving the SPA stuck at "ETA: pending path estimate"
      // even though we have everything we need locally to estimate.
      //
      // Workaround: when the Transport doesn't have a usable bitrate
      // or hop count, assume the destination is reachable over our
      // local LoRa interface at hops=0 (direct RF). This is correct
      // for the SX↔LR bench setup and any other direct-RF pair; for
      // multi-hop links it just under-estimates the ETA, which is a
      // better UX than refusing to show one.
      // The real fix belongs in microReticulum (#165).
      bool estimate_fallback = false;
      if ((bitrate == 0 || hops == RNS::Type::Transport::PATHFINDER_M)
          && lora_bitrate > 0) {
        bitrate = lora_bitrate;
        hops    = 0;
        estimate_fallback = true;
      }
      doc["kind"]     = "path";
      doc["bitrate"]  = bitrate;
      doc["hops"]     = (int)hops;
      doc["first_hop_timeout_ms"] = (uint32_t)(fh_to * 1000);
      if (estimate_fallback) doc["estimate_fallback"] = true;
      if (bitrate > 0 && hops != RNS::Type::Transport::PATHFINDER_M) {
        // bytes*8 / bitrate = seconds to clock a single packet over
        // one hop; multiply by (hops + 1) for the cumulative on-air
        // time then add the first-hop turnaround.
        const double sec = ((double)bytes * 8.0 / (double)bitrate) * (hops + 1)
                           + fh_to;
        doc["eta_ms"] = (uint32_t)(sec * 1000);
      } else {
        doc["eta_ms"] = nullptr;
      }
      send_json(req, 200, doc);
    }

