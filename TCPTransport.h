#pragma once

#include <Reticulum.h>
#include <Interface.h>
#include <Log.h>
#include <ArduinoJson.h>

#include "TCPClientInterface.h"
#include "TCPServerInterface.h"

#include <WiFi.h>
#include <WiFiServer.h>

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

  inline void setup() {
    ClientCfg clients[TCP_MAX_CLIENTS];
    uint8_t num_clients = 0;
    load_config(clients, num_clients, server_cfg);

    for (uint8_t i = 0; i < num_clients; ++i) {
      auto* impl = new TCPClientInterface(
        clients[i].name.c_str(),
        clients[i].host.c_str(),
        clients[i].port,
        clients[i].reconnect_ms);
      client_impls[i] = impl;
      client_interfaces[i] = impl;
      client_interfaces[i].mode(RNS::Type::Interface::MODE_GATEWAY);
      RNS::Transport::register_interface(client_interfaces[i]);
      RNS::logf(RNS::LOG_NOTICE, "TCPTransport: client[%u] %s:%u",
                (unsigned)i, clients[i].host.c_str(), (unsigned)clients[i].port);
      client_count++;
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

    // Reap dropped peers first to free slots
    for (uint8_t i = 0; i < TCP_MAX_SERVER_PEERS; ++i) {
      if (server_peer_impls[i]) {
        if (!server_peer_impls[i]->service()) {
          // Connection dropped — unregister and free
          // Note: microReticulum's Transport doesn't currently expose
          // unregister_interface(). We just clear our handle; the impl
          // will keep its slot in Transport until next reboot, but it
          // won't be re-used because _online is false.
          server_peers[i].clear();
          delete server_peer_impls[i];
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
