#pragma once

#include <Reticulum.h>
#include <Interface.h>
#include <Log.h>
#include <Bytes.h>

#include <WiFi.h>
#include <WiFiClient.h>

#include "HDLC.h"

extern bool wifi_initialized;

#if defined(HAS_RNS) && defined(TCP_TRANSPORT)

class TCPClientInterface : public RNS::InterfaceImpl {
public:
  static constexpr unsigned long DEFAULT_RECONNECT_MS = 5000;
  static constexpr uint16_t      DEFAULT_HW_MTU       = 1064;
  static constexpr unsigned long CONNECT_TIMEOUT_MS   = 5000;
  // Cap on wall-clock spent draining buffered TCP per service() call. A large
  // backbone announce burst otherwise runs Transport::inbound (Ed25519 verify +
  // path-store work) for every buffered frame in a single loop pass, holding the
  // single-threaded loop for seconds and starving LoRa RX queue servicing. Unread
  // bytes stay socket-buffered and drain over the following iterations, so TCP
  // throughput is preserved while the loop keeps cycling fast enough to service
  // the radio. Tunable via -DTCP_SERVICE_BUDGET_MS.
#ifndef TCP_SERVICE_BUDGET_MS
#define TCP_SERVICE_BUDGET_MS 50
#endif
  static constexpr unsigned long SERVICE_BUDGET_MS = TCP_SERVICE_BUDGET_MS;

  // Optional hook the firmware wires to drain the LoRa radio's RX FIFO
  // between TCP frames. A burst of backbone traffic in the service() drain below
  // runs Transport::inbound per frame and can hold the single-threaded main
  // loop for 100+ ms; the SX126x has a single RX buffer, so a LoRa packet that
  // arrives in that window is overwritten before the loop's once-per-iteration
  // read. Pumping the FIFO (read-only; pushes to modem_packet_queue, no
  // Transport work) between frames keeps it drained. No-op by default.
  inline static void (*rx_pump)() = nullptr;

  // Diagnostic: largest TCP backlog (bytes buffered) seen at a service() entry.
  // A big value confirms backbone bursts are landing - the condition that, when
  // drained unbounded, froze the loop. Read/reset via /api/diag/loop.
  inline static uint32_t max_burst_bytes = 0;

  TCPClientInterface(const char* name,
                     const char* host,
                     uint16_t port,
                     unsigned long reconnect_ms = DEFAULT_RECONNECT_MS)
    : RNS::InterfaceImpl(name),
      _host(host), _port(port),
      _reconnect_ms(reconnect_ms),
      _state(State::DISCONNECTED),
      _last_attempt(0) {
    _IN = true;
    _OUT = true;
    _HW_MTU = DEFAULT_HW_MTU;
  }

  virtual ~TCPClientInterface() {
    if (_client.connected()) _client.stop();
    _name = "deleted";
  }

  void service() {
    if (!wifi_initialized || WiFi.status() != WL_CONNECTED) {
      if (_state == State::CONNECTED) {
        _client.stop();
        _state = State::DISCONNECTED;
        _online = false;
        _decoder.reset();
      }
      return;
    }

    switch (_state) {
      case State::DISCONNECTED: {
        unsigned long now = millis();
        if (now - _last_attempt >= _reconnect_ms) {
          _last_attempt = now;
          if (_client.connect(_host.c_str(), _port, CONNECT_TIMEOUT_MS)) {
            _state = State::CONNECTED;
            _online = true;
            _decoder.reset();
          }
        }
        break;
      }
      case State::CONNECTED: {
        if (!_client.connected()) {
          _client.stop();
          _state = State::DISCONNECTED;
          _online = false;
          _decoder.reset();
          break;
        }
        int avail = _client.available();
        if ((uint32_t)avail > max_burst_bytes) max_burst_bytes = (uint32_t)avail;
        const unsigned long svc_start = millis();
        while (avail-- > 0) {
          int b = _client.read();
          if (b < 0) break;
          _decoder.feed((uint8_t)b, [this](const RNS::Bytes& frame) {
            try {
              this->handle_incoming(frame);
            } catch (const std::bad_alloc&) {
              ERROR("TCPClientInterface::service: bad_alloc - out of memory");
            } catch (std::exception& e) {
              ERRORF("TCPClientInterface::service: %s", e.what());
            }
            if (rx_pump) rx_pump();   // service LoRa RX between frames
          });
          // Bound per-iteration TCP work so a backbone burst can't hold the loop
          // for seconds; the rest stays socket-buffered for the next iteration.
          if ((millis() - svc_start) >= SERVICE_BUDGET_MS) break;
        }
        break;
      }
    }
  }

  bool connected() const { return _state == State::CONNECTED; }
  const std::string& host() const { return _host; }
  uint16_t port() const { return _port; }

protected:
  virtual void send_outgoing(const RNS::Bytes& data) override {
    try {
      if (_state == State::CONNECTED && _client.connected()) {
        RNS::Bytes framed;
        HDLC::encode(data, framed);
        size_t written = _client.write(framed.data(), framed.size());
        if (written != framed.size()) {
          // Partial write - drop the connection so we resync on next reconnect
          _client.stop();
          _state = State::DISCONNECTED;
          _online = false;
          _decoder.reset();
        }
      }
      InterfaceImpl::handle_outgoing(data);
    } catch (const std::bad_alloc&) {
      ERROR("TCPClientInterface::send_outgoing: bad_alloc - out of memory");
    } catch (std::exception& e) {
      ERRORF("TCPClientInterface::send_outgoing: %s", e.what());
    }
  }

private:
  enum class State : uint8_t { DISCONNECTED, CONNECTED };

  std::string  _host;
  uint16_t     _port;
  unsigned long _reconnect_ms;
  WiFiClient   _client;
  HDLC::Decoder _decoder;
  State        _state;
  unsigned long _last_attempt;
};

#endif
