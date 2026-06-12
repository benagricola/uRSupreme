#pragma once

// WebUI - HTTP/SSE bridge between browser SPAs and the LXMF gateway.
//
// Registers routes on the existing WebServer (declared in Console.h) so the
// same port-80 instance serves both the legacy console docs and our /api/*
// surface. The server is brought up by WebUI::start() once WiFi is up,
// independent of the legacy console-mode path. Both code paths share a
// `_started` guard so server.begin() is only called once.

#include <Arduino.h>
#include <EEPROM.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include "../Common/RnsLock.h"
#include "../Common/WifiTransition.h"   // shared APSTA-state types
#include "../Common/Status.h"           // Status::latest for /api/info
#include "../Common/HeapWatermark.h"    // window_low for /api/diag/mem
#if defined(URTN_HEAP_TRACE)
#include "../HeapTrace.h"               // /api/diag/heaptrace leak tracker
#endif

// Variables and constants owned by Remote.h's WiFi state machine.
// Remote.h is included after WebUI.h in the .ino translation unit,
// but the WiFi transition handlers below need to reference them, so
// we extern-declare them here.
extern WifiPhase        wifi_phase;
extern uint32_t         wifi_apsta_deauth_at_ms;
extern PendingProvision wr_pending;
extern char             wr_hostname[10];
// Timing constants come from Common/WifiTransition.h above.
#include <ChunkPrint.h>
#include <Log.h>
#include <Reticulum.h>
#include <Transport.h>
#include <SHA256.h>                     // streaming SHA-256 for optional upload verify
#include <deque>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include "../Clock/Manager.h"
#include "ApiRoutes.h"
#include "WebSocket.h"
#include "../Sensors/Position/Gnss.h"
#include "../Sensors/Clock/PCF8563.h"
#include "../Storage/SDCard.h"
#include "../Sensors/Environment/BME280.h"
#include "../Sensors/Compass/QMC6310.h"
#include "../Sensors/Motion/QMI8658.h"
#include "../Sensors/Config.h"
#include "../Storage/OutboundStaging.h"
#include "../Storage/Config.h"
#include "../Storage/Migration.h"
#include "../Telemetry/Battery.h"
#include "../Discovery/Config.h"
#include "../Discovery/State.h"
#include "../Discovery/Identity.h"
#include "../Discovery/Announcer.h"
#if HAS_WIFI && defined(TCP_TRANSPORT)
#include "../TCPTransport.h"
#endif

#include "../LXMF/LXMFGateway.h"
#include "../LXMF/LXMFTypes.h"
#include "../LXMF/TelemetrySender.h"
#include "../LXMF/TelemetryShare.h"
#include "../LXMF/Messenger.h"
#include "../LXMF/AnnounceLog.h"
#include "AuthTokens.h"
#include "BootCounter.h"
#include "PasswordHash.h"
#include "SPAEmbedded.h"

// EEPROM helpers + WiFi config constants from the existing firmware.
// Used by the bootstrap-mode WiFi configure endpoint.
#include "../Config.h"   // WR_WIFI_OFF/STA/AP, config_addr()
#include "../ROM.h"      // ADDR_CONF_SSID, ADDR_CONF_PSK

// The single Reticulum instance lives in RNode_Firmware.ino - pulled
// in here so persist_and_restart() can flush state before ESP.restart().
extern RNS::Reticulum reticulum;
extern void eeprom_update(int mapped_addr, uint8_t byte);
extern void wr_conf_save(uint8_t mode);
extern void wifi_remote_eeprom_write_sta_creds(const char* ssid, const char* psk);
extern bool eeprom_have_conf();
extern uint32_t lora_freq;
extern uint32_t lora_bw;
extern int      lora_sf;
extern uint32_t lora_bitrate;
extern int      lora_cr;
extern int      lora_txp;
extern bool     radio_online;
// Radio activity counters (defined in Config.h / RNode_Firmware.ino).
// Used to expose live transmit/receive stats so callers can confirm the
// radio is actually doing work rather than just reporting `online: true`.
extern uint32_t stat_rx;          // total packets received since boot
extern uint32_t stat_tx;          // total packets transmitted since boot
extern volatile uint32_t lora_tx_dropped;  // LoRa packets dropped: TX ring full
extern volatile uint32_t lora_tx_by_type[4]; // LoRa TX count by RNS packet type (0 DATA,1 ANNOUNCE,2 LINKREQUEST,3 PROOF)
extern volatile uint32_t tx_hold_bytes;    // LoRa TX flow-control: bytes held (not dropped)
extern volatile uint8_t  queue_height;     // LoRa TX ring: queued packet count
extern volatile uint16_t queued_bytes;     // LoRa TX ring: queued byte count
extern int      last_rssi;        // RSSI of the last received packet (dBm)
extern uint8_t  last_snr_raw;     // SNR of the last received packet (raw scale)
extern int      noise_floor;      // measured noise floor (dBm)
extern float    airtime;          // short-term channel utilisation as 0..1
extern float    longterm_airtime; // long-term (1h) channel utilisation as 0..1
extern float    st_airtime_limit; // short-term duty-cycle cap (0..1, 0=disabled)
extern float    lt_airtime_limit; // long-term duty-cycle cap (0..1, 0=disabled)
extern bool     airtime_lock;     // true when current airtime exceeds the cap; TX blocked
extern bool     kiss_serial_output;  // toggle KISS-framed bytes on USB UART
// Live OLED framebuffer copy for /api/diag/display. Defined in
// Display.h, which compiles after this file in the firmware TU.
extern bool oled_capture(uint8_t* out, size_t cap, uint16_t* w, uint16_t* h);

#include <algorithm>
#include <vector>
#include <string>

extern AsyncWebServer server;     // declared in Console.h
extern bool      wifi_initialized;
// WiFi state from Remote.h. Exposed so /api/info can surface the
// current connection mode + SSID and so the /api/wifi/softap handler
// can flip the runtime-switch flag without including Remote.h.
extern uint8_t   wifi_mode;
extern wl_status_t wr_wifi_status;
extern bool      wr_runtime_softap;
extern volatile bool wr_force_softap_pending;
extern char      bt_devname[];
extern char      wr_hostname[];
extern microStore::FileSystem filesystem;

namespace Web {

  // Single-use 6-hex-char identity code generated by a button press on
  // the device. Required for /api/identities (create), /api/auth/login,
  // /api/wifi/configure, /api/radio. Proves physical possession of the
  // device - same role as PSK-on-back-of-router but ephemeral.
  // Lives only in RAM; expires after IDENTITY_CODE_TTL_MS.
  struct IdentityCode {
    std::string  hex6;        // first 6 chars of a 16-byte random token
    uint32_t     expires_ms;  // millis() snapshot
    bool         consumed;
  };

  class WebUI {
  public:
    static constexpr uint32_t IDENTITY_CODE_TTL_MS = 60UL * 1000UL;

    // Set by the firmware setup() path when WiFi failed to come up in STA
    // mode and we fell back to softAP. Toggles /api/wifi/* into the
    // unauthenticated bootstrap configuration mode.
    static inline bool bootstrap_mode = false;

    // Called once at boot, after WiFi STA is up. Idempotent.
    static void start() {
      if (_started) return;
      _started = true;
      AuthTokens::load();
      // While the SPA sensors popover is open it sends {"type":
      // "sensor_live"} over the WS; honour it by polling those sensors
      // fast and draining at a faster cadence (the OLED screens use the
      // same per-sensor request_live demand).
      Web::WS::on_client_message() = [](const char* data, size_t len) {
        on_ws_client_message(data, len);
      };
      BootCounter::init();  // emit the log line; current() is otherwise lazy
      register_routes();
      server.begin();
      // Wire AnnounceLog → WebSocket. Every new announce / path now
      // pushes a typed frame to every connected client. is_lxmf flips
      // between announce_seen and path_seen on the wire.
      LXMF::AnnounceLog::on_new_announce(
        [](const LXMF::AnnounceRecord& rec, bool is_lxmf) {
          Web::WS::publish_announce_or_path(rec, is_lxmf);
        });
      // Wire TimeManager → WebSocket. Fires when a source (NTP, GPS,
      // browser, RTC seed) adopts a new wall-clock value, so the SPA
      // clock pill updates without polling /api/time.
      Clock::Manager::set_on_change(
        [](Clock::Manager::Source src, double epoch_s) {
          const uint64_t unix_ms = (uint64_t)(epoch_s * 1000.0);
          Web::WS::publish_time(Clock::Manager::source_name(src),
                                unix_ms, /*calibrated=*/true);
        });
      // Stuff a fresh sensor + clock snapshot into every `hello` frame
      // so a freshly-connected SPA has live data immediately - no
      // wait for the next periodic push, no fallback /api/system_status
      // round-trip.
      Web::WS::set_hello_extras([](JsonObject hello) {
        // Sensors + storage + battery + outbound_caps + rtc - the full
        // system snapshot fill_system_block also emits in system_update
        // events. The SPA replaces its cached snapshot wholesale on
        // both hello and system_update, so the shapes must match.
        fill_system_block(hello);
        JsonObject clock = hello["clock"].to<JsonObject>();
        clock["now_ms"]            = millis();
        clock["current_boot_epoch"] = Web::BootCounter::current();
        clock["unix_ms"]            = (uint64_t)(Clock::Manager::now_epoch() * 1000.0);
        clock["calibrated"]         = Clock::Manager::is_calibrated();
        clock["source"]             = Clock::Manager::source_name(Clock::Manager::current_source());
        // millis() of the most recent successful calibration. Pair
        // with the clock-pill `now_ms` anchor in the SPA to render
        // "Last calibrated Xs ago (SOURCE)".
        clock["last_calibrated_ms"] = Clock::Manager::last_calibrated_ms();
      });
      NOTICE("WebUI: listening on port 80");
    }

    // Main-loop hook. Token-expiry sweep + identity-code sweep. The
    // HTTP server itself runs in AsyncTCP's own task and doesn't need
    // a tick here.
    static void loop() {
      if (!_started) return;
      uint32_t now = millis();
      if (now - _last_sweep > 60000) {
        _last_sweep = now;
        if (acquire_rns_lock(50)) {
          AuthTokens::sweep_expired();
          if (id_code().expires_ms != 0 && now > id_code().expires_ms) {
            id_code().hex6.clear();
            id_code().expires_ms = 0;
            id_code().consumed = false;
          }
          release_rns_lock();
        }
      }
      // Deferred-reboot check - endpoints that change EEPROM (radio,
      // wifi, factory reset, etc.) schedule the reboot for ~5 s
      // after responding so the SPA can show "Saved, rebooting…"
      // rather than getting a TCP-drop error mid-flight.
      check_scheduled_reboot();

      // WiFi-provision response handoff - when handle_wifi_configure
      // parks a request waiting for STA to come up, this is where we
      // send the response (or a timeout error). Lives in the main
      // loop because the WiFi phase machine in Remote.h advances on
      // the same tick; doing it from a WebServer task would race.
      drain_wifi_provision_response();
      drain_upload_finalize();

      // SD ejection edge - verify_or_disable in the SD write paths
      // flips a flag when a card stops responding mid-session. Drain
      // it here so the SPA's Settings sliders + toast respond
      // immediately, without paying any cost on the steady-state
      // happy path.
      if (Storage::SDCard::take_eject_edge()) {
        const auto& sc = Storage::Config::current();
        Web::WS::publish_storage(
            false,
            (uint32_t)std::min<size_t>(sc.user_max_send_bytes,    0xFFFFFFFFu),
            (uint32_t)std::min<size_t>(sc.user_max_receive_bytes, 0xFFFFFFFFu),
            (uint32_t)std::min<size_t>(Storage::Config::effective_max_send(),    0xFFFFFFFFu),
            (uint32_t)std::min<size_t>(Storage::Config::effective_max_receive(), 0xFFFFFFFFu));
      }
      // Radio telemetry - sample at 1 Hz unconditionally so the ring is
      // pre-populated for any client that connects later. Only the WS
      // broadcast is gated on subscribers.
      if (now - _last_radio_tlm >= Telemetry::Radio::SAMPLE_PERIOD_MS) {
        _last_radio_tlm = now;
        const Telemetry::Radio::Sample* s = Telemetry::Radio::tick(now);
        if (s && Web::WS::any_subscribers()) {
          Web::WS::publish_radio_telemetry(*s);
        }
      }
      // Network telemetry - same 1 Hz cadence. Sum tx/rx bytes across the
      // non-LoRa interfaces (backbone TCP client, TCP server, UDP) under the
      // recursive rns_lock so the interface table isn't iterated while
      // reticulum.loop mutates it. Only advance/tick when the lock is
      // acquired, so a missed window doesn't corrupt the rate baseline.
      if (now - _last_net_tlm >= Telemetry::Network::SAMPLE_PERIOD_MS) {
        uint64_t tx_total = 0, rx_total = 0;
        if (acquire_rns_lock(50)) {
          for (const auto& kv : RNS::Transport::get_interfaces()) {
            if (kv.second.name() == "LoRaInterface") continue;
            tx_total += (uint64_t)kv.second.txb();
            rx_total += (uint64_t)kv.second.rxb();
          }
          release_rns_lock();
          _last_net_tlm = now;
          const Telemetry::Network::Sample* ns =
              Telemetry::Network::tick(now, tx_total, rx_total);
          if (ns && Web::WS::any_subscribers()) {
            Web::WS::publish_network_telemetry(*ns);
          }
        }
      }

      // Skip all WS-publish work when nobody's listening - WebUI::loop
      // runs at ~50 Hz on the main task (shared with reticulum.loop and
      // the radio modem), so any allocation here is hot-path cost.
      if (Web::WS::any_subscribers()) {
        // Coalesced sensor publish. Runs at most once per second (the
        // floor of the user-configurable per-sensor interval). Each
        // pass walks the sensor kinds; any that have a fresh reading
        // since the last drain land in a single multi-kind frame.
        // Nothing changes → nothing sent.
        const uint32_t drain_period =
            (now < _sensor_ws_live_until) ? SENSOR_DRAIN_LIVE_MS
                                          : SENSOR_DRAIN_PERIOD_MS;
        if (now - _last_sensor_drain >= drain_period) {
          _last_sensor_drain = now;
          drain_sensor_updates();
        }
        // Full system snapshot every 30 s - battery decay, storage
        // usage drift, outbound_caps shifts on SD insert/eject. Cheap
        // (one client typical; ~1 KB payload). Hello carries the
        // initial snapshot so the SPA has live data from frame zero.
        if (now - _last_system_push > 30000) {
          _last_system_push = now;
          Web::WS::publish_system([](JsonObject root) { fill_system_block(root); });
        }
      }
    }

    // Read the taken_ms of a sensor's most recent reading WITHOUT
    // building any JSON. Hot path - runs every WebUI::loop iteration
    // (~50 Hz), so we must not allocate. Returns 0 for kinds whose
    // last reading hasn't lit up yet.
    static uint32_t sensor_taken_ms(const char* kind) {
      if (strcmp(kind, "gps")          == 0) return Sensors::Gnss::last_fix().fix_received_ms;
      if (strcmp(kind, "environment")  == 0) return Sensors::BME280::last_reading().taken_ms;
      if (strcmp(kind, "magnetometer") == 0) return Sensors::QMC6310::last_reading().taken_ms;
      if (strcmp(kind, "imu")          == 0) return Sensors::QMI8658::last_reading().taken_ms;
      return 0;
    }
    // Walk every sensor kind, find the ones whose taken_ms advanced
    // since their last published value, and emit a single multi-kind
    // WS frame carrying all of them. Skips entirely when nothing
    // changed (no allocation on the no-op path - important because
    // this runs every SENSOR_DRAIN_PERIOD_MS while any client is
    // connected).
    // Honour a sensors-popover live demand: poll the I2C sensors fast
    // and renew a window during which the drain runs at SENSOR_DRAIN_
    // LIVE_MS. GPS is not demanded here - it runs on its own schedule.
    static void on_ws_client_message(const char* data, size_t len) {
      if (!data || len == 0) return;
      const std::string m(data, len);
      if (m.find("\"sensor_live\"") == std::string::npos) return;
      const bool on = m.find("\"on\":true") != std::string::npos
                   || m.find("\"on\": true") != std::string::npos;
      if (!on) { _sensor_ws_live_until = 0; return; }
      Sensors::BME280::request_live(SENSOR_WS_LIVE_TTL_MS);
      Sensors::QMC6310::request_live(SENSOR_WS_LIVE_TTL_MS);
      Sensors::QMI8658::request_live(SENSOR_WS_LIVE_TTL_MS);
      _sensor_ws_live_until = millis() + SENSOR_WS_LIVE_TTL_MS;
    }

    static void drain_sensor_updates() {
      static constexpr const char* KINDS[]  = { "gps", "environment", "magnetometer", "imu" };
      uint32_t* const last_pub[] = { &_last_pub_gps_ms, &_last_pub_bme_ms,
                                     &_last_pub_mag_ms, &_last_pub_imu_ms };
      // First pass: detect changes without allocating.
      bool any = false;
      uint32_t fresh[sizeof(KINDS)/sizeof(KINDS[0])] = {0};
      for (size_t i = 0; i < sizeof(KINDS)/sizeof(KINDS[0]); ++i) {
        const uint32_t taken = sensor_taken_ms(KINDS[i]);
        if (taken == 0 || taken == *last_pub[i]) continue;
        fresh[i] = taken;
        any = true;
      }
      if (!any) return;
      // Second pass: build the multi-kind frame.
      Web::WS::publish_sensors_update([&](JsonObject values) {
        for (size_t i = 0; i < sizeof(KINDS)/sizeof(KINDS[0]); ++i) {
          if (fresh[i] == 0) continue;
          *last_pub[i] = fresh[i];
          Common::PsramJsonDocument tmp;
          JsonObject root = tmp.to<JsonObject>();
          fill_sensor_block(root, KINDS[i]);
          JsonObject src = root[KINDS[i]].as<JsonObject>();
          JsonObject dst = values[KINDS[i]].to<JsonObject>();
          for (JsonPair kv : src) dst[kv.key()] = kv.value();
        }
      });
    }

    // RNS state mutex. Shared between the WebServer task (which calls
    // into LXMFGateway / Identity / Destination / Inbox / etc. when
    // handling requests) and the main loop (which runs the RNS+LXMF
    // event loop). RNS is NOT reentrant; without serialisation a request
    // arriving mid-flight while the main loop is mutating path tables
    // races and crashes the device. The mutex is recursive so a handler
    // that holds it can call any number of nested RNS helpers without
    // deadlocking itself. Implementation lives in Common/RnsLock.h so
    // non-web callers (Discovery, LXMF) can grab the same lock without
    // a circular dependency on Web/.
    static bool acquire_rns_lock(uint32_t timeout_ms = portMAX_DELAY) {
      return Common::RnsLock::acquire(timeout_ms);
    }
    static void release_rns_lock() {
      Common::RnsLock::release();
    }
    using RnsLockGuard = Common::RnsLock::Guard;

    // Flush in-memory RNS state to flash, then restart. Used by every
    // controlled-reboot path (WiFi save, radio save, radio reset,
    // factory reset). Without this, _known_destinations and the path
    // store only get flushed once per hour by Reticulum::jobs(), so
    // any announces learned since the last flush are lost on reboot.
    // The web_task already holds the rns_lock around
    // handler execution, so persist_data() runs under the lock too.
    static void persist_and_restart(uint32_t flush_ms = 500) {
      reticulum.persist_data();
      delay(flush_ms);
      ESP.restart();
    }

    // Defer the reboot a few seconds so the calling handler's HTTP
    // response goes back cleanly and the SPA can show "saved,
    // rebooting in Ns" instead of getting a connection-drop error
    // mid-flight. The check fires from WebUI::loop on the same task
    // that serves requests, so by the time it runs the response is
    // long since flushed. Idempotent - second call with a smaller
    // delay wins, second call with the same/larger delay is a no-op.
    static inline uint32_t _scheduled_reboot_at_ms = 0;
    static void schedule_reboot(uint32_t delay_ms = 5000) {
      const uint32_t target = millis() + delay_ms;
      if (_scheduled_reboot_at_ms == 0 || target < _scheduled_reboot_at_ms) {
        _scheduled_reboot_at_ms = target;
      }
    }
    static uint32_t scheduled_reboot_ms_remaining() {
      if (_scheduled_reboot_at_ms == 0) return 0;
      const uint32_t now = millis();
      return (now >= _scheduled_reboot_at_ms) ? 0
           : (_scheduled_reboot_at_ms - now);
    }
    static void check_scheduled_reboot() {
      if (_scheduled_reboot_at_ms == 0) return;
      if (millis() < _scheduled_reboot_at_ms) return;
      // Time's up - flush + reboot. Use persist_and_restart's
      // existing semantics so any pending RNS state lands on disk.
      persist_and_restart(100);
    }

    // Send a 200 JSON response that carries a reboot_in_ms hint, then
    // arm the deferred reboot. Lets the SPA show a countdown card
    // instead of getting a TCP-drop error mid-flight. The actual
    // reboot fires from WebUI::loop, well after the response has been
    // flushed.
    static void respond_and_reboot(AsyncWebServerRequest* req,
                                   JsonDocument& doc,
                                   uint32_t delay_ms = 5000) {
      doc["reboot_in_ms"] = delay_ms;
      send_json(req, 200, doc);
      schedule_reboot(delay_ms);
    }

    // AsyncWebServer drives requests from AsyncTCP's own task - no
    // need to spawn or pump a handler loop here. Handlers still take
    // the rns_lock themselves around any RNS access.
    static void start_task() {}

    // Sends the response to a handle_wifi_configure() request that has
    // been parked waiting for STA to come up. Three cases:
    //   - STA reached WL_CONNECTED → respond with sta_ip + hostname,
    //     then schedule the deauth so the response can flush before
    //     the AP-side socket dies.
    //   - The provision attempt timed out → respond with 504.
    //   - Neither yet → no-op, try again next tick.
    static void drain_wifi_provision_response() {
      if (!wr_pending.parked) return;
      if (wifi_phase == WifiPhase::ApStaGrace) {
        auto req = wr_pending.req.lock();   // empty if the client dropped
        wr_pending.req.reset();
        wr_pending.parked = false;
        if (req) {
          Common::PsramJsonDocument doc;
          doc["status"]   = "connected";
          doc["sta_ip"]   = WiFi.localIP().toString();
          doc["hostname"] = String(wr_hostname);
          doc["mdns_url"] = String("http://") + wr_hostname + ".local/";
          doc["sta_url"]  = String("http://") + WiFi.localIP().toString() + "/";
          send_json(req.get(), 200, doc);
        }
        // TCP-flush margin before deauthing AP clients (scheduled even when
        // the client vanished - the transition must still complete). The
        // pump in Remote.h fires the deauth when millis() catches up.
        wifi_apsta_deauth_at_ms = millis() + WR_APSTA_DEAUTH_DELAY;
      } else if (wifi_phase == WifiPhase::ApStaConnecting
                 && (millis() - wr_pending.requested_ms) >= WR_PROVISION_TIMEOUT_MS) {
        auto req = wr_pending.req.lock();
        wr_pending.req.reset();
        wr_pending.parked = false;
        if (req) send_error(req.get(), 504, "sta_timeout");
      }
    }

    // Called from the button handler in RNode_Firmware.ino when the user
    // short-presses the device's USR1 button. Generates a fresh 16-byte
    // random; the first 6 hex chars are the proof we'll demand on the next
    // auth-sensitive request. The full hex is printed to serial so the
    // operator can see it (OLED integration is a later step).
    static void on_button_request_identity_code() {
      RNS::Bytes b = RNS::Cryptography::random(16);
      std::string hex = b.toHex();
      id_code().hex6      = hex.substr(0, 6);
      id_code().expires_ms = millis() + IDENTITY_CODE_TTL_MS;
      id_code().consumed  = false;
      // Visible to the operator. Also fires an SSE notification so any
      // logged-in browser can prompt the user to enter the code.
      NOTICEF("WebUI: identity code = %s (60s) - type the first 6 chars into the browser",
              hex.c_str());
      Serial.print("\n\n==== IDENTITY CODE: ");
      Serial.print(id_code().hex6.c_str());
      Serial.println(" (valid 60s) ====\n");
      Web::WS::publish_identity_code_available();
    }

    // Display accessors so the OLED renderer can show the identity code
    // without coupling Display.h to the rest of the WebUI internals. Read
    // each frame; returns empty / 0 if no identity code is pending.
    static const std::string& identity_code_for_display() {
      static const std::string empty;
      auto& u = id_code();
      if (u.hex6.empty() || u.consumed) return empty;
      if (millis() > u.expires_ms) return empty;
      return u.hex6;
    }

    // Cancel a pending code from the device side (a button tap on the
    // code page). The web flow that requested it just sees the code
    // expire; nothing is consumed.
    static void dismiss_identity_code() {
      id_code().hex6.clear();
      id_code().consumed = false;
    }

    static uint32_t identity_code_remaining_ms() {
      auto& u = id_code();
      if (u.hex6.empty() || u.consumed) return 0;
      uint32_t now = millis();
      if (now > u.expires_ms) return 0;
      return u.expires_ms - now;
    }

    // Check that the given proof is currently valid. Single-use: marks the
    // unlock as consumed on success.
    static bool consume_identity_code(const std::string& proof) {
      if (proof.empty() || id_code().hex6.empty() || id_code().consumed) return false;
      if (millis() > id_code().expires_ms) return false;
      // Constant-time-ish comparison.
      if (proof.size() != id_code().hex6.size()) return false;
      uint8_t diff = 0;
      for (size_t i = 0; i < proof.size(); i++) diff |= proof[i] ^ id_code().hex6[i];
      if (diff != 0) return false;
      id_code().consumed = true;
      return true;
    }

    // Explain why an identity-code proof did or didn't work. Returns
    // nullptr on success (proof valid, code consumed). On failure returns
    // a human-readable string the caller can shove into the JSON error
    // response so the UI can show something better than just
    // "identity_code_required". Same single-use side-effect as
    // consume_identity_code on success.
    static const char* explain_identity_code_failure(const std::string& proof) {
      auto& u = id_code();
      const uint32_t now = millis();
      if (u.hex6.empty()) {
        return "No identity code is pending on this device. Press the device's BOOT button: long-press (~1s), release, then short-press within 2s. Read the 6 hex chars from the OLED or serial console.";
      }
      if (u.consumed) {
        return "The pending identity code was already used. Press the BOOT button again to generate a fresh one.";
      }
      if (now > u.expires_ms) {
        return "The identity code expired (60s lifetime). Press the BOOT button again to generate a fresh one.";
      }
      if (proof.empty()) {
        return "Identity code is required for this operation. Type the 6 hex chars currently displayed on the device.";
      }
      if (proof.size() != u.hex6.size()) {
        return "Identity code is the wrong length. It should be 6 hex characters.";
      }
      uint8_t diff = 0;
      for (size_t i = 0; i < proof.size(); i++) diff |= proof[i] ^ u.hex6[i];
      if (diff != 0) {
        return "Identity code didn't match. Check you've read it from the correct device's OLED.";
      }
      u.consumed = true;
      return nullptr;
    }

  private:
    // ---- Helpers ----

    static IdentityCode& id_code() {
      static IdentityCode u{"", 0, false};
      return u;
    }

    // Streams the JSON document to the client via beginChunkedResponse +
    // ChunkPrint, never materialising the full serialised JSON as an
    // Arduino String. Required because String allocates from the default
    // heap (internal SRAM); for responses > ~16 KiB the realloc hits the
    // largest-contiguous-block ceiling and partway-fails, producing
    // garbled output where later field values overwrite earlier bytes
    // (observed: outbox JSON with 50K body returned `"ts":1.AAA...`).
    //
    // The filler closure owns the JsonDocument via shared_ptr because
    // the original lives on the handler's stack which returns before
    // any bytes are sent. serializeJson runs once per TCP chunk -
    // ChunkPrint discards the first `index` bytes (already sent) and
    // writes up to `maxLen` into the buffer. CPU cost is O(N) per chunk,
    // O(N²/chunkSize) total; for a 50 KiB response on a 240 MHz ESP32-S3
    // that's ~50 ms - acceptable given the device is memory-poor,
    // CPU-rich.
    //
    // Takes `doc` by non-const lvalue ref and std::move's into the
    // shared_ptr: zero-copy transfer of the ArduinoJson internal pool.
    // Callers MUST NOT use `doc` after send_json - its pool is empty.
    static void send_json(AsyncWebServerRequest* req, int code, JsonDocument& doc) {
      auto holder = std::make_shared<JsonDocument>(std::move(doc));
      AsyncWebServerResponse* response = req->beginChunkedResponse(
        "application/json",
        [holder](uint8_t* buf, size_t maxLen, size_t index) -> size_t {
          ChunkPrint dest(buf, index, maxLen);
          serializeJson(*holder, dest);
          return dest.written();
        });
      response->setCode(code);
      req->send(response);
    }

    static void send_error(AsyncWebServerRequest* req, int code, const char* msg) {
      Common::PsramJsonDocument doc;
      doc["error"]   = msg;
      send_json(req, code, doc);
    }

    // Same shape as send_error but also includes a free-text `message`
    // field the SPA can display directly to the user.
    static void send_error_with_message(AsyncWebServerRequest* req, int code,
                                        const char* err, const char* msg) {
      Common::PsramJsonDocument doc;
      doc["error"]   = err;
      doc["message"] = msg;
      send_json(req, code, doc);
    }

    // Extract bearer-token identity from the Authorization header. Falls
    // back to a `token` query-string param so EventSource (which can't set
    // headers) can authenticate. Sends 401 and returns empty on failure.
    // /api/info gets the lightweight battery summary - what the topbar
    // icon needs to render its glyph (percent + charge state).
    static void emit_battery_summary(JsonDocument& doc) {
      const auto b = Telemetry::Battery::current();
      if (!b.pmu_present) return;
      JsonObject bo = doc["battery"].to<JsonObject>();
      bo["percent"] = b.percent;
      bo["state"]   = Telemetry::Battery::state_name(b.state);
    }

    // Detailed battery telemetry (voltage, slope, USB rail, PMU temp,
    // discharge current) is built by fill_system_block and shipped
    // over WS - the `hello` snapshot + 30 s `system_update` cadence
    // is enough for human-eye monitoring of slow-changing power state.

    static LXMF::IdentityId require_auth(AsyncWebServerRequest* req) {
      std::string token;
      if (req->hasHeader("Authorization")) {
        String h = req->header("Authorization");
        if (h.startsWith("Bearer ")) {
          String hex_str = h.substring(7);
          hex_str.trim();
          token = std::string(hex_str.c_str());
        }
      }
      if (token.empty() && req->hasArg("token")) {
        token = std::string(req->arg("token").c_str());
      }
      if (token.empty()) {
        send_error(req, 401, "missing_bearer");
        return {};
      }
      LXMF::IdentityId acc = AuthTokens::validate(token);
      if (acc.empty()) {
        send_error(req, 401, "invalid_or_expired_token");
      }
      return acc;
    }

    static RNS::Bytes hex_to_bytes(const std::string& hex, size_t expected = 0) {
      RNS::Bytes b;
      if (hex.empty()) return b;
      if (expected > 0 && hex.size() != expected * 2) return b;
      b.assignHex(hex.c_str());
      return b;
    }

    // ---- Routes ----

    // Build an AsyncURIMatcher from a path containing `{}` placeholders
    // (legacy syntax inherited from WebServer.h's UriBraces). Each `{}`
    // becomes `([^/]+)` in the compiled regex; the resulting matcher
    // exposes the captures via request->pathArg(N). Paths without `{}`
    // get the matcher's default Exact behaviour for free.
    static AsyncURIMatcher uri(const String& path) {
      // Plain string paths must match exactly. The library's default
      // matcher (Type::BackwardCompatible) treats a registered path as
      // a prefix - e.g. POST /api/identities would also match
      // /api/identities/<id>/settings and steal that route. Force exact.
      if (path.indexOf("{}") < 0) return AsyncURIMatcher::exact(path);
      String pattern = "^";
      for (size_t i = 0; i < path.length(); ++i) {
        const char c = path[i];
        if (c == '{' && i + 1 < path.length() && path[i + 1] == '}') {
          pattern += "([^/]+)";
          ++i;
        } else if (c == '.' || c == '/' || c == '^' || c == '$'
                || c == '?' || c == '*' || c == '+' || c == '\\'
                || c == '(' || c == ')' || c == '[' || c == ']') {
          pattern += '\\';
          pattern += c;
        } else {
          pattern += c;
        }
      }
      pattern += "$";
      return AsyncURIMatcher::regex(pattern);
    }

    // Helper: register a JSON-body POST route. AsyncCallbackJsonWebHandler
    // collects the body, parses it, then invokes the handler with the
    // parsed JsonVariant.
    // AsyncCallbackJsonWebHandler defaults to a 16 KiB request-body
    // cap, which silently truncates anything bigger and surfaces as a
    // confusing parse failure on the client. `max_content_length`
    // overrides it for routes that take long JSON bodies (notably
    // /send, where the user's message text + inline metadata can run
    // well past 16 KiB). 0 leaves the library default in place.
    static void on_json_post(const char* path,
                             void (*fn)(AsyncWebServerRequest*, JsonVariant&),
                             size_t max_content_length = 0) {
      route_registry().push_back({"POST", path});
      auto* h = new AsyncCallbackJsonWebHandler(uri(path), fn);
      h->setMethod(HTTP_POST);
      if (max_content_length > 0) h->setMaxContentLength((int)max_content_length);
      server.addHandler(h);
    }

    // ---- Route registration goes through these wrappers so the device
    // can report what it actually serves (GET /api/diag/routes). The
    // wrappers preserve the underlying matcher semantics exactly:
    // on_http keeps the library's prefix matching for plain paths
    // (registration order still matters; see /api/radio/telemetry),
    // on_uri keeps the exact/param matcher. Paths come from
    // ApiRoutes:: constants only (api_routes.def is the single source
    // of truth; tools/check_api_parity.py bans raw literals).
    struct RouteRec { const char* method; const char* path; };
    static std::vector<RouteRec>& route_registry() {
      static std::vector<RouteRec> v;
      return v;
    }
    static const char* method_name(WebRequestMethodComposite m) {
      // if-chain, not switch: WebRequestMethodComposite is not an
      // integer type in this ESPAsyncWebServer version.
      if (m == HTTP_GET)    return "GET";
      if (m == HTTP_POST)   return "POST";
      if (m == HTTP_DELETE) return "DELETE";
      return "ANY";
    }
    static void on_http(WebRequestMethodComposite m, const char* path,
                        ArRequestHandlerFunction h,
                        ArUploadHandlerFunction upload = nullptr) {
      route_registry().push_back({method_name(m), path});
      if (upload) server.on(path, m, h, upload);
      else        server.on(path, m, h);
    }
    static void on_uri(WebRequestMethodComposite m, const char* path,
                       ArRequestHandlerFunction h) {
      route_registry().push_back({method_name(m), path});
      server.on(uri(path), m, h);
    }

    // GET /api/diag/routes - every route the running firmware has
    // registered, for parity checks against api_routes.def (the rig
    // suite asserts the two match; build-flag-gated ROUTE_OPT entries
    // may be absent here).
    static void handle_diag_routes(AsyncWebServerRequest* req) {
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["routes"].to<JsonArray>();
      for (const auto& r : route_registry()) {
        JsonObject o = arr.add<JsonObject>();
        o["m"] = r.method;
        o["p"] = r.path;
      }
      send_json(req, 200, doc);
    }

    static void register_routes() {
      // WebSocket event channel at /api/ws. Single push channel for
      // every realtime event the SPA cares about. The old SSE short-
      // poll lived here too; deleted now that the SPA is WS-only.
      Web::WS::bind_validator(&AuthTokens::validate);
      Web::WS::server().onEvent(Web::WS::on_event);
      Web::WS::server().handleHandshake(Web::WS::on_handshake);
      server.addHandler(&Web::WS::server());
      route_registry().push_back({"WS", ApiRoutes::WS});
      // SPA - single embedded HTML file, served gzipped at / and /index.html
      server.on("/",              HTTP_GET, handle_spa);
      server.on("/index.html",    HTTP_GET, handle_spa);
      server.on("/styles.css",    HTTP_GET, handle_styles_css);
      server.on("/alpine.min.js", HTTP_GET, handle_alpine_js);
      // Public
      on_http(HTTP_GET, ApiRoutes::INFO, handle_info);
      // Diagnostics (bearer-gated). GET reads heap headroom; POST resets
      // the per-window low-water marker (Common::HeapWatermark).
      on_http(HTTP_GET, ApiRoutes::DIAG_MEM, handle_diag_mem);
      on_json_post(ApiRoutes::DIAG_MEM,     handle_diag_mem_reset);
      on_http(HTTP_GET, ApiRoutes::DIAG_STORAGE, handle_diag_storage);
#if defined(URTN_LOOP_DIAG)
      on_http(HTTP_GET, ApiRoutes::DIAG_LOOP, handle_diag_loop);
      on_json_post(ApiRoutes::DIAG_LOOP,    handle_diag_loop_reset);
#endif
      on_http(HTTP_GET, ApiRoutes::DIAG_TRANSPORT, handle_diag_transport);
      on_http(HTTP_GET, ApiRoutes::DIAG_ROUTES, handle_diag_routes);
      // Live OLED framebuffer + messenger nav injection (testing).
      on_http(HTTP_GET, ApiRoutes::DIAG_DISPLAY, handle_diag_display_get);
      on_json_post(ApiRoutes::DIAG_DISPLAY,     handle_diag_display_post);
#if defined(URTN_HEAP_TRACE)
      on_http(HTTP_GET, ApiRoutes::DIAG_HEAPTRACE, handle_diag_heaptrace);
#endif
      // Auth
      on_json_post(ApiRoutes::AUTH_LOGIN,   handle_login);
      on_http(HTTP_POST, ApiRoutes::AUTH_LOGOUT, handle_logout);
      // Identities
      on_json_post(ApiRoutes::IDENTITIES,  handle_create_identity);
      // Session-scoped identity endpoints. The identity is taken from the
      // bearer token (require_auth), so these carry no {id} in the path - the
      // old /api/identities/{id}/* form was redundant (the handler rejected
      // any id != the token's identity anyway).
      on_http(HTTP_GET, ApiRoutes::IDENTITY, handle_get_identity);
      on_http(HTTP_DELETE, ApiRoutes::IDENTITY, handle_delete_identity);
      on_http(HTTP_POST, ApiRoutes::IDENTITY_DELETE, handle_delete_identity);
      on_http(HTTP_POST, ApiRoutes::ANNOUNCE, handle_announce);
      on_json_post(ApiRoutes::IDENTITY_SETTINGS, handle_identity_settings);
      // Announces (recent LXMF endpoint announces seen by the device)
      on_http(HTTP_GET, ApiRoutes::ANNOUNCES, handle_announces);
      // System - full wipe, gated by identity_code (physical presence).
      // Only path to recovery if every identity password is forgotten.
      on_json_post(ApiRoutes::SYSTEM_FACTORY_RESET, handle_factory_reset);
      on_json_post(ApiRoutes::SYSTEM_TRANSPORT,     handle_transport_toggle);
      on_json_post(ApiRoutes::SYSTEM_KISS,          handle_kiss_toggle);
      on_json_post(ApiRoutes::SYSTEM_REBOOT,        handle_reboot);
      // Time management. GET returns the current calibrated time +
      // source-priority config; POST /api/time {unix_ms} adopts a
      // browser-supplied time; POST /api/time/sources {sources:{...}}
      // writes the source config. All bearer-auth-gated.
      on_http(HTTP_GET, ApiRoutes::TIME, handle_time_get);
      on_json_post(ApiRoutes::TIME,                 handle_time_set);
      on_json_post(ApiRoutes::TIME_SOURCES,         handle_time_sources_set);
      // GPS fix - last RMC sentence parsed.
      on_http(HTTP_GET, ApiRoutes::GPS, handle_gps_get);
      // RTC diagnostics - raw chip state.
      on_http(HTTP_GET, ApiRoutes::RTC, handle_rtc_get);
      // Aggregated device status: storage + clock + sensors.
      // /api/system_status retired - `system_update` WS events carry
      // the same payload, with the initial snapshot in the `hello` frame.
      // Per-sensor enable + polling-interval overrides.
      on_json_post(ApiRoutes::SENSORS_CONFIG,       handle_sensors_config_post);
      // Telemetry-to-collector config + manual send trigger.
      on_http(HTTP_GET, ApiRoutes::TELEMETRY_CONFIG, handle_telemetry_config_get);
      on_json_post(ApiRoutes::TELEMETRY_CONFIG,     handle_telemetry_config_post);
      on_json_post(ApiRoutes::TELEMETRY_SEND,       handle_telemetry_send);
      on_http(HTTP_GET, ApiRoutes::TELEMETRY_SHARES, handle_telemetry_shares_get);
      on_json_post(ApiRoutes::TELEMETRY_SHARES_STOP, handle_telemetry_shares_stop);
      // OLED messenger presets.
      on_http(HTTP_GET, ApiRoutes::MESSENGER_PRESETS, handle_messenger_presets_get);
      on_json_post(ApiRoutes::MESSENGER_PRESETS,   handle_messenger_presets_post);
      // Global inbox capacity + wall-clock TTL pruning.
      on_http(HTTP_GET, ApiRoutes::INBOX_CONFIG, handle_inbox_config_get);
      on_json_post(ApiRoutes::INBOX_CONFIG,         handle_inbox_config_post);
      // Per-direction transfer caps (max_send / max_receive bytes).
      on_http(HTTP_GET, ApiRoutes::STORAGE_CONFIG, handle_storage_config_get);
      on_json_post(ApiRoutes::STORAGE_CONFIG,       handle_storage_config_post);
      // Streaming outbound attachment upload - PSRAM/SD-backed staging
      // that the /send path consumes by id. The body is multipart/
      // form-data with one file field; the X-Total-Length header tells
      // us how much to allocate up front.
      on_http(HTTP_POST, ApiRoutes::ATTACHMENT_UPLOAD,
                handle_outbound_upload_final,
                handle_outbound_upload_chunk);
      on_http(HTTP_GET, ApiRoutes::INBOX, handle_inbox);
      on_http(HTTP_GET, ApiRoutes::OUTBOX, handle_outbox);
      // /send takes the user's whole message body inline (text + emoji
      // + paste markdown). 16 KiB is plenty for chat but trips up on
      // realistic long-form content (e.g. logs pasted into a message).
      // 512 KiB safely fits in PSRAM and still leaves attachments to
      // ride the dedicated multipart staging-upload path for anything
      // bigger.
      on_json_post(ApiRoutes::SEND, handle_send,
                   /*max_content_length=*/512 * 1024);
      // POST /api/outbox/{seq}/retry - manually re-queue a Failed outbox
      // entry. Resets the auto-retry budget.
      on_uri(HTTP_POST, ApiRoutes::OUTBOX_RETRY, handle_outbox_retry);
      on_http(HTTP_GET, ApiRoutes::STATE, handle_state);
      on_uri(HTTP_DELETE, ApiRoutes::CONVERSATION, handle_clear_conversation);
      on_uri(HTTP_GET, ApiRoutes::CONVERSATION_CONFIG, handle_conversation_config_get);
      on_json_post(ApiRoutes::CONVERSATION_CONFIG,
                   handle_conversation_config_post);
      on_uri(HTTP_GET, ApiRoutes::ATTACHMENT_DOWNLOAD, handle_attachment_get);
      on_http(HTTP_POST, ApiRoutes::STORAGE_MIGRATE, handle_migrate_flash_to_sd);
      // Paths - per-destination lookup + transmit-ETA estimate.
      // `uri()` forces an exact match on /estimate. Without it the
      // plain-string form is Type::BackwardCompatible, which is a prefix.
      on_json_post(ApiRoutes::PATHS_LOOKUP,  handle_path_lookup);
      on_uri(HTTP_GET, ApiRoutes::PATHS_ESTIMATE, handle_path_estimate);
      // WiFi config - gated by bearer token OR identity_code in body.
      // Reboots on save.
      on_http(HTTP_GET, ApiRoutes::WIFI_SCAN, handle_wifi_scan);
      on_json_post(ApiRoutes::WIFI_CONFIGURE, handle_wifi_configure);
      on_json_post(ApiRoutes::WIFI_SOFTAP,    handle_wifi_force_softap);
      on_http(HTTP_GET, ApiRoutes::WIFI_SAVED, handle_wifi_saved_list);
      on_json_post(ApiRoutes::WIFI_FORGET,    handle_wifi_forget);
      // Radio config - read requires bearer auth; write/reset require
      // bearer OR identity_code. Both write paths reboot on success.
      // Telemetry history - auth-gated. Returns the rolling ring of 1Hz
      // samples (oldest→newest) so a freshly-connected SPA client can
      // backfill its chart before the WS push catches up.
      // REGISTERED BEFORE /api/radio because AsyncWebServer's plain
      // string matcher does prefix matching, so /api/radio would
      // otherwise swallow the more-specific /api/radio/telemetry.
      on_http(HTTP_GET, ApiRoutes::RADIO_TELEMETRY, handle_radio_telemetry);
      on_http(HTTP_GET, ApiRoutes::NETWORK_TELEMETRY, handle_network_telemetry);
      on_http(HTTP_GET, ApiRoutes::RADIO, handle_radio_get);
      on_json_post(ApiRoutes::RADIO,         handle_radio_set);
      on_json_post(ApiRoutes::RADIO_RESET,   handle_radio_reset);
      on_json_post(ApiRoutes::RADIO_AIRTIME, handle_radio_airtime);
      // TCP-client endpoint CRUD. Registered only when the build
      // actually includes the TCP transport - otherwise the routes
      // don't exist at all, and the SPA learns that from the
      // transports.tcp_client flag on /api/info.
      // List + add are bearer-only; per-row PATCH requires the
      // identity code only when enabling discovery on the entry.
      // DELETE is bearer-only.
#if HAS_WIFI && defined(TCP_TRANSPORT)
      on_http(HTTP_GET, ApiRoutes::TCP_CLIENTS, handle_tcp_clients_list);
      on_json_post(ApiRoutes::TCP_CLIENTS, handle_tcp_clients_add);
      on_uri(HTTP_DELETE, ApiRoutes::TCP_CLIENT, handle_tcp_clients_remove);
      on_json_post(ApiRoutes::TCP_CLIENT_PATCH, handle_tcp_clients_patch);
#endif
      // Discovery master state (toggle, default interval, default stamp
      // cost) + persistent network identity. Enabling the master toggle
      // requires identity-code physical presence; reads + disable are
      // bearer-only.
      on_http(HTTP_GET, ApiRoutes::DISCOVERY_STATE, handle_discovery_state_get);
      on_json_post(ApiRoutes::DISCOVERY_STATE,                  handle_discovery_state_set);
      on_http(HTTP_GET, ApiRoutes::DISCOVERY_IDENTITY, handle_discovery_identity_get);
      // Per-built-in-interface discoverable toggle. The LoRa interface
      // is always present on supreme; this endpoint reads/writes its
      // Discovery::Config entry. Enabling requires identity-code; the
      // disable / read paths are bearer-only.
      on_http(HTTP_GET, ApiRoutes::LORA_DISCOVERABLE, handle_lora_discoverable_get);
      on_json_post(ApiRoutes::LORA_DISCOVERABLE,         handle_lora_discoverable_set);
      // Full LoRa interface config: mode + IFAC (+ discoverable). Mode and
      // IFAC apply on reboot. Read is bearer-only; enabling IFAC/discovery
      // requires identity-code physical presence (enforced in the handler).
      on_http(HTTP_GET, ApiRoutes::LORA_CONFIG, handle_lora_config_get);
      on_json_post(ApiRoutes::LORA_CONFIG,               handle_lora_config_set);
      // UDP interface config: mode + IFAC (+ discoverable), same shape as
      // /lora/config. Lets the WiFi-UDP segment be made private without
      // hand-editing the interfaces.json file.
      on_http(HTTP_GET, ApiRoutes::UDP_CONFIG, handle_udp_config_get);
      on_json_post(ApiRoutes::UDP_CONFIG,                handle_udp_config_set);
    }


    // ============================== HANDLER MODULES ==============================
    // The ~70 HTTP handlers used to live inline in this file; they have
    // been split by domain into Web/WebUI/<domain>.h files for navigability.
    // Each file is #include'd from here, inside the class body of WebUI,
    // so its static method definitions stay implicit-inline and there is
    // no ODR risk. The files have no include guards and must not be
    // included elsewhere.
    #include "WebUI/helpers.h"
    #include "WebUI/static_assets.h"
    #include "WebUI/identity.h"
    #include "WebUI/diag.h"
    #include "WebUI/system.h"
    #include "WebUI/time_gps.h"
    #include "WebUI/config_storage.h"
    #include "WebUI/telemetry.h"
    #include "WebUI/messenger.h"
    #include "WebUI/wifi.h"
    #include "WebUI/radio.h"
    #include "WebUI/discovery.h"
    #include "WebUI/tcp_clients.h"
    #include "WebUI/paths.h"
    #include "WebUI/conversations.h"
    #include "WebUI/state.h"

  private:
    static inline bool     _started = false;
    static inline uint32_t _last_sweep = 0;
    // Drain cadence for the multi-kind sensors_update WS frame.
    // Matches the floor of the per-sensor interval the user can pick
    // in the SPA, so the SPA never sees a fresh sensor reading sit
    // for longer than one drain period before getting through to it.
    static constexpr uint32_t SENSOR_DRAIN_PERIOD_MS = 1000;
    // Faster drain + sensor TTL while a client demands live data.
    static constexpr uint32_t SENSOR_DRAIN_LIVE_MS  = 250;
    static constexpr uint32_t SENSOR_WS_LIVE_TTL_MS = 3000;
    static inline uint32_t _sensor_ws_live_until = 0;

    // Per-sensor `taken_ms` snapshots, used by drain_sensor_updates
    // to dedupe WS broadcasts. Reading a sensor twice per second when
    // it hasn't pumped is fine - the underlying Bme280::last_reading
    // etc are cached; we just don't want to flood the WS channel.
    static inline uint32_t _last_pub_gps_ms   = 0;
    static inline uint32_t _last_pub_bme_ms   = 0;
    static inline uint32_t _last_pub_mag_ms   = 0;
    static inline uint32_t _last_pub_imu_ms   = 0;
    static inline uint32_t _last_sensor_drain = 0;
    static inline uint32_t _last_system_push  = 0;
    static inline uint32_t _last_radio_tlm    = 0;
    static inline uint32_t _last_net_tlm      = 0;
  };

  // Free function the LXMF gateway calls to publish a Resource-transfer
  // progress event. Implemented here as inline so it's visible everywhere
  // WebUI.h is included; declared as a free function (not a static member)
  // so LXMFGateway.h - which is *included by* WebUI.h - can forward-
  // declare and call it without re-including WebUI.h (which would loop).
  inline void publish_lxmf_progress(const LXMF::IdentityId& identity_id,
                                    const RNS::Bytes& peer_hash,
                                    const RNS::Bytes& link_hash,
                                    bool incoming,
                                    uint32_t bytes_done,
                                    uint32_t bytes_total,
                                    bool finished) {
    Web::WS::publish_progress(identity_id, peer_hash, link_hash,
                              incoming, bytes_done, bytes_total, finished);
  }

}
