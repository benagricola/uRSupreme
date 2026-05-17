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
#if defined(UDP_TRANSPORT)
#include <WiFiUdp.h>
#include <Bytes.h>
#endif

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
// boot. Used by the 30-second auto-softAP fallback so a device whose
// STA network has gone away (changed SSID, router off, moved) doesn't
// stay unreachable forever — once the timer expires we drop into
// softAP so the user can recover via the bootstrap UI.
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
  }
  
  delay(500);
  //delay(10000);
  wr_wifi_status = WiFi.status(); 
  //Serial.print("WiFi status: ");
  //Serial.println(wr_wifi_status);
  wifi_initialized = true;
  wr_last_connect_try = millis();
}

void wifi_remote_stop() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_MODE_NULL);
  wifi_initialized = false;
}

void wifi_remote_start() {
  if      (wifi_mode == WR_WIFI_AP)  { wifi_remote_start_ap(); }
  else if (wifi_mode == WR_WIFI_STA) { wifi_remote_start_sta(); }
  else                               { wifi_remote_stop(); }

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
// window after the first attempt, automatically fall back to softAP
// so the user can recover. 30 s is long enough for a slow router
// to come up but short enough not to leave a deployed device
// silently unreachable.
#define WR_STA_FALLBACK_MS 30000UL

void wifi_update_status() {
  // Web handler asked for a switch to softAP — apply it on the main
  // loop, not the WebServer task, so WiFi reinit doesn't race
  // with in-flight requests.
  if (wr_force_softap_pending) {
    wr_force_softap_pending = false;
    wifi_runtime_force_softap("user-requested via /api/wifi/softap");
    return;
  }

  wr_wifi_status = WiFi.status();
  if (wr_wifi_status == WL_CONNECTED) {
    wr_device_ip = WiFi.localIP();
    // Once we've successfully attached at least once, disarm the
    // fallback timer — a transient drop later shouldn't kick us
    // out of STA (the existing 10 s reconnect logic handles that).
    wr_sta_fallback_armed = false;
  }
  if (wifi_mode == WR_WIFI_AP && wifi_initialized) { wr_device_ip = WiFi.softAPIP(); wr_wifi_status = WL_CONNECTED; }
  if (wifi_init_ran && wifi_mode == WR_WIFI_STA && wr_wifi_status != WL_CONNECTED) {
    // Auto-fallback: we've been trying STA for too long without ever
    // connecting. Go softAP so the bootstrap UI is reachable.
    if (wr_sta_fallback_armed && wr_sta_first_attempt_ms != 0
        && (millis() - wr_sta_first_attempt_ms) >= WR_STA_FALLBACK_MS) {
      wifi_runtime_force_softap("STA timeout after 30s");
      return;
    }
    if (millis()-wr_last_connect_try >= WR_RECONNECT_INTERVAL_MS) { wifi_remote_init(); }
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