// Copyright (C) 2024, Mark Qvist

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <WiFi.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>           // esp_wifi_deauth_sta — drops AP clients
                                // on the WIFI layer, not just at HTTP.
#if defined(UDP_TRANSPORT)
#include <WiFiUdp.h>
#include <Bytes.h>
#endif

// Shared WiFi transition types — both this file and Web/WebUI.h
// reference WifiPhase / PendingProvision, so the types live in
// Common/ and neither side has to pull the other's full header.
#include "Common/WifiTransition.h"
#include "Common/Status.h"      // Status::say for WiFi countdown / lifecycle

#if CONFIG_IDF_TARGET_ESP32
  #include "esp32/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32S2
  #include "esp32s2/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32C3
  #include "esp32c3/rom/rtc.h"
#elif CONFIG_IDF_TARGET_ESP32S3
  #include "esp32s3/rom/rtc.h"
#else 
  #error Target CONFIG_IDF_TARGET is not supported
#endif

#define WIFI_UPDATE_INTERVAL_MS 500
#define WR_SOCKET_TIMEOUT 6
#define WR_READ_TIMEOUT_MS 6500
#define WR_RECONNECT_INTERVAL_MS 10000

uint32_t wifi_update_interval_ms = WIFI_UPDATE_INTERVAL_MS;
uint32_t last_wifi_update = 0;
uint32_t wr_last_connect_try = 0;
uint32_t wr_last_read = 0;

WiFiClient connection;
WiFiServer remote_listener(7633, 1);
IPAddress ap_ip(10, 0, 0, 1);
IPAddress ap_nm(255, 255, 255, 0);
IPAddress wr_device_ip;
char wr_hostname[10];
wl_status_t wr_wifi_status = WL_IDLE_STATUS;
#if defined(UDP_TRANSPORT)
WiFiUDP udp;
RNS::Bytes udp_buffer;
#if defined(HAS_RNS)
RNS::Interface udp_interface(RNS::Type::NONE);
#endif
#endif

uint8_t wifi_mode = WIFI_OFF;
bool wifi_init_ran = false;
bool wifi_initialized = false;
// Tracks the millis() value of the first STA-mode init attempt this
// boot. Drives the "no AP at boot, promote to APSTA after 3 min if
// STA hasn't connected" recovery policy — see WR_STA_BEFORE_AP_DELAY_MS
// in wifi_update_status(). Cleared on first successful STA-connect
// so subsequent runtime drops never re-arm AP; only a reboot can
// re-enter the AP-recovery window.
uint32_t wr_sta_first_attempt_ms = 0;
bool wr_sta_fallback_armed = false;
// Set by the web handler when the user wants to switch out of STA
// mode without rebooting. The main-loop wifi pump notices this on its
// next tick and calls wifi_remote_init() in AP mode. RAM-only: a
// reboot will go back to whatever EEPROM says, which is exactly what
// you want for a "temporary softAP" toggle.
volatile bool wr_force_softap_pending = false;
// True while we're in the runtime-fallback softAP (either because the
// 30 s timer fired or because the web handler asked for it). Cleared
// by wifi_remote_init() so a deliberate STA reconnect from the
// bootstrap UI gets a clean slate. The SPA reads this via /api/info
// to decide whether to surface the "switch to softAP" button.
bool wr_runtime_softap = false;

// =================== APSTA TRANSITION STATE ===================
// On boot with saved STA creds, and again whenever the softAP web
// handler accepts new credentials, the device sits in APSTA mode for
// a bounded window:
//
//   - ApStaConnecting: STA is associating. AP is live as a recovery
//                      channel; HTTP request (if any) is parked.
//   - ApStaGrace:      STA reached WL_CONNECTED. We respond to the
//                      parked request, then ~1 s later deauth all AP
//                      clients (force their devices to reconnect to
//                      their original WiFi), then 2 min later tear
//                      the AP interface down entirely.
//
// Once teardown completes we're STA-only and don't re-arm AP on STA
// drops — see the policy note in handle_wifi_configure. The only ways
// back to AP at that point are a reboot (boot path may go AP if STA
// fails) or an authenticated /api/wifi/softap call (which already
// requires the identity-code physical-presence proof).
// WifiPhase + PendingProvision come from Common/WifiTransition.h —
// included near the top of this file so the variable definitions
// below resolve.
WifiPhase wifi_phase                 = WifiPhase::Idle;
uint32_t  wifi_phase_started_ms      = 0;     // when we entered the current phase
uint32_t  wifi_apsta_deauth_at_ms    = 0;     // 0 = not scheduled
bool      wifi_apsta_deauth_done     = false; // one-shot for the current grace
PendingProvision wr_pending;

// WR_PROVISION_TIMEOUT_MS, WR_APSTA_DEAUTH_DELAY, WR_APSTA_GRACE_MS
// live in Common/WifiTransition.h so both this file and Web/WebUI.h
// agree on the timings.

char wr_ssid[33];
char wr_psk[33];

extern void host_disconnected();

void wifi_dbg(String msg) { Serial.print("[WiFi] "); Serial.println(msg); }

uint8_t wifi_remote_mode() { return wifi_mode; }

bool wifi_is_connected() { return (wr_wifi_status == WL_CONNECTED); }
bool wifi_host_is_connected() { if (connection) { return true; } else { return false; } }

void wifi_remote_start_ap() {
  WiFi.mode(WIFI_AP);
  if (wr_ssid[0] != 0x00) {
    if (wr_psk[0] != 0x00) { WiFi.softAP(wr_ssid, wr_psk, wr_channel); }
    else                   { WiFi.softAP(wr_ssid, NULL, wr_channel); }
  } else {
    if (wr_psk[0] != 0x00) { WiFi.softAP(bt_devname, wr_psk, wr_channel); }
    else                   { WiFi.softAP(bt_devname, NULL, wr_channel); }
  }
  delay(150);
  WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
  wifi_initialized = true;
}

void wifi_remote_start_sta() {
  WiFi.mode(WIFI_STA);

  uint8_t ip[4]; bool ip_ok = true;
  for (uint8_t i = 0; i < 4; i++) { ip[i]  = EEPROM.read(config_addr(ADDR_CONF_IP+i)); }
  if (ip[0]==0x00 && ip[1]==0x00 && ip[2]==0x00 && ip[3]==0x00) { ip_ok = false; }
  if (ip[0]==0xFF && ip[1]==0xFF && ip[2]==0xFF && ip[3]==0xFF) { ip_ok = false; }

  uint8_t nm[4]; bool nm_ok = true;
  for (uint8_t i = 0; i < 4; i++) { nm[i]  = EEPROM.read(config_addr(ADDR_CONF_NM+i)); }
  if (nm[0]==0x00 && nm[1]==0x00 && nm[2]==0x00 && nm[3]==0x00) { nm_ok = false; }
  if (nm[0]==0xFF && nm[1]==0xFF && nm[2]==0xFF && nm[3]==0xFF) { nm_ok = false; }

  if (ip_ok && nm_ok) {
    IPAddress sta_ip(ip[0], ip[1], ip[2], ip[3]);
    IPAddress sta_nm(nm[0], nm[1], nm[2], nm[3]);
    WiFi.config(sta_ip, sta_ip, sta_nm);
  }

  delay(100);
  //Serial.print("WiFi ssid: ");
  //Serial.println(wr_ssid);
  //Serial.print("WiFi psk: ");
  //Serial.println(wr_psk);
  if (wr_ssid[0] != 0x00) {
    if (wr_psk[0] != 0x00) { WiFi.begin(wr_ssid, wr_psk); }
    else                   { WiFi.begin(wr_ssid); }
    // We deliberately don't call WiFi.setSleep() here. ESP-IDF
    // aborts at boot with "Should enable WiFi modem sleep when both
    // WiFi and Bluetooth are enabled" if PS_NONE is set while the
    // BT controller is compiled in (HAS_BLE on Supreme builds), and
    // explicitly setting PS_MIN_MODEM gave the same wake-latency
    // grief as the default. Leaving it as Arduino's default — when
    // we want to tune this it needs (a) BT-controller-runtime aware
    // gating and (b) retry-aware HTTP transports both sides.
  }

  delay(500);
  //delay(10000);
  wr_wifi_status = WiFi.status();
  //Serial.print("WiFi status: ");
  //Serial.println(wr_wifi_status);
  wifi_initialized = true;
  wr_last_connect_try = millis();
  // No "WiFi: connecting <SSID>" affirmation — the topbar WiFi icon
  // already shows the state, and the marquee strip is reserved for
  // things the user can act on (errors, recovery countdown).
}

void wifi_remote_stop() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  wifi_initialized = false;
}

// APSTA boot: bring up the softAP first (so the bootstrap UI is
// reachable while STA is still negotiating), then kick STA with the
// saved credentials. The phase machine in wifi_pump_phase() handles
// the transition to STA-only after STA stabilises.
void wifi_remote_start_apsta() {
  WiFi.mode(WIFI_AP_STA);
  // softAP — open, named after bt_devname. Matches the bootstrap softAP
  // exactly (same SSID format, no PSK) so a user joining "RNode XXXX"
  // sees the same network whether the device is freshly out of the
  // box or coming up in the APSTA boot grace window. Using wr_psk
  // would advertise the user's home-WiFi password as the AP PSK —
  // unhelpful and a soft security leak.
  WiFi.softAP(bt_devname, NULL, wr_channel);
  delay(150);
  WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
  // STA — same wiring as wifi_remote_start_sta(), minus the bare
  // WiFi.mode(WIFI_STA) call that would tear down the AP we just set
  // up.
  uint8_t ip[4]; bool ip_ok = true;
  for (uint8_t i = 0; i < 4; i++) { ip[i] = EEPROM.read(config_addr(ADDR_CONF_IP+i)); }
  if (ip[0]==0x00 && ip[1]==0x00 && ip[2]==0x00 && ip[3]==0x00) { ip_ok = false; }
  if (ip[0]==0xFF && ip[1]==0xFF && ip[2]==0xFF && ip[3]==0xFF) { ip_ok = false; }
  uint8_t nm[4]; bool nm_ok = true;
  for (uint8_t i = 0; i < 4; i++) { nm[i] = EEPROM.read(config_addr(ADDR_CONF_NM+i)); }
  if (nm[0]==0x00 && nm[1]==0x00 && nm[2]==0x00 && nm[3]==0x00) { nm_ok = false; }
  if (nm[0]==0xFF && nm[1]==0xFF && nm[2]==0xFF && nm[3]==0xFF) { nm_ok = false; }
  if (ip_ok && nm_ok) {
    IPAddress sta_ip(ip[0], ip[1], ip[2], ip[3]);
    IPAddress sta_nm(nm[0], nm[1], nm[2], nm[3]);
    WiFi.config(sta_ip, sta_ip, sta_nm);
  }
  delay(100);
  if (wr_ssid[0] != 0x00) {
    if (wr_psk[0] != 0x00) { WiFi.begin(wr_ssid, wr_psk); }
    else                   { WiFi.begin(wr_ssid); }
  }
  delay(500);
  wr_wifi_status = WiFi.status();
  wifi_initialized = true;
  wr_last_connect_try = millis();
  wifi_phase           = WifiPhase::ApStaConnecting;
  wifi_phase_started_ms = millis();
  wifi_apsta_deauth_done = false;
  wifi_apsta_deauth_at_ms = 0;
}

void wifi_remote_start() {
  if      (wifi_mode == WR_WIFI_AP)    { wifi_remote_start_ap(); }
  else if (wifi_mode == WR_WIFI_APSTA) { wifi_remote_start_apsta(); }
  else if (wifi_mode == WR_WIFI_STA)   { wifi_remote_start_sta(); }
  else                                 { wifi_remote_stop(); }

  if (wifi_initialized == true) {
    remote_listener.begin();
    remote_listener.setTimeout(WR_SOCKET_TIMEOUT);
    wr_state = WR_STATE_ON;
#if defined(UDP_TRANSPORT)
    udp.begin(UDP_PORT);
#endif
  } else {
    remote_listener.end();
    wr_state = WR_STATE_OFF;
#if defined(UDP_TRANSPORT)
    udp.stop();
#endif
  }
}

void wifi_remote_init() {
  //Serial.print("Initializing WiFi...\n");
  memcpy(wr_hostname, bt_devname, 5);
  memcpy(wr_hostname+5, bt_devname+6, 4);
  wr_hostname[9] = 0x00;
  // mDNS / DNS-SD convention is all-lowercase hostnames. The DNS spec
  // is case-insensitive but Bonjour/Avahi clients display whatever is
  // advertised verbatim — keeping it lowercase avoids "RNode7D31.local"
  // looking inconsistent with other devices on the LAN that follow the
  // convention. Same byte count, just lowercased in place.
  for (int i = 0; i < 9 && wr_hostname[i]; i++) {
    wr_hostname[i] = (char)tolower((unsigned char)wr_hostname[i]);
  }
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  WiFi.setHostname(wr_hostname);

  wr_ssid[32] = 0x00; wr_psk[32] = 0x00;
  for (uint8_t i = 0; i < 32; i++) { wr_ssid[i] = EEPROM.read(config_addr(ADDR_CONF_SSID+i)); if (wr_ssid[i] == 0xFF) { wr_ssid[i] = 0x00; } }
  for (uint8_t i = 0; i < 32; i++) { wr_psk[i]  = EEPROM.read(config_addr(ADDR_CONF_PSK+i));  if (wr_psk[i]  == 0xFF) { wr_psk[i]  = 0x00; } }
  wr_channel = EEPROM.read(eeprom_addr(ADDR_CONF_WCHN)); if (wr_channel < 1 || wr_channel > 14) { wr_channel = WR_CHANNEL_DEFAULT; }
  wifi_remote_start();
  wifi_init_ran = true;
  // Reset the runtime-softAP marker — a fresh init means whatever
  // mode we're trying now is the authoritative one. Auto-fallback
  // and force-softap rearm themselves on the next tick if needed.
  wr_runtime_softap = (wifi_mode == WR_WIFI_AP);
  if (wifi_mode == WR_WIFI_STA && wr_sta_first_attempt_ms == 0) {
    wr_sta_first_attempt_ms = millis();
    wr_sta_fallback_armed = true;
  }
}

// Runtime STA→AP switch. Reinits the WiFi stack as a softAP using the
// device's BT name, sets bootstrap_mode so the SPA shows the wifi
// configure form, and clears the auto-fallback timer so we don't
// loop. EEPROM is untouched — a reboot tries STA again.
void wifi_runtime_force_softap(const char* reason) {
  NOTICEF("WiFi: switching to softAP (%s)", reason ? reason : "manual");
  wifi_mode = WR_WIFI_AP;
  wr_sta_first_attempt_ms = 0;
  wr_sta_fallback_armed = false;
  wifi_remote_init();
  wr_runtime_softap = true;
}

// Write SSID + PSK to EEPROM in the single saved-network slot and mark
// the configured WiFi mode as STA. Shared between handle_wifi_configure
// (HTTP path) and Improv::handle_wifi_settings (Serial provisioning)
// so both end up with byte-identical EEPROM layout.
void wifi_remote_eeprom_write_sta_creds(const char* ssid, const char* psk) {
  const size_t ssid_n = ssid ? strnlen(ssid, 32) : 0;
  const size_t psk_n  = psk  ? strnlen(psk,  32) : 0;
  for (uint8_t i = 0; i < 33; i++) {
    uint8_t c = (i < ssid_n) ? (uint8_t)ssid[i] : 0x00;
    eeprom_update(config_addr(ADDR_CONF_SSID + i), c);
  }
  for (uint8_t i = 0; i < 33; i++) {
    uint8_t c = (i < psk_n) ? (uint8_t)psk[i] : 0x00;
    eeprom_update(config_addr(ADDR_CONF_PSK + i), c);
  }
  wr_conf_save(WR_WIFI_STA);
}

// Promote a STA-only device to APSTA without touching the running STA
// attempt: brings up the recovery softAP alongside, leaves STA driver
// untouched so it keeps retrying in the background. Called by the boot
// 3-minute timeout when STA hasn't connected, and never again this
// boot (wr_sta_fallback_armed flips to false). EEPROM untouched; a
// reboot resets the timer.
void wifi_promote_to_apsta(const char* reason) {
  NOTICEF("WiFi: STA timeout — promoting STA→APSTA (%s)",
          reason ? reason : "boot delay expired");
  Common::Status::say("WiFi: no network found, AP recovery up");
  wifi_mode = WR_WIFI_APSTA;
  // softAP first — open, named after bt_devname (matches the bootstrap
  // softAP exactly). WiFi.mode() with WIFI_AP_STA doesn't tear down
  // the active STA attempt; it just enables the AP interface alongside.
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(bt_devname, NULL, wr_channel);
  delay(50);
  WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
  // Disarm permanently for this boot — security policy: AP can only be
  // brought up by boot path or by explicit authenticated request.
  wr_sta_fallback_armed = false;
}

// Called every wifi_update_status() tick. Drives the WifiPhase state
// machine — applies pending provisions, detects STA-connect, runs the
// deauth + teardown timers. The HTTP-response side of the parked
// request is owned by Web::WebUI::loop() so this file doesn't have to
// drag in the AsyncWebServer headers — wr_pending.req is just data
// to those who care.
void wifi_pump_phase() {
  // -------- 1. Apply pending provision from the HTTP task --------
  if (wr_pending.pending) {
    wr_pending.pending = false;
    NOTICEF("WiFi: applying new SSID '%s'", wr_pending.ssid);

    // Make sure AP is up so the requesting client can still receive
    // the response. softAP() is idempotent — if we're already in
    // APSTA from boot this is a no-op.
    if (wifi_mode != WR_WIFI_APSTA) {
      wifi_mode = WR_WIFI_APSTA;
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(bt_devname, NULL, wr_channel);  // open AP — see wifi_remote_start_apsta
      delay(50);
      WiFi.softAPConfig(ap_ip, ap_ip, ap_nm);
    }

    // Swap creds in RAM and kick STA.
    strncpy(wr_ssid, wr_pending.ssid, 32); wr_ssid[32] = 0;
    strncpy(wr_psk,  wr_pending.psk,  32); wr_psk[32]  = 0;
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.disconnect(/*wifioff=*/false);
      delay(50);
    }
    if (wr_ssid[0] != 0x00) {
      if (wr_psk[0] != 0x00) { WiFi.begin(wr_ssid, wr_psk); }
      else                   { WiFi.begin(wr_ssid); }
    }
    wifi_phase             = WifiPhase::ApStaConnecting;
    wifi_phase_started_ms  = millis();
    wifi_apsta_deauth_done = false;
    wifi_apsta_deauth_at_ms = 0;
  }

  // -------- 2. STA-connected transition --------
  if (wifi_phase == WifiPhase::ApStaConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      wifi_phase            = WifiPhase::ApStaGrace;
      wifi_phase_started_ms = millis();
      wr_device_ip          = WiFi.localIP();
      NOTICEF("WiFi: STA up, IP %s — AP grace 2 min",
              wr_device_ip.toString().c_str());
      // Web::WebUI::loop() will see ApStaGrace + a non-null wr_pending.req
      // and send the HTTP response carrying sta_ip + hostname; once the
      // response is in flight it bumps wifi_apsta_deauth_at_ms so the
      // deauth fires after a TCP-flush margin.
    }
  }

  // -------- 3. Deauth + teardown --------
  if (wifi_phase == WifiPhase::ApStaGrace) {
    if (!wifi_apsta_deauth_done
        && wifi_apsta_deauth_at_ms != 0
        && millis() >= wifi_apsta_deauth_at_ms) {
      int n = WiFi.softAPgetStationNum();
      esp_wifi_deauth_sta(0);   // 0 == all associated stations
      wifi_apsta_deauth_done = true;
      NOTICEF("WiFi: deauthed %d AP client(s); AP down at +%lus",
              n, (unsigned long)(WR_APSTA_GRACE_MS / 1000));
    }
    if ((millis() - wifi_phase_started_ms) >= WR_APSTA_GRACE_MS) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      wifi_mode             = WR_WIFI_STA;
      wifi_phase            = WifiPhase::Idle;
      wifi_phase_started_ms = 0;
      wifi_apsta_deauth_at_ms = 0;
      NOTICE("WiFi: AP grace expired — STA only");
    }
  }
}

void wifi_remote_close_all() {
  // wifi_dbg("Close all"); // TODO: Remove debug
  if (connection) { connection.stop(); }
  WiFiClient client = remote_listener.available();
  while (client) { client.stop(); client = remote_listener.available(); }
  wr_state = WR_STATE_ON;
}

void wifi_remote_check_active() {
  if (millis()-wr_last_read >= WR_READ_TIMEOUT_MS) {
    // wifi_dbg("Connection activity timed out"); // TODO: Remove debug
    if (connection && connection.connected()) {
      connection.stop();
      wifi_remote_close_all();
      host_disconnected();
    }
  }
}

bool wifi_remote_available() {
  if (connection) {
    if (connection.connected()) {
      if (connection.available()) { wr_last_read = millis(); return true; }
      else                        { wifi_remote_check_active(); return false; }
    } else {
      // wifi_dbg("Client disconnected"); // TODO: Remove debug
      wifi_remote_close_all();
      return false;
    }
  } else {
    WiFiClient client = remote_listener.available();
    if (!client) { return false; }
    else {
      // wifi_dbg("Client connected"); // TODO: Remove debug
      connection = client;
      wr_state = WR_STATE_CONNECTED;
      wr_last_read = millis();
      if (connection.available()) { return true; }
      else                        { return false; }
    }
  }
}

uint8_t wifi_remote_read() {
  if (connection && connection.available()) { return connection.read(); }
  else {
    // wifi_dbg("Error: No data to read from TCP socket"); // TODO: Remove debug
    if (connection) { wifi_remote_close_all(); }
    return 0xC0;
  }
}

void wifi_remote_write(uint8_t byte) { if (connection) { connection.write(byte); } }

// If the device can't reach its configured STA network within this
// window after the first attempt, promote STA→APSTA so the user has
// a recovery channel. 3 minutes is long enough that a router power
// blip / DHCP renewal / brief outage doesn't expose the recovery AP
// in the normal happy path, but short enough that someone with a
// genuinely broken WiFi setup isn't waiting forever. Security: this
// only fires at boot. Runtime STA drops never re-arm the timer (see
// wr_sta_fallback_armed clear on first WL_CONNECTED).
#define WR_STA_BEFORE_AP_DELAY_MS (3UL * 60UL * 1000UL)

void wifi_update_status() {
  // Web handler asked for a switch to softAP — apply it on the main
  // loop, not the WebServer task, so WiFi reinit doesn't race
  // with in-flight requests.
  if (wr_force_softap_pending) {
    wr_force_softap_pending = false;
    wifi_runtime_force_softap("user-requested via /api/wifi/softap");
    return;
  }

  // Drive the APSTA transition state machine (boot grace + post-
  // provision teardown). Must run before the existing STA-status
  // reading below so the phase is up-to-date for everything else.
  wifi_pump_phase();

  wr_wifi_status = WiFi.status();
  if (wr_wifi_status == WL_CONNECTED) {
    wr_device_ip = WiFi.localIP();
    // First-time STA-connect this boot: clear any countdown / "no
    // network" message the marquee was showing, disarm the AP-recovery
    // timer permanently. We don't announce the IP on the marquee —
    // the WiFi icon shows connected state and the SPA / /api/info
    // surface the IP for anyone who needs it.
    if (wr_sta_fallback_armed) {
      Common::Status::clear();
    }
    wr_sta_fallback_armed = false;
    // mDNS: advertise the device under wr_hostname.local so the SPA
    // can redirect to it after a WiFi-save reboot, and so users on the
    // LAN can reach the web UI without hunting for the DHCP-assigned
    // IP. Start once on first STA-connected — MDNS.begin is idempotent
    // but logging it twice is noisy.
    //
    // Retry on failure with a 30-second backoff, NOT every main-loop
    // iteration. mDNS is non-critical, and the prior tight retry loop
    // hammered the WiFi stack: each failed MDNS.begin() leaks small
    // allocations in the lwIP / mdns service code, so unbounded retry
    // burns through internal SRAM (we measured ~85 KB lost in 60 s).
    static bool s_mdns_started = false;
    static uint32_t s_mdns_next_retry_ms = 0;
    // Start mDNS whenever STA is associated, regardless of whether
    // we're STA-only or APSTA. Holding it off during the post-
    // provisioning APSTA grace means a freshly Improv-provisioned
    // device isn't resolvable as <hostname>.local for the first
    // ~2 minutes — exactly the window when the SPA most needs the
    // friendly URL.
    const bool sta_carrier = (wifi_mode == WR_WIFI_STA || wifi_mode == WR_WIFI_APSTA);
    if (!s_mdns_started && sta_carrier
        && (int32_t)(millis() - s_mdns_next_retry_ms) >= 0) {
      if (MDNS.begin(wr_hostname)) {
        MDNS.addService("http", "tcp", 80);
        NOTICEF("mDNS: advertising as http://%s.local", wr_hostname);
        s_mdns_started = true;
      } else {
        WARNINGF("mDNS: begin(%s) failed — retrying in 30s", wr_hostname);
        s_mdns_next_retry_ms = millis() + 30000UL;
      }
    }
  }
  if (wifi_mode == WR_WIFI_AP && wifi_initialized) { wr_device_ip = WiFi.softAPIP(); wr_wifi_status = WL_CONNECTED; }
  if (wifi_init_ran && wifi_mode == WR_WIFI_STA && wr_wifi_status != WL_CONNECTED) {
    // STA-only path — no AP in play. Two things happen here:
    //   1. Boot timer: if STA hasn't connected within
    //      WR_STA_BEFORE_AP_DELAY_MS (3 min), promote to APSTA so
    //      the user has a recovery channel. Only fires while
    //      wr_sta_fallback_armed is true — i.e. only during the
    //      first boot window before any successful STA connection.
    //   2. Reconnect: every WR_RECONNECT_INTERVAL_MS, re-run the
    //      WiFi init. Once promoted to APSTA the wifi_mode check
    //      above no longer matches and the APSTA branch below takes
    //      over instead.
    if (wr_sta_fallback_armed && wr_sta_first_attempt_ms != 0) {
      uint32_t elapsed = millis() - wr_sta_first_attempt_ms;
      if (elapsed >= WR_STA_BEFORE_AP_DELAY_MS) {
        wifi_promote_to_apsta("no STA connection in 3 min");
        return;
      }
      // Throttle to ~1Hz — update() replaces the most recent ring
      // entry in place, so we don't fill the ring with countdown
      // ticks. The OLED / SPA marquee sees a single message whose
      // text changes each second.
      static uint32_t s_next_countdown_ms = 0;
      if ((int32_t)(millis() - s_next_countdown_ms) >= 0) {
        uint32_t remaining_s = (WR_STA_BEFORE_AP_DELAY_MS - elapsed) / 1000;
        char buf[64];
        snprintf(buf, sizeof(buf), "WiFi: no network, AP in %lu:%02lu",
                 (unsigned long)(remaining_s / 60),
                 (unsigned long)(remaining_s % 60));
        // 1.5 s TTL: each update auto-expires before the next one
        // arrives, so when STA finally connects (and we stop updating)
        // the message naturally falls off the marquee within ~1.5 s
        // without needing a separate clear.
        Common::Status::update(buf, 1500);
        s_next_countdown_ms = millis() + 1000;
      }
    }
    if (millis()-wr_last_connect_try >= WR_RECONNECT_INTERVAL_MS) { wifi_remote_init(); }
  }
  if (wifi_init_ran && wifi_mode == WR_WIFI_APSTA && wr_wifi_status != WL_CONNECTED) {
    // APSTA path — AP is already up as the recovery channel, so we
    // don't tear down on STA timeout. Just keep retrying STA in the
    // background; the user can either wait it out or reconfigure via
    // the AP.
    if (millis() - wr_last_connect_try >= WR_RECONNECT_INTERVAL_MS) {
      WiFi.disconnect(/*wifioff=*/false);
      delay(50);
      if (wr_ssid[0] != 0x00) {
        if (wr_psk[0] != 0x00) { WiFi.begin(wr_ssid, wr_psk); }
        else                   { WiFi.begin(wr_ssid); }
      }
      wr_last_connect_try = millis();
    }
  }
}

void update_wifi() {
#if defined(UDP_TRANSPORT)
  if (wifi_initialized) {
    if (udp.parsePacket() > 0) {
      size_t len = udp.read(udp_buffer.writable(MTU), MTU);
    if (len > 0) {
        udp_buffer.resize(len);
#if defined(HAS_RNS)
        if (udp_interface) {
          udp_interface.handle_incoming(udp_buffer);
        }
#endif
      }
    }
  }
#endif
  if (millis()-last_wifi_update >= wifi_update_interval_ms) {
    wifi_update_status();
    last_wifi_update = millis();
  }
}