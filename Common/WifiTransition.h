#pragma once

// Common types for the WiFi APSTA transition state machine.
//
// The state-machine implementation lives in Remote.h (alongside the
// rest of the WiFi remote stack); the HTTP-response side of the
// parked /api/wifi/configure request lives in Web/WebUI.h. Both files
// need to agree on the type of `wr_pending` and `wifi_phase`, which
// is why these declarations sit in Common/ — neither side has to
// drag in the other's full header to compile.

#include <cstdint>

class AsyncWebServerRequest;  // forward — pointer-only use here

// What stage of the APSTA bridge we're currently in. See the policy
// note above wifi_pump_phase() in Remote.h.
enum class WifiPhase : uint8_t {
  Idle,             // STA-only or AP-only steady state
  ApStaConnecting,  // APSTA active, STA hasn't reached WL_CONNECTED yet
  ApStaGrace,       // APSTA, STA up, AP in 2-minute grace before teardown
};

// Policy constants — number-of-milliseconds knobs the transition
// state machine uses. Shared so Remote.h's pump and Web/WebUI.h's
// response handoff agree on the timings.
static constexpr uint32_t WR_PROVISION_TIMEOUT_MS = 30 * 1000UL;
static constexpr uint32_t WR_APSTA_DEAUTH_DELAY   = 1000UL;
static constexpr uint32_t WR_APSTA_GRACE_MS       = 120 * 1000UL;

// Pending /api/wifi/configure request parked while STA negotiates.
// The HTTP task fills this in and sets `pending = true`; the main
// loop drains and applies the credentials, then later sends the
// response when STA either reaches WL_CONNECTED or times out.
struct PendingProvision {
  bool                   pending       = false;
  char                   ssid[33]      = {0};
  char                   psk[33]       = {0};
  AsyncWebServerRequest* req           = nullptr;
  uint32_t               requested_ms  = 0;
};
