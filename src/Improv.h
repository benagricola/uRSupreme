#pragma once

// Improv WiFi over Serial — https://www.improv-wifi.com/serial/
//
// ESP Web Tools opens the USB CDC port for flashing and keeps the
// handle alive across the post-flash reboot. Once the device boots,
// the install button switches to the Improv handshake: scans for
// networks, prompts for credentials, and asks the device to connect.
// The device replies with a list of URLs the user can browse to (e.g.
// http://192.168.x.y/, http://rnode7d31.local/) so the user lands
// straight on the SPA without ever joining the softAP.
//
// Coexistence with KISS: KISS frames are delimited by 0xC0 (FEND).
// Improv frames start with the ASCII magic "IMPROV". Outside a KISS
// frame, the KISS callback silently discards any byte that isn't
// FEND, so leaking Improv magic bytes into the KISS FIFO is harmless.
// While the Improv parser owns the byte stream (post-magic), bytes
// are suppressed so binary SSID/PSK payloads don't accidentally
// trigger KISS framing.
//
// Wire format:
//   "IMPROV" (6) | version (1) | type (1) | length (1) | data (length) | checksum (1)
//   checksum = (sum of all preceding bytes) & 0xFF
//
// Active state lives in Common/Status.h as well so the OLED ticker
// (deferred) and the SPA topbar (/api/info.last_status) can show
// what's happening.

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
#include "Config.h"
#include "Common/WifiTransition.h"
#include "Common/Status.h"

extern char     bt_devname[11];
extern char     wr_hostname[10];
extern char     wr_ssid[33];
extern char     wr_psk[33];
extern uint8_t  wifi_mode;
extern PendingProvision wr_pending;
void wifi_remote_eeprom_write_sta_creds(const char* ssid, const char* psk);

namespace Improv {

namespace _detail {

  constexpr uint8_t MAGIC_BYTES[6] = {'I','M','P','R','O','V'};
  constexpr uint8_t PROTO_VERSION  = 1;

  constexpr uint8_t TYPE_CURRENT_STATE = 0x01;
  constexpr uint8_t TYPE_ERROR_STATE   = 0x02;
  constexpr uint8_t TYPE_RPC_COMMAND   = 0x03;
  constexpr uint8_t TYPE_RPC_RESULT    = 0x04;

  constexpr uint8_t STATE_AUTHORIZED   = 0x02;
  constexpr uint8_t STATE_PROVISIONING = 0x03;
  constexpr uint8_t STATE_PROVISIONED  = 0x04;

  constexpr uint8_t ERR_NONE                = 0x00;
  constexpr uint8_t ERR_INVALID_RPC_PACKET  = 0x01;
  constexpr uint8_t ERR_UNKNOWN_RPC_COMMAND = 0x02;
  constexpr uint8_t ERR_UNABLE_TO_CONNECT   = 0x03;

  constexpr uint8_t CMD_WIFI_SETTINGS     = 0x01;
  constexpr uint8_t CMD_IDENTIFY          = 0x02;
  constexpr uint8_t CMD_GET_STATE         = 0x03;
  constexpr uint8_t CMD_GET_DEVICE_INFO   = 0x04;
  constexpr uint8_t CMD_GET_WIFI_NETWORKS = 0x05;

  constexpr uint32_t PROVISION_TIMEOUT_MS = 30UL * 1000UL;
  constexpr uint32_t PARSER_IDLE_RESET_MS = 1000UL;

  enum class ParseState : uint8_t {
    WAIT_MAGIC,
    WAIT_VERSION,
    WAIT_TYPE,
    WAIT_LENGTH,
    WAIT_DATA,
    WAIT_CHECKSUM,
  };

  struct Parser {
    ParseState state         = ParseState::WAIT_MAGIC;
    uint8_t    magic_idx     = 0;
    uint8_t    packet_type   = 0;
    uint8_t    length        = 0;
    uint16_t   data_received = 0;
    uint32_t   checksum      = 0;
    uint32_t   last_byte_ms  = 0;
    uint8_t    data[256];
  };

  inline Parser& parser() { static Parser p; return p; }
  inline bool& prov_in_progress() { static bool b = false; return b; }
  inline uint32_t& prov_started_ms() { static uint32_t t = 0; return t; }

  // The "effective state" — what GET_CURRENT_STATE should answer right
  // now. Computed dynamically from WiFi.status() so a device that boots
  // with saved STA creds reports PROVISIONED (not the stale AUTHORIZED
  // from a cached variable). PROVISIONING wins over both — it's the
  // mid-flight WIFI_SETTINGS substate driven by Improv::loop.
  inline uint8_t effective_state() {
    if (prov_in_progress())              return STATE_PROVISIONING;
    if (WiFi.status() == WL_CONNECTED)   return STATE_PROVISIONED;
    return STATE_AUTHORIZED;
  }

  // Buffer the whole frame and emit via one Serial.write(buf, len) call.
  // Arduino's HWCDC::write() takes a per-call TX lock but releases it
  // between calls — so a sequence of one-byte writes can be sandwiched
  // by a Log.h NOTICEF/WARNINGF emitted from another FreeRTOS task
  // (the WebUI server task is on a different core). The host's Improv
  // parser would then see a corrupted frame. Writing once locks once.
  inline void write_frame(uint8_t type, const uint8_t* payload, uint8_t len) {
    uint8_t buf[6 + 1 + 1 + 1 + 255 + 1];
    // pos must be wider than uint8_t: a full frame is 9 header bytes +
    // up to 255 payload + checksum = 265, so an 8-bit offset wraps at
    // payloads >= 247 and emits a truncated, corrupt frame.
    size_t pos = 0;
    memcpy(buf + pos, MAGIC_BYTES, 6); pos += 6;
    buf[pos++] = PROTO_VERSION;
    buf[pos++] = type;
    buf[pos++] = len;
    if (len > 0 && payload != nullptr) { memcpy(buf + pos, payload, len); pos += len; }
    uint8_t cs = 0;
    for (size_t i = 0; i < pos; i++) cs += buf[i];
    buf[pos++] = cs;
    Serial.write(buf, pos);
  }

  inline void send_current_state() {
    uint8_t s = effective_state();
    write_frame(TYPE_CURRENT_STATE, &s, 1);
  }

  inline void send_error_state(uint8_t err) {
    write_frame(TYPE_ERROR_STATE, &err, 1);
  }

  // Build [cmd, inner_len, lp_str, lp_str, ...] and emit as RPC_RESULT.
  // strings[i] is a const char*; nulls are skipped. Returns nothing.
  template <size_t N>
  inline void send_rpc_result(uint8_t cmd, const char* (&strings)[N]) {
    uint8_t buf[256];
    size_t pos = 0;
    buf[pos++] = cmd;
    const size_t inner_len_pos = pos++;
    const size_t inner_start = pos;
    for (size_t i = 0; i < N; i++) {
      const char* s = strings[i];
      if (!s) continue;
      // The frame's payload length byte caps the whole RPC result at
      // 255 bytes: each lp_str needs 1 length byte + its data, so stop
      // once nothing more fits. The old `250 - pos` arithmetic went
      // negative (then huge, as size_t) past 250 and let memcpy run off
      // the end of buf.
      if (pos >= 254) break;
      size_t slen = strnlen(s, 254 - pos);
      buf[pos++] = (uint8_t)slen;
      memcpy(buf + pos, s, slen);
      pos += slen;
    }
    buf[inner_len_pos] = (uint8_t)(pos - inner_start);
    write_frame(TYPE_RPC_RESULT, buf, (uint8_t)pos);
  }

  inline void send_rpc_result_empty(uint8_t cmd) {
    uint8_t buf[2] = { cmd, 0 };
    write_frame(TYPE_RPC_RESULT, buf, 2);
  }

  inline void reset_parser() {
    Parser& p = parser();
    p.state = ParseState::WAIT_MAGIC;
    p.magic_idx = 0;
    p.checksum = 0;
    p.data_received = 0;
  }

  inline void handle_get_device_info() {
    const char* strings[] = {
      "uRSupreme",            // firmware name
      FW_VERSION_STRING,      // firmware version (injected by extra_script.py)
      "ESP32-S3",             // hardware chip family
      bt_devname,             // device name (e.g. "RNode 7D31")
    };
    send_rpc_result(CMD_GET_DEVICE_INFO, strings);
  }

  inline void handle_get_wifi_networks() {
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String ssid_s = WiFi.SSID(i);
      char   rssi_buf[16];
      snprintf(rssi_buf, sizeof(rssi_buf), "%ld", (long)WiFi.RSSI(i));
      const bool secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      const char* strings[] = {
        ssid_s.c_str(),
        rssi_buf,
        secure ? "YES" : "NO",
      };
      send_rpc_result(CMD_GET_WIFI_NETWORKS, strings);
    }
    WiFi.scanDelete();
    send_rpc_result_empty(CMD_GET_WIFI_NETWORKS);  // end-of-list marker
  }

  inline void handle_identify() {
    Common::Status::say("Improv: identify", 5000);
    // IDENTIFY has no RPC_RESULT data per spec — implementations
    // typically blink an LED / buzz. We surface a Status message so
    // the OLED ticker (when integrated) shows the device is being
    // physically located.
  }

  inline void handle_wifi_settings(const uint8_t* data, uint8_t len) {
    if (len < 2) { send_error_state(ERR_INVALID_RPC_PACKET); return; }
    const uint8_t ssid_len = data[0];
    if (ssid_len > 32 || (uint16_t)(1 + ssid_len + 1) > len) {
      send_error_state(ERR_INVALID_RPC_PACKET); return;
    }
    const uint8_t psk_len = data[1 + ssid_len];
    if (psk_len > 32 || (uint16_t)(1 + ssid_len + 1 + psk_len) > len) {
      send_error_state(ERR_INVALID_RPC_PACKET); return;
    }
    char ssid[33] = {0}, psk[33] = {0};
    memcpy(ssid, data + 1,                 ssid_len);
    memcpy(psk,  data + 1 + ssid_len + 1,  psk_len);
    NOTICEF("Improv: WIFI_SETTINGS ssid='%s'", ssid);
    // 35 s TTL — slightly longer than PROVISION_TIMEOUT_MS so the
    // "provisioning…" message naturally ages out by the time the
    // success / failure message lands, and doesn't permanently win
    // the latest() lookup against the TTL'd success message.
    Common::Status::say("Improv: provisioning", 35000);

    // EEPROM-persist via the shared helper, then route through the
    // existing wr_pending pump so the WifiPhase machine in Remote.h
    // drives the APSTA + STA transitions. req=nullptr signals "no HTTP
    // response to send" — drain_wifi_provision_response() early-outs
    // and Improv::loop() handles the response over serial instead.
    wifi_remote_eeprom_write_sta_creds(ssid, psk);
    if (wr_pending.req != nullptr || wr_pending.pending) {
      // An HTTP provision is already in flight; report busy.
      send_error_state(ERR_UNABLE_TO_CONNECT);
      return;
    }
    strncpy(wr_pending.ssid, ssid, 32); wr_pending.ssid[32] = 0;
    strncpy(wr_pending.psk,  psk,  32); wr_pending.psk[32]  = 0;
    wr_pending.req           = nullptr;
    wr_pending.requested_ms  = millis();
    wr_pending.pending       = true;

    prov_in_progress() = true;
    prov_started_ms()  = millis();
    send_current_state();   // now reports STATE_PROVISIONING via effective_state()
  }

  inline void dispatch_packet() {
    Parser& p = parser();
    if (p.packet_type != TYPE_RPC_COMMAND) return;
    if (p.length < 2) { send_error_state(ERR_INVALID_RPC_PACKET); return; }
    const uint8_t cmd_id  = p.data[0];
    const uint8_t cmd_len = p.data[1];
    if ((uint16_t)(2 + cmd_len) > p.length) {
      send_error_state(ERR_INVALID_RPC_PACKET); return;
    }
    const uint8_t* cmd_data = p.data + 2;
    switch (cmd_id) {
      case CMD_WIFI_SETTINGS:     handle_wifi_settings(cmd_data, cmd_len); break;
      case CMD_IDENTIFY:          handle_identify();                       break;
      case CMD_GET_STATE:         send_current_state();                    break;
      case CMD_GET_DEVICE_INFO:   handle_get_device_info();                break;
      case CMD_GET_WIFI_NETWORKS: handle_get_wifi_networks();              break;
      default:                    send_error_state(ERR_UNKNOWN_RPC_COMMAND);
    }
  }

}  // namespace _detail

// Feed every byte read from the USB CDC port through this before
// pushing into the KISS FIFO. Returns true if Improv consumed the
// byte (caller must drop it); false if it should pass through.
inline bool on_byte(uint8_t b) {
  using namespace _detail;
  Parser& p = parser();
  p.last_byte_ms = millis();

  switch (p.state) {
    case ParseState::WAIT_MAGIC:
      if (b == MAGIC_BYTES[p.magic_idx]) {
        if (p.magic_idx == 0) p.checksum = 0;
        p.checksum += b;
        p.magic_idx++;
        if (p.magic_idx == 6) p.state = ParseState::WAIT_VERSION;
      } else {
        // Restart match. If b is 'I' we begin a new candidate;
        // otherwise drop to idle. Either way, return false so the
        // byte passes through to KISS (which ignores stray bytes
        // outside a 0xC0-delimited frame).
        p.magic_idx = (b == MAGIC_BYTES[0]) ? 1 : 0;
        p.checksum  = (b == MAGIC_BYTES[0]) ? b : 0;
      }
      return false;

    case ParseState::WAIT_VERSION:
      p.checksum += b;
      if (b != PROTO_VERSION) { reset_parser(); return true; }
      p.state = ParseState::WAIT_TYPE;
      return true;

    case ParseState::WAIT_TYPE:
      p.checksum += b;
      p.packet_type = b;
      p.state = ParseState::WAIT_LENGTH;
      return true;

    case ParseState::WAIT_LENGTH:
      p.checksum += b;
      p.length = b;
      p.data_received = 0;
      p.state = (p.length == 0) ? ParseState::WAIT_CHECKSUM
                                 : ParseState::WAIT_DATA;
      return true;

    case ParseState::WAIT_DATA:
      p.data[p.data_received++] = b;
      p.checksum += b;
      if (p.data_received >= p.length) p.state = ParseState::WAIT_CHECKSUM;
      return true;

    case ParseState::WAIT_CHECKSUM:
      if (b == (uint8_t)(p.checksum & 0xFF)) {
        dispatch_packet();
      } else {
        send_error_state(ERR_INVALID_RPC_PACKET);
      }
      reset_parser();
      return true;
  }
  return false;
}

// Call from the main loop. Drives provisioning state transitions and
// resets a stuck mid-frame parser if no byte has arrived for ~1 s.
inline void loop() {
  using namespace _detail;
  Parser& p = parser();

  if (p.state != ParseState::WAIT_MAGIC && p.last_byte_ms != 0
      && (millis() - p.last_byte_ms) > PARSER_IDLE_RESET_MS) {
    reset_parser();
  }

  if (!prov_in_progress()) return;

  if (WiFi.status() == WL_CONNECTED) {
    String sta_url  = String("http://") + WiFi.localIP().toString() + "/";
    String mdns_url = String("http://") + wr_hostname + ".local/";
    const char* strings[] = { sta_url.c_str(), mdns_url.c_str() };
    send_rpc_result(CMD_WIFI_SETTINGS, strings);
    prov_in_progress() = false;
    send_current_state();   // STATE_PROVISIONED via effective_state()
    NOTICEF("Improv: STA up, IP %s", WiFi.localIP().toString().c_str());
    // No IP-on-marquee affirmation — the WiFi icon shows connected
    // state and the URL has already been handed back over USB-CDC to
    // the web flasher. Clear so the "Improv: provisioning" message
    // doesn't linger.
    Common::Status::clear();
  } else if ((millis() - prov_started_ms()) >= PROVISION_TIMEOUT_MS) {
    send_error_state(ERR_UNABLE_TO_CONNECT);
    prov_in_progress() = false;
    send_current_state();   // STATE_AUTHORIZED via effective_state()
    NOTICE("Improv: provisioning timed out");
    Common::Status::say("Improv: connect failed");   // sticky
    // Cancel the pending provision in the WifiPhase machine too —
    // otherwise the AP teardown timer keeps ticking.
    wr_pending.pending = false;
  }
}

}  // namespace Improv
