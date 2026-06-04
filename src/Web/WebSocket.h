#pragma once

// WebSocket event channel for the SPA.
//
// Replaces the SSE short-poll pattern with a persistent push stream:
// one TCP socket per browser tab, events delivered as JSON text frames
// as they happen. The SSE endpoint is retained alongside this for
// transitional compatibility — the SPA negotiates which to use.
//
// Wire protocol:
//   * Client connects to /api/ws?token=<bearer>&identity_id=<id>
//   * On accept the server sends one `{"type":"hello", ...}` frame
//     containing the most recent state markers so the SPA can resync.
//   * Server then streams typed events as they occur:
//       incoming, outbox_status, message_progress, message_complete,
//       announce_seen, path_seen, sensors_update, time_update,
//       identity_code_available, heartbeat
//   * Client may send `{"type":"ping"}` at any time; server replies
//     `{"type":"pong"}`. Used to keep middleboxes from idling the
//     connection out.
//
// Auth model: same bearer tokens as the REST API, scoped to one
// identity_id. A client's events are filtered to only those that
// concern its identity (incoming/outbox/progress) plus globally
// broadcast events (sensor/time/announce). A mismatched identity_id
// closes the socket with a 403-ish reason.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <vector>
#include <string>
#include <stdint.h>
#include <esp_heap_caps.h>
#include <memory>
#include <type_traits>
#include "../LXMF/LXMFTypes.h"
#include "../LXMF/AnnounceLog.h"
#include "../Telemetry/Radio.h"
#include "../Telemetry/Network.h"

namespace LXMF { struct MessageRecord; }

namespace Web {
namespace WS {

  inline AsyncWebSocket& server() {
    static AsyncWebSocket s("/api/ws");
    return s;
  }

  // Per-connection state. Filled at connect-time after auth.
  struct ClientState {
    uint32_t            client_id   = 0;
    LXMF::IdentityId    identity_id;       // bound at auth
    bool                authed      = false;
  };

  inline std::vector<ClientState>& clients() {
    static std::vector<ClientState> v;
    return v;
  }

  inline ClientState* find_client(uint32_t cid) {
    for (auto& c : clients()) if (c.client_id == cid) return &c;
    return nullptr;
  }

  // True when at least one authed client is connected. Lets publishers
  // skip work entirely when no SPA is listening — building JSON for a
  // broadcast-to-zero is wasted main-loop time, and matters because
  // WebUI::loop runs at ~50 Hz on the same task as reticulum.loop().
  inline bool any_subscribers() {
    for (const auto& c : clients()) if (c.authed) return true;
    return false;
  }

  // Forward to AuthTokens::validate without pulling its header here.
  // The implementation is wired in WebUI.h's register_routes(); see
  // bind_validator() below.
  using TokenValidator = LXMF::IdentityId(*)(const std::string& token_hex);
  inline TokenValidator& validator() { static TokenValidator v = nullptr; return v; }
  inline void bind_validator(TokenValidator v) { validator() = v; }

  // Broadcast a JSON event to every client whose identity_id matches
  // `scope`. Pass an empty IdentityId to broadcast to all authed
  // clients (e.g. global sensor / time / announce events).
  //
  // Heap-fragmentation note: the previous form did
  //   String s; serializeJson(doc, s); for (...) srv.text(id, s);
  // which churned the default heap twice per broadcast — once for the
  // Arduino String (resizing via repeated realloc) and once per client
  // for AsyncWebSocket's internal makeSharedBuffer std::vector copy.
  // Both landed in internal SRAM at the same size range
  // (~200–2 KiB) and at high frequency (sensors_update + system_update
  // + message_progress) — exactly the size range the ESP-IDF WiFi
  // driver's `esf_buf_alloc_dynamic` needs (1626 B for 802.11 TX frames),
  // so app-side churn fragmented region 1 of internal SRAM and starved
  // the WiFi MAC layer, dropping outbound frames and tearing down
  // Reticulum links.
  //
  // The new form does one allocation per broadcast: a SharedBuffer
  // sized exactly from measureJson(doc), serialised directly into via
  // serializeJson(doc, ptr, len). The shared_ptr is ref-counted across
  // all targeted clients — srv.text(id, buf) does NOT copy.
  // PSRAM-backed AsyncWebSocketSharedBuffer construction.
  //
  // AsyncWebSocketSharedBuffer is `std::shared_ptr<std::vector<uint8_t>>`
  // (typedef in ESPAsyncWebServer/AsyncWebSocket.h), and the inner
  // std::vector uses the default `std::allocator<uint8_t>` which goes
  // to internal SRAM. With the 1 Hz sensor / radio-telemetry /
  // progress broadcasts, every call allocates `n` bytes of internal
  // SRAM, serializes the JSON in, then frees on shared_ptr refcount
  // drop. Even though net usage stays flat, the alloc/free churn
  // fragments internal SRAM until the WiFi PHY can't grab a 1.6 KB
  // esf_buf and the device crashes.
  //
  // Fix: build a shared_ptr<std::vector<uint8_t>> whose backing
  // storage lives in PSRAM. We can't change the vector's allocator
  // type (would change `std::vector<uint8_t>` to a different type the
  // lib won't accept), but we *can* swap out the destructor via the
  // shared_ptr's custom-deleter slot, and we *can* construct a
  // layout-compatible "fake vector" whose pointer triplet (start,
  // finish, end_of_storage) points into PSRAM-allocated memory.
  //
  // Layout assumption: libstdc++ implements std::vector<T, Alloc>
  // (with empty Alloc via EBO) as three raw pointers in this order:
  //   T* _M_start
  //   T* _M_finish
  //   T* _M_end_of_storage
  // The static_assert below catches any future ABI break.
  namespace _psram_ws {
    struct VecGuts {
      uint8_t* start;
      uint8_t* finish;
      uint8_t* end_of_storage;
    };
    static_assert(sizeof(VecGuts) == sizeof(std::vector<uint8_t>),
                  "std::vector<uint8_t> layout assumption broken");
    static_assert(std::is_standard_layout<VecGuts>::value,
                  "VecGuts must be standard layout for type-punning");
  }

  // Build a PSRAM-backed AsyncWebSocketSharedBuffer of `n` bytes.
  // Returns an empty shared_ptr on PSRAM exhaustion (8 MB; unlikely).
  // The custom deleter frees both PSRAM allocations and crucially
  // does NOT call ~vector() — which would try std::allocator::
  // deallocate on PSRAM memory and crash.
  inline AsyncWebSocketSharedBuffer make_psram_ws_buffer(size_t n) {
    auto* data = static_cast<uint8_t*>(
      heap_caps_malloc(n, MALLOC_CAP_SPIRAM));
    if (!data) return {};
    auto* guts = static_cast<_psram_ws::VecGuts*>(
      heap_caps_malloc(sizeof(_psram_ws::VecGuts), MALLOC_CAP_SPIRAM));
    if (!guts) { heap_caps_free(data); return {}; }
    guts->start          = data;
    guts->finish         = data + n;
    guts->end_of_storage = data + n;
    auto deleter = [data, guts](std::vector<uint8_t>* /*v*/) {
      // Deliberately do NOT call v->~vector() — the vector was never
      // constructed (we type-punned VecGuts onto it). Calling the dtor
      // would invoke std::allocator::deallocate on `data`, which lives
      // in PSRAM, not the default heap → crash.
      heap_caps_free(data);
      heap_caps_free(guts);
    };
    return AsyncWebSocketSharedBuffer(
      reinterpret_cast<std::vector<uint8_t>*>(guts),
      std::move(deleter));
  }

  inline void broadcast(const JsonDocument& doc, const LXMF::IdentityId& scope = {}) {
    // Early out when nobody's listening — covers the LR-rig boot case
    // where the SPA hasn't connected yet and 1 Hz emitters would
    // otherwise generate a steady stream of unwanted alloc/free
    // cycles. Even though our PSRAM-backed make_psram_ws_buffer
    // doesn't churn internal SRAM, doing the JSON measure +
    // serialization is still wasted work with no subscribers.
    bool any_subscriber = false;
    for (const auto& c : clients()) {
      if (!c.authed) continue;
      if (!scope.empty() && c.identity_id != scope) continue;
      any_subscriber = true;
      break;
    }
    if (!any_subscriber) return;
    const size_t n = measureJson(doc);
    if (n == 0) return;
    AsyncWebSocketSharedBuffer buf = make_psram_ws_buffer(n);
    if (!buf) return;  // PSRAM exhausted; drop the event silently
    serializeJson(doc, buf->data(), n);
    auto& srv = server();
    for (const auto& c : clients()) {
      if (!c.authed) continue;
      if (!scope.empty() && c.identity_id != scope) continue;
      srv.text(c.client_id, buf);  // SharedBuffer overload — refcounted, no copy
    }
  }

  // Convenience: publish an "incoming" event for a freshly-appended
  // inbox record. Mirrors the SSE payload shape so the SPA's existing
  // event handlers can take both feeds.
  inline void publish_incoming(const LXMF::IdentityId& identity_id,
                               const LXMF::MessageRecord& m) {
    Common::PsramJsonDocument doc;
    doc["type"]            = "incoming";
    JsonObject msg         = doc["msg"].to<JsonObject>();
    msg["seq"]             = m.seq;
    msg["ts"]              = m.ts;
    msg["boot_epoch"]      = m.boot_epoch;
    msg["received_ms"]     = m.received_ms;
    msg["peer"]            = m.peer_hash.toHex();
    msg["title"]           = m.title;
    // Bodies are always inline now (capped at LXMF_MAX_BODY_BYTES on
    // the send path). The earlier body_on_disk spill + lazy /body
    // fetch was dead infrastructure — the SPA never called the
    // endpoint, so large bodies stayed unreadable from the UI even
    // though the bytes were on disk.
    msg["body"]            = m.content;
    msg["body_size"]       = m.body_size;
    msg["sig_ok"]          = m.signature_ok;
    if (!m.attachments.empty()) {
      JsonArray atts = msg["attachments"].to<JsonArray>();
      for (const auto& a : m.attachments) {
        JsonObject o = atts.add<JsonObject>();
        o["tag"]      = a.tag;
        o["size"]     = a.size;
        o["filename"] = a.filename;
        if (!a.display_name.empty()) o["display_name"] = a.display_name;
        if (!a.mime.empty())         o["mime"]         = a.mime;
        if (!a.backend.empty())      o["backend"]      = a.backend;
      }
    }
    broadcast(doc, identity_id);
  }

  // A newly-appended OUTBOUND message. Lets every connected client render
  // the sender bubble live, including for sends that originated from the
  // API or another browser session. The SPA dedups against its own
  // optimistic bubble by seq / pkt.
  inline void publish_outbound(const LXMF::IdentityId& identity_id,
                               const LXMF::MessageRecord& m) {
    Common::PsramJsonDocument doc;
    doc["type"]        = "outbox_new";
    JsonObject msg     = doc["msg"].to<JsonObject>();
    msg["seq"]         = m.seq;
    msg["ts"]          = m.ts;
    msg["boot_epoch"]  = m.boot_epoch;
    msg["received_ms"] = m.received_ms;
    msg["peer"]        = m.peer_hash.toHex();
    msg["title"]       = m.title;
    msg["body"]        = m.content;
    msg["body_size"]   = m.body_size;
    msg["in"]          = false;
    msg["status"]      = LXMF::outbox_status_name(m.status);
    if (m.packet_hash.size() > 0) msg["pkt"] = m.packet_hash.toHex();
    if (!m.attachments.empty()) {
      JsonArray atts = msg["attachments"].to<JsonArray>();
      for (const auto& a : m.attachments) {
        JsonObject o = atts.add<JsonObject>();
        o["tag"]      = a.tag;
        o["size"]     = a.size;
        o["filename"] = a.filename;
        if (!a.display_name.empty()) o["display_name"] = a.display_name;
        if (!a.mime.empty())         o["mime"]         = a.mime;
        if (!a.backend.empty())      o["backend"]      = a.backend;
      }
    }
    broadcast(doc, identity_id);
  }

  // Outbound progress event. `finished=true` makes the SPA swap the
  // in-flight progress bubble for the final delivered/sent/failed form.
  inline void publish_progress(const LXMF::IdentityId& identity_id,
                               const RNS::Bytes& peer_hash,
                               const RNS::Bytes& link_hash,
                               bool incoming,
                               uint32_t bytes_done,
                               uint32_t bytes_total,
                               bool finished) {
    Common::PsramJsonDocument doc;
    doc["type"]        = finished ? "message_complete" : "message_progress";
    doc["peer"]        = peer_hash.toHex();
    doc["link_hash"]   = link_hash.toHex();
    doc["incoming"]    = incoming;
    doc["bytes_done"]  = bytes_done;
    doc["bytes_total"] = bytes_total;
    doc["finished"]    = finished;
    broadcast(doc, identity_id);
  }

  // Outbox-status transition: queued → sent → delivered, or failed.
  // Used by the SPA to update the status pill next to an outbound bubble.
  inline void publish_outbox_status(const LXMF::IdentityId& identity_id,
                                    const RNS::Bytes& link_hash,
                                    const char* status_name) {
    Common::PsramJsonDocument doc;
    doc["type"]      = "outbox_status";
    doc["link_hash"] = link_hash.toHex();
    doc["status"]    = status_name;
    broadcast(doc, identity_id);
  }

  // Same event, keyed by outbox seq instead of a packet/link hash — for a
  // queued send that has no packet hash yet (the "finding route" give-up).
  // The SPA matches the bubble by seq.
  inline void publish_outbox_status_seq(const LXMF::IdentityId& identity_id,
                                        uint32_t seq,
                                        const char* status_name) {
    Common::PsramJsonDocument doc;
    doc["type"]   = "outbox_status";
    doc["seq"]    = seq;
    doc["status"] = status_name;
    broadcast(doc, identity_id);
  }

  // Announce / path events. These are global (every connected client
  // gets them) so the SPA's contacts list and path table stay live.
  // is_lxmf=true → "announce_seen" (lxmf.delivery aspect, populates
  // contacts), is_lxmf=false → "path_seen" (any aspect, populates the
  // path table).
  inline void publish_announce_or_path(const LXMF::AnnounceRecord& rec, bool is_lxmf) {
    Common::PsramJsonDocument doc;
    doc["type"]         = is_lxmf ? "announce_seen" : "path_seen";
    doc["dest"]         = rec.destination.toHex();
    doc["display_name"] = rec.display_name;
    doc["aspect"]       = rec.aspect;
    doc["received_ms"]  = rec.received_ms;
    doc["age_ms"]       = (uint32_t)(millis() - rec.received_ms);
    broadcast(doc);
  }

  // Multi-kind sensor update — one frame carrying every sensor that
  // produced a fresh reading in the last drain window. The `fill`
  // callback receives the `values` object; it adds one nested object
  // per kind (gps, environment, magnetometer, imu, ...) shaped the
  // same way /api/system_status sensors[kind] is. The SPA patches
  // each kind into its cached snapshot in turn.
  //
  // Shape: { type: "sensors_update", values: { gps: {...}, imu: {...} } }
  inline void publish_sensors_update(const std::function<void(JsonObject)>& fill) {
    Common::PsramJsonDocument doc;
    doc["type"] = "sensors_update";
    JsonObject values = doc["values"].to<JsonObject>();
    fill(values);
    broadcast(doc);
  }

  // Hook fired once per connected client right after auth succeeds,
  // before the `hello` frame is sent. Lets WebUI inject a fresh
  // snapshot of all sensors / clock / etc into the hello payload so
  // the SPA has live state from the very first frame.
  using HelloExtras = std::function<void(JsonObject /*hello*/)>;
  inline HelloExtras& hello_extras() { static HelloExtras fn; return fn; }
  inline void set_hello_extras(HelloExtras fn) { hello_extras() = std::move(fn); }

  // Time-source update — fires when the active clock source changes
  // (GPS lock acquired, NTP sync completed, browser-set, RTC seed).
  inline void publish_time(const char* source, uint64_t unix_ms, bool calibrated) {
    Common::PsramJsonDocument doc;
    doc["type"]               = "time_update";
    doc["source"]              = source;
    doc["unix_ms"]            = unix_ms;
    doc["calibrated"]         = calibrated;
    // Stamp the moment this event represents. Used by the SPA's
    // "Last calibrated Xs ago" label so the user sees a fresh "0s ago"
    // immediately after a source reports, without waiting for the
    // next hello.
    doc["now_ms"]             = (uint32_t)millis();
    doc["last_calibrated_ms"] = (uint32_t)millis();
    broadcast(doc);
  }

  // Generic system-block update — battery telemetry, storage usage,
  // outbound staging caps. The shape mirrors what /api/system_status
  // used to carry; the SPA replaces its cached block wholesale on
  // each event. Periodic (every 30 s from WebUI::loop) so the
  // popover stays live without polling.
  inline void publish_system(const std::function<void(JsonObject)>& fill) {
    Common::PsramJsonDocument doc;
    doc["type"] = "system_update";
    JsonObject root = doc["payload"].to<JsonObject>();
    fill(root);
    broadcast(doc);
  }

  // SD-presence transition. Edge-triggered from the main-loop poll
  // when sd_present flips. Carries the user-configured and the
  // currently-effective send/receive caps so the SPA can re-render
  // its Settings slider bounds + toast the user that the cap moved.
  inline void publish_storage(bool sd_present,
                              uint32_t user_max_send,
                              uint32_t user_max_receive,
                              uint32_t effective_max_send,
                              uint32_t effective_max_receive) {
    Common::PsramJsonDocument doc;
    doc["type"]                      = "storage_changed";
    doc["sd_present"]                = sd_present;
    doc["user_max_send_bytes"]       = user_max_send;
    doc["user_max_receive_bytes"]    = user_max_receive;
    doc["effective_max_send_bytes"]  = effective_max_send;
    doc["effective_max_recv_bytes"]  = effective_max_receive;
    broadcast(doc);
  }

  // Radio telemetry — 1 Hz live sample of RSSI / channel util / CSMA
  // state. Frame is intentionally small (~120 B JSON) because it ships
  // every second and shouldn't dominate WiFi/lwIP buffering. Fields are
  // emitted flat alongside `type` to match the wire shape of the other
  // light WS frames (announce_seen, path_seen, time_update, etc.) —
  // the heavier frames (incoming, system_update) wrap under a semantic
  // sub-object (msg / payload / value) because they carry the type
  // plus their own meta; telemetry carries no meta beyond the sample
  // itself, so the wrap was redundant.
  inline void publish_radio_telemetry(const Telemetry::Radio::Sample& s) {
    Common::PsramJsonDocument doc;
    doc["type"] = "radio_telemetry";
    Telemetry::Radio::encode(s, doc.as<JsonObject>());
    // Cumulative packet counters tagged onto the live frame (not the
    // history fill — those samples are frozen in time). Lets the
    // status popover update the "RX 686 / TX 662 pkt" line and the
    // chart simultaneously, without depending on the open-popover
    // /api/info refresh that previously gated those numbers.
    doc["rx_packets"] = (uint32_t)RNS::Transport::packets_received();
    doc["tx_packets"] = (uint32_t)RNS::Transport::packets_sent();
    // Outbound packets dropped because the LoRa TX ring was full — the
    // honest counterpart to tx_packets. Climbs when the radio can't keep
    // up (e.g. duty-cycle airtime lock holding the queue).
    doc["tx_dropped"] = (uint32_t)lora_tx_dropped;
    broadcast(doc);
  }

  // Network (WiFi/transport) telemetry — aggregate tx/rx byte rate across the
  // non-LoRa interfaces. Counterpart to the radio frame; carries no extra meta
  // beyond the sample itself.
  inline void publish_network_telemetry(const Telemetry::Network::Sample& s) {
    Common::PsramJsonDocument doc;
    doc["type"] = "network_telemetry";
    Telemetry::Network::encode(s, doc.as<JsonObject>());
    broadcast(doc);
  }

  // Identity-code edge event (button-press triggered).
  inline void publish_identity_code_available() {
    Common::PsramJsonDocument doc;
    doc["type"] = "identity_code_available";
    broadcast(doc);
  }

  // Pending-auth map: handshake runs BEFORE the WS client object
  // exists, so we stash the validated IdentityId keyed by the TCP
  // remote port and look it up in WS_EVT_CONNECT (same connection,
  // so the port is stable).
  struct PendingAuth {
    uint16_t         remote_port;
    LXMF::IdentityId identity_id;
    uint32_t         created_ms;
  };
  inline std::vector<PendingAuth>& pending_auths() {
    static std::vector<PendingAuth> v;
    return v;
  }

  // Handshake handler — returns true to allow the upgrade. Parses
  // ?token=... from the URL, validates against AuthTokens, and stashes
  // the resulting IdentityId so on_event can complete the binding.
  inline bool on_handshake(AsyncWebServerRequest* request) {
    if (!validator() || !request) return false;
    String token_hex;
    String id;
    if (request->hasParam("token", false))       token_hex = request->getParam("token", false)->value();
    if (request->hasParam("identity_id", false)) id        = request->getParam("identity_id", false)->value();
    if (token_hex.length() == 0) return false;
    LXMF::IdentityId tok_iden = validator()(std::string(token_hex.c_str()));
    if (tok_iden.empty()) return false;
    if (id.length() > 0 && tok_iden != std::string(id.c_str())) return false;
    PendingAuth pa;
    pa.remote_port = request->client() ? request->client()->remotePort() : 0;
    pa.identity_id = tok_iden;
    pa.created_ms  = millis();
    // GC entries older than 30s (handshake-to-CONNECT lag should be ms).
    auto& v = pending_auths();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [](const PendingAuth& p){ return millis() - p.created_ms > 30000; }),
            v.end());
    v.push_back(pa);
    return true;
  }

  // Internals — connect/disconnect/ping wiring. Auth is already done
  // in the handshake; here we just bind the IdentityId to the client.
  inline void on_event(AsyncWebSocket* /*srv*/, AsyncWebSocketClient* client,
                       AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
      ClientState st;
      st.client_id = client->id();
      st.authed    = false;
      // Look up the pending auth by TCP remote port — same socket the
      // handshake just ran on.
      const uint16_t rport = client->client() ? client->client()->remotePort() : 0;
      auto& v = pending_auths();
      for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->remote_port == rport) {
          st.authed      = true;
          st.identity_id = it->identity_id;
          v.erase(it);
          break;
        }
      }
      clients().push_back(st);
      if (!st.authed) {
        client->text("{\"type\":\"error\",\"error\":\"auth_required\"}");
        client->close(1008, "auth");
        return;
      }
      // Hello frame — gives the SPA the device time + identity it's
      // subscribed for, so it can drop any stale "connected as..." UI.
      // If WebUI installed an extras hook, let it inject a current
      // sensor / clock snapshot so the SPA has live data from frame
      // one rather than waiting for the first periodic push.
      Common::PsramJsonDocument hello;
      hello["type"]        = "hello";
      hello["identity_id"] = st.identity_id;
      hello["now_ms"]      = (uint32_t)millis();
      if (hello_extras()) {
        hello_extras()(hello.as<JsonObject>());
      }
      // Same SharedBuffer pattern as broadcast() — see comment there.
      // Hello frames are the largest periodic WS message (~2 KiB JSON
      // from hello_extras), and fire on every client connect, so they
      // belong on the same allocation path as the periodic broadcasts.
      const size_t n = measureJson(hello);
      if (n > 0) {
        AsyncWebSocketSharedBuffer buf;
        try {
          buf = std::make_shared<std::vector<uint8_t>>(n);
          serializeJson(hello, buf->data(), n);
          client->text(buf);
        }
        catch (const std::bad_alloc&) {
          // Fragmented default heap; client will reconnect on its own
          // policy. Better than crashing on the hello path.
        }
      }
    }
    else if (type == WS_EVT_DISCONNECT) {
      auto& v = clients();
      v.erase(std::remove_if(v.begin(), v.end(),
                             [&](const ClientState& c){ return c.client_id == client->id(); }),
              v.end());
    }
    else if (type == WS_EVT_DATA) {
      AwsFrameInfo* info = (AwsFrameInfo*)arg;
      if (!info || !info->final || info->opcode != WS_TEXT) return;
      // Tiny client→server protocol — just ping for keepalive.
      if (len == 0 || !data) return;
      std::string s((const char*)data, len);
      if (s.find("\"ping\"") != std::string::npos) {
        client->text("{\"type\":\"pong\"}");
      }
    }
  }

} // namespace WS
} // namespace Web
