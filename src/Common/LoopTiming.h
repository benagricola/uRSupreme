#pragma once
#include <stdint.h>

// Per-section max-duration tracking for the main loop, to find which step is
// holding the single-threaded loop long enough to starve LoRa receive. Each
// slot holds the longest observed duration (microseconds) since the last
// reset. micros() wrap is handled by the unsigned subtraction at the call site.
//
// Opt-in instrumentation: build with -DURTN_LOOP_DIAG to compile it in (and
// expose it on /api/diag/loop). Production builds omit the flag, the storage
// and accessors below disappear, and the URTN_LT* macros collapse to running
// the wrapped statement with no micros() probe and no timing overhead.
namespace Common {
namespace LoopTiming {

#if defined(URTN_LOOP_DIAG)
  inline uint32_t max_loop_us      = 0;   // full iteration period
  inline uint32_t max_reticulum_us = 0;   // reticulum.loop()
  inline uint32_t max_tcp_us       = 0;   // TCPTransport::service()
  inline uint32_t max_wifi_us      = 0;   // update_wifi()
  inline uint32_t max_lxmf_us      = 0;   // LXMFGateway::loop()
  inline uint32_t max_webui_us     = 0;   // WebUI::loop()
  inline uint32_t max_txq_us       = 0;   // tx_queue_handler() - LoRa TX drain
  inline uint32_t max_modem_us     = 0;   // check_modem_status()
  inline uint32_t max_prune_us     = 0;   // LXMFGateway::prune_all()
  inline uint32_t max_serial_us    = 0;   // buffer_serial()

  inline void note(uint32_t& slot, uint32_t us) { if (us > slot) slot = us; }

  inline void reset() {
    max_loop_us = max_reticulum_us = max_tcp_us = max_wifi_us = max_lxmf_us = max_webui_us = 0;
    max_txq_us = max_modem_us = max_prune_us = max_serial_us = 0;
  }
#endif

}  // namespace LoopTiming
}  // namespace Common

// Time a single statement into a LoopTiming slot, and mark the loop period.
// When URTN_LOOP_DIAG is off both macros just run the statement (no micros(),
// no slot reference), so they cost nothing in production builds.
#if defined(URTN_LOOP_DIAG)
  #define URTN_LT(slot, stmt) do { uint32_t _urtn_t0 = micros(); stmt; Common::LoopTiming::note(slot, micros() - _urtn_t0); } while (0)
  #define URTN_LT_PERIOD()    do { static uint32_t _urtn_ll = 0; uint32_t _urtn_ln = micros(); if (_urtn_ll) Common::LoopTiming::note(Common::LoopTiming::max_loop_us, _urtn_ln - _urtn_ll); _urtn_ll = _urtn_ln; } while (0)
#else
  #define URTN_LT(slot, stmt) do { stmt; } while (0)
  #define URTN_LT_PERIOD()    do { } while (0)
#endif
