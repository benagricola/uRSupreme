#pragma once

#include <Reticulum.h>
#include <Interface.h>
#include <Log.h>
#include <Bytes.h>

#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiClient.h>

#include "HDLC.h"

extern bool wifi_initialized;

#if defined(HAS_RNS) && defined(TCP_TRANSPORT)

// One-per-accepted-connection peer. Registered with the Reticulum transport
// independently so each remote sees a distinct interface hash.
class TCPServerPeer : public RNS::InterfaceImpl {
public:
  static constexpr uint16_t DEFAULT_HW_MTU = 1064;

  TCPServerPeer(const char* name, WiFiClient client)
    : RNS::InterfaceImpl(name), _client(client) {
    _IN = true;
    _OUT = true;
    _HW_MTU = DEFAULT_HW_MTU;
    _online = _client.connected();
  }

  virtual ~TCPServerPeer() {
    if (_client.connected()) _client.stop();
    _name = "deleted";
  }

  // Returns false once the underlying socket has dropped.
  bool service() {
    if (!_client.connected()) {
      _online = false;
      return false;
    }
    int avail = _client.available();
    while (avail-- > 0) {
      int b = _client.read();
      if (b < 0) break;
      _decoder.feed((uint8_t)b, [this](const RNS::Bytes& frame) {
        try {
          this->handle_incoming(frame);
        } catch (const std::bad_alloc&) {
          ERROR("TCPServerPeer::service: bad_alloc - out of memory");
        } catch (std::exception& e) {
          ERRORF("TCPServerPeer::service: %s", e.what());
        }
      });
    }
    return true;
  }

protected:
  virtual void send_outgoing(const RNS::Bytes& data) override {
    try {
      if (_client.connected()) {
        RNS::Bytes framed;
        HDLC::encode(data, framed);
        size_t written = _client.write(framed.data(), framed.size());
        if (written != framed.size()) {
          _client.stop();
          _online = false;
        }
      }
      InterfaceImpl::handle_outgoing(data);
    } catch (const std::bad_alloc&) {
      ERROR("TCPServerPeer::send_outgoing: bad_alloc - out of memory");
    } catch (std::exception& e) {
      ERRORF("TCPServerPeer::send_outgoing: %s", e.what());
    }
  }

private:
  WiFiClient    _client;
  HDLC::Decoder _decoder;
};

#endif
