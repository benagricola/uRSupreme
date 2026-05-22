// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

#if HAS_WIFI && defined(TCP_TRANSPORT)
    // /reticulum/interfaces.json (managed by Discovery::Config) is the
    // source of truth for outbound TCP client interface definitions.
    // TCPTransport keeps the live runtime — its add_client / remove_client
    // construct or tear down the actual TCPClientInterface and register
    // it with RNS::Transport. These endpoints coordinate both: every
    // write to a tcp_client entry here also adjusts the runtime, and
    // every list read pulls live `online` from the runtime.

    static void handle_tcp_clients_list(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["clients"].to<JsonArray>();
      // Walk the in-memory Discovery::Config map. For each tcp_client
      // entry, look up its runtime status (online + slot) from
      // TCPTransport so the UI can show whether the connection is up.
      for (const auto& kv : Discovery::Config::all()) {
        if (kv.second.type != Discovery::Config::Type::TcpClient) continue;
        JsonObject o = arr.add<JsonObject>();
        o["name"]          = kv.first;
        o["host"]          = kv.second.host;
        o["port"]          = kv.second.port;
        o["discoverable"]  = kv.second.discoverable;
        const int slot = TCPTransport::find_client_slot(kv.first.c_str());
        o["slot"]          = slot;
        o["online"]        = (slot >= 0)
            && TCPTransport::client_impls[slot]
            && TCPTransport::client_impls[slot]->connected();
      }
      doc["capacity"]    = TCPTransport::client_capacity();
      doc["live_count"]  = TCPTransport::live_client_count();
      send_json(req, 200, doc);
    }

    static void handle_tcp_clients_add(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      std::string name = (const char*)(body["name"] | "");
      std::string host = (const char*)(body["host"] | "");
      int port         = (int)(body["port"] | 0);
      if (name.empty() || host.empty() || port <= 0 || port > 65535) {
        send_error_with_message(req, 400, "invalid_args",
          "Body must include `name`, `host`, and 1<=`port`<=65535.");
        return;
      }
      // A TCP client is outbound-only; peers cannot connect to it,
      // so advertising it via discovery would be wrong. Reject any
      // attempt to set discoverable=true even if a client tried.
      if ((bool)(body["discoverable"] | false)) {
        send_error_with_message(req, 400, "not_discoverable",
          "TCP clients can't be advertised — peers can't connect to an outbound-only link.");
        return;
      }
      if (Discovery::Config::get(name)) {
        send_error_with_message(req, 409, "name_exists",
          "An interface with this name already exists.");
        return;
      }
      if (!TCPTransport::add_client(name.c_str(), host.c_str(), (uint16_t)port)) {
        send_error_with_message(req, 503, "tcp_clients_full",
          "All TCP client slots are in use. Remove one before adding another.");
        return;
      }
      Discovery::Config::Entry e;
      e.type         = Discovery::Config::Type::TcpClient;
      e.host         = host;
      e.port         = port;
      e.discoverable = false;
      if (!Discovery::Config::upsert(name, e)) {
        TCPTransport::remove_client(name.c_str());
        send_error_with_message(req, 500, "persist_failed",
          "Could not persist interface config to /reticulum/interfaces.json.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]       = "created";
      doc["name"]         = name;
      doc["host"]         = host;
      doc["port"]         = port;
      send_json(req, 201, doc);
    }

    static void handle_tcp_clients_remove(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const std::string name = std::string(req->pathArg(0).c_str());
      Discovery::Config::Entry e;
      if (!Discovery::Config::get(name, &e) || e.type != Discovery::Config::Type::TcpClient) {
        send_error_with_message(req, 404, "not_found",
          "No TCP client with that name is configured.");
        return;
      }
      TCPTransport::remove_client(name.c_str());
      Discovery::Config::remove(name);
      Common::PsramJsonDocument doc;
      doc["status"] = "removed";
      doc["name"]   = name;
      send_json(req, 200, doc);
    }

    static void handle_tcp_clients_patch(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      const std::string name = std::string(req->pathArg(0).c_str());
      Discovery::Config::Entry e;
      if (!Discovery::Config::get(name, &e) || e.type != Discovery::Config::Type::TcpClient) {
        send_error_with_message(req, 404, "not_found",
          "No TCP client with that name is configured.");
        return;
      }
      // TCP clients can't be advertised — see handle_tcp_clients_add.
      if ((bool)(body["discoverable"] | false)) {
        send_error_with_message(req, 400, "not_discoverable",
          "TCP clients can't be advertised — peers can't connect to an outbound-only link.");
        return;
      }
      bool host_port_changed = false;
      if (body["host"].is<const char*>()) {
        const std::string nh = (const char*)(body["host"] | "");
        if (!nh.empty() && nh != e.host) { e.host = nh; host_port_changed = true; }
      }
      if (body["port"].is<int>()) {
        const int np = body["port"].as<int>();
        if (np > 0 && np <= 65535 && np != e.port) { e.port = np; host_port_changed = true; }
      }
      // Always clamp discoverable to false on TCP clients regardless
      // of what's in the body — defensive against legacy persisted
      // entries that have it set to true from before this restriction.
      e.discoverable = false;
      if (!Discovery::Config::upsert(name, e)) {
        send_error_with_message(req, 500, "persist_failed",
          "Could not persist interface config update.");
        return;
      }
      if (host_port_changed) {
        TCPTransport::remove_client(name.c_str());
        TCPTransport::add_client(name.c_str(), e.host.c_str(), (uint16_t)e.port);
      }
      Common::PsramJsonDocument doc;
      doc["status"]       = "updated";
      doc["name"]         = name;
      doc["host"]         = e.host;
      doc["port"]         = e.port;
      send_json(req, 200, doc);
    }
#endif  // HAS_WIFI && TCP_TRANSPORT

