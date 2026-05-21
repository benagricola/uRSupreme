#pragma once

#include <Reticulum.h>
#include <Interface.h>
#include <Log.h>
#include <ArduinoJson.h>

#include "TCPClientInterface.h"
#include "TCPServerInterface.h"
#include "Discovery/Config.h"

#include <WiFi.h>
#include <WiFiServer.h>
#include <string>

#include <microStore/FileSystem.h>

extern microStore::FileSystem filesystem;
extern bool wifi_initialized;

#if defined(HAS_RNS) && defined(TCP_TRANSPORT)

// Compile-time caps. Each peer/client carries an HDLC reassembly buffer
// (~1 KB) plus an lwIP TCP socket (~6 KB recv buffer). Keep these small.
#ifndef TCP_MAX_CLIENTS
#define TCP_MAX_CLIENTS 4
#endif

#ifndef TCP_MAX_SERVER_PEERS
#define TCP_MAX_SERVER_PEERS 3
#endif

#ifndef TCP_CONFIG_PATH
#define TCP_CONFIG_PATH "/tcp_config.json"
#endif

namespace TCPTransport {

  struct ClientCfg {
    String   name;
    String   host;
    uint16_t port;
    uint32_t reconnect_ms;
  };

  struct ServerCfg {
    bool     enabled;
    uint16_t port;
    uint8_t  max_peers;
  };

  // Persistent storage for outbound TCP client interfaces.
  // RNS::Interface wraps a shared_ptr to InterfaceImpl, so we keep the
  // Interface objects alive here for lookups; the underlying impls own
  // their own state.
  static RNS::Interface  client_interfaces[TCP_MAX_CLIENTS] = {
    RNS::Interface(RNS::Type::NONE), RNS::Interface(RNS::Type::NONE),
    RNS::Interface(RNS::Type::NONE), RNS::Interface(RNS::Type::NONE)
  };
  static TCPClientInterface* client_impls[TCP_MAX_CLIENTS] = { nullptr, nullptr, nullptr, nullptr };
  static uint8_t client_count = 0;

  // Server state. Each accepted connection becomes its own Reticulum
  // interface so peers can be distinguished by Transport.
  static WiFiServer*       server          = nullptr;
  static ServerCfg         server_cfg      = { false, 4965, TCP_MAX_SERVER_PEERS };
  static RNS::Interface    server_peers[TCP_MAX_SERVER_PEERS] = {
    RNS::Interface(RNS::Type::NONE), RNS::Interface(RNS::Type::NONE), RNS::Interface(RNS::Type::NONE)
  };
  static TCPServerPeer*    server_peer_impls[TCP_MAX_SERVER_PEERS] = { nullptr, nullptr, nullptr };

  // Default config used if no JSON file is found. Modify or extend by
  // writing /tcp_config.json on the device's microStore filesystem.
  inline void apply_defaults(ClientCfg* clients, uint8_t& num_clients, ServerCfg& srv) {
    num_clients = 0;
    srv = { false, 4965, TCP_MAX_SERVER_PEERS };
  }

  inline bool load_config(ClientCfg* clients, uint8_t& num_clients, ServerCfg& srv) {
    apply_defaults(clients, num_clients, srv);

    if (!filesystem.exists(TCP_CONFIG_PATH)) {
      RNS::log("TCPTransport: no config file, using defaults", RNS::LOG_DEBUG);
      return false;
    }

    size_t fsize = filesystem.size(TCP_CONFIG_PATH);
    if (fsize == 0 || fsize > 8192) {
      RNS::log("TCPTransport: config file size unreasonable", RNS::LOG_WARNING);
      return false;
    }

    std::unique_ptr<char[]> buf(new (std::nothrow) char[fsize + 1]);
    if (!buf) return false;
    size_t read = filesystem.readFile(TCP_CONFIG_PATH, (uint8_t*)buf.get(), fsize);
    buf[read] = 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, (const char*)buf.get());
    if (err) {
      RNS::logf(RNS::LOG_WARNING, "TCPTransport: config parse error: %s", err.c_str());
      return false;
    }

    if (doc["clients"].is<JsonArray>()) {
      for (JsonObject c : doc["clients"].as<JsonArray>()) {
        if (num_clients >= TCP_MAX_CLIENTS) break;
        const char* host = c["host"] | "";
        if (!host || !*host) continue;
        clients[num_clients].name         = (const char*)(c["name"] | "tcp_client");
        clients[num_clients].host         = host;
        clients[num_clients].port         = (uint16_t)(c["port"] | 4965);
        clients[num_clients].reconnect_ms = (uint32_t)(c["reconnect_ms"] | 5000);
        num_clients++;
      }
    }

    JsonObject srv_obj = doc["server"];
    if (srv_obj) {
      srv.enabled   = srv_obj["enabled"] | false;
      srv.port      = (uint16_t)(srv_obj["port"] | 4965);
      srv.max_peers = (uint8_t)(srv_obj["max_peers"] | TCP_MAX_SERVER_PEERS);
      if (srv.max_peers > TCP_MAX_SERVER_PEERS) srv.max_peers = TCP_MAX_SERVER_PEERS;
    }

    return true;
  }

  // Find the slot a named client occupies, or -1 if not present.
  inline int find_client_slot(const char* name) {
    if (!name) return -1;
    // Read the name through the wrapper — InterfaceImpl's _name is
    // protected, only the RNS::Interface wrapper exposes a public
    // accessor.
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; ++i) {
      if (client_impls[i] && client_interfaces[i].name() == name) return (int)i;
    }
    return -1;
  }

  // Construct a TCP client interface + register it with Transport.
  // Returns true on success. Reasons for failure: array full, name
  // collision with an existing client. Idempotent for name-conflict
  // (does not replace; caller should remove_client() first if it
  // wants to swap host/port for the same name).
  inline bool add_client(const char* name, const char* host, uint16_t port,
                          uint32_t reconnect_ms = TCPClientInterface::DEFAULT_RECONNECT_MS) {
    if (!name || !*name || !host || !*host) return false;
    if (find_client_slot(name) >= 0) return false;
    int slot = -1;
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; ++i) {
      if (!client_impls[i]) { slot = (int)i; break; }
    }
    if (slot < 0) {
      RNS::logf(RNS::LOG_WARNING, "TCPTransport: add_client('%s') refused — all %u slots full",
                name, (unsigned)TCP_MAX_CLIENTS);
      return false;
    }
    auto* impl = new TCPClientInterface(name, host, port, reconnect_ms);
    client_impls[slot] = impl;
    client_interfaces[slot] = impl;
    client_interfaces[slot].mode(RNS::Type::Interface::MODE_GATEWAY);
    RNS::Transport::register_interface(client_interfaces[slot]);
    client_count = 0;
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; ++i) if (client_impls[i]) ++client_count;
    RNS::logf(RNS::LOG_NOTICE, "TCPTransport: add_client('%s') %s:%u → slot %d",
              name, host, (unsigned)port, slot);
    return true;
  }

  // Deregister + destroy a TCP client by name. Returns true if a
  // client was actually removed. The shared_ptr in Transport's
  // interface table will free the impl once we drop our raw ref.
  inline bool remove_client(const char* name) {
    int slot = find_client_slot(name);
    if (slot < 0) return false;
    RNS::Transport::deregister_interface(client_interfaces[slot]);
    client_interfaces[slot].clear();
    client_impls[slot] = nullptr;
    client_count = 0;
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; ++i) if (client_impls[i]) ++client_count;
    RNS::logf(RNS::LOG_NOTICE, "TCPTransport: remove_client('%s') → slot %d freed", name, slot);
    return true;
  }

  // Iterate live clients. The visitor receives (slot, *impl); skip
  // empty slots. Used by /api/transport/tcp_clients for status.
  template <typename V>
  inline void for_each_client(V&& visit) {
    for (uint8_t i = 0; i < TCP_MAX_CLIENTS; ++i) {
      if (client_impls[i]) visit((int)i, client_impls[i]);
    }
  }

  inline uint8_t client_capacity() { return TCP_MAX_CLIENTS; }
  inline uint8_t live_client_count() { return client_count; }

  inline void setup() {
    // Server-side config still lives in the legacy /tcp_config.json
    // for the moment (separate concern from outbound clients, no UI
    // yet). The clients block there is now ignored — outbound TCP
    // clients are sourced exclusively from Discovery::Config's
    // /reticulum/interfaces.json entries of type=tcp_client.
    ClientCfg ignored_clients[TCP_MAX_CLIENTS];
    uint8_t ignored_num_clients = 0;
    load_config(ignored_clients, ignored_num_clients, server_cfg);

    // Outbound clients: iterate Discovery::Config entries.
    for (const auto& kv : Discovery::Config::all()) {
      if (kv.second.type != Discovery::Config::Type::TcpClient) continue;
      add_client(kv.first.c_str(), kv.second.host.c_str(),
                 (uint16_t)kv.second.port);
    }

    if (server_cfg.enabled) {
      server = new WiFiServer(server_cfg.port, server_cfg.max_peers);
      server->begin();
      server->setNoDelay(true);
      RNS::logf(RNS::LOG_NOTICE, "TCPTransport: server listening on :%u", (unsigned)server_cfg.port);
    }
  }

  inline void service_clients() {
    for (uint8_t i = 0; i < client_count; ++i) {
      if (client_impls[i]) client_impls[i]->service();
    }
  }

  inline void service_server() {
    if (!server || !wifi_initialized) return;

    // Reap dropped peers first to free slots.
    // Ownership: Interface wraps std::shared_ptr<InterfaceImpl>, and
    // Transport::_interfaces stores Interface by value (Transport.h:75),
    // so it holds its own shared_ptr ref to the impl we created with
    // `new`. We must drop Transport's ref via deregister_interface()
    // before clearing our own — once both refs are gone, shared_ptr's
    // deleter frees the impl. The raw server_peer_impls[i] pointer
    // becomes dangling at that point; we only use it as a slot marker.
    for (uint8_t i = 0; i < TCP_MAX_SERVER_PEERS; ++i) {
      if (server_peer_impls[i]) {
        if (!server_peer_impls[i]->service()) {
          RNS::Transport::deregister_interface(server_peers[i]);
          server_peers[i].clear();
          server_peer_impls[i] = nullptr;
        }
      }
    }

    while (server->hasClient()) {
      WiFiClient c = server->accept();
      if (!c) break;
      int slot = -1;
      for (uint8_t i = 0; i < server_cfg.max_peers && i < TCP_MAX_SERVER_PEERS; ++i) {
        if (!server_peer_impls[i]) { slot = i; break; }
      }
      if (slot < 0) {
        c.stop();
        RNS::log("TCPTransport: server full, rejecting connection", RNS::LOG_WARNING);
        continue;
      }
      char name[24];
      snprintf(name, sizeof(name), "tcp_peer_%d", slot);
      auto* impl = new TCPServerPeer(name, c);
      server_peer_impls[slot] = impl;
      server_peers[slot] = impl;
      server_peers[slot].mode(RNS::Type::Interface::MODE_GATEWAY);
      RNS::Transport::register_interface(server_peers[slot]);
      IPAddress ip = c.remoteIP();
      RNS::logf(RNS::LOG_NOTICE, "TCPTransport: accepted peer[%d] from %u.%u.%u.%u",
                slot, ip[0], ip[1], ip[2], ip[3]);
    }
  }

  inline void service() {
    service_clients();
    service_server();
  }

}

#endif
