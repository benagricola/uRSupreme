// GNSS facade - the one interface the rest of the firmware talks to.
//
// The T-Beam Supreme ships with one of two GNSS modules on the same
// UART (GPIO 8/9, 9600 8N1): a Quectel L76K (CASIC core) or a u-blox
// MAX-M10. Both stream NMEA, parsed by Nmea.h into the shared Fix.
// This facade owns the UART, the power policy and the module
// identification, and binds the chip-specific driver at runtime:
//
//   identification  Each begin()/power_on() sends both identification
//                   queries (each chipset ignores the other's): the
//                   CASIC product-info query from L76K.h and a UBX
//                   MON-VER poll from MaxM10.h. A checksum-verified
//                   MON-VER reply, or a "u-blox" TXT banner, binds
//                   MAX-M10; a CASIC TXT banner or PCAS reply binds
//                   L76K. Until something matches the module stays
//                   Unknown and everything behaves as a plain NMEA
//                   receiver.
//
//   power           Enable + LOCATION interval come from the sensor
//                   power config (set_power_config, persisted with
//                   the other sensors):
//                     disabled              -> rail off
//                     interval <  5 min     -> always on
//                     interval >= 5 min     -> duty-cycled
//                   Acquisition always runs at full power with a
//                   progress-aware window (a cold receiver cannot
//                   download ephemeris in short power-save wakes);
//                   failures back off exponentially. Between fixes
//                   the L76K sleeps by rail cut, the MAX-M10 by PSMOO
//                   (after first fix) or RXM-PMREQ (backoff), both
//                   with backup RAM kept warm where the hardware
//                   allows. M10 config writes are wake-prefixed and
//                   ACK-verified.
//
//   time            On a valid RMC whose epoch is fresh per the GPS
//                   entry in Clock::Manager, reports to the clock.
//                   That config gates ONLY clock resync cadence; it
//                   has no influence on receiver power.

#pragma once

#include <stdint.h>
#include <Arduino.h>
#include <XPowersLib.h>
#include "../../Clock/Manager.h"
#include "Nmea.h"
#include "L76K.h"
#include "MaxM10.h"

extern XPowersLibInterface* PMU;

namespace Sensors {
namespace Gnss {

using Nmea::Fix;

struct Pins {
  int rx;
  int tx;
  int en;
  uint32_t baud;
};

// Sensor-side power policy. Owned by Sensors::SensorConfig (persisted
// in /lxmf/sensors.json), pushed here via set_power_config(). This is
// the LOCATION cadence: how often the receiver should be awake and
// producing fixes. It is deliberately decoupled from the GPS entry in
// Clock::Manager, which only gates how often a live fix may resync
// the clock (see maybe_report_time) and has no power meaning.
struct PowerConfig {
  bool     enabled    = true;
  uint32_t interval_s = 0;   // 0 = always on; >= PULSE_THRESHOLD_S = pulsed
};

// If the location interval is at or above this, duty-cycle the
// receiver (rail pulse or PSMOO depending on the module). Below it,
// keep it continuously powered - frequent rail cycling would shred
// the L76K's cold-start budget, and a PSMOO period that short saves
// nothing.
inline constexpr uint32_t PULSE_THRESHOLD_S    = 5 * 60;     // 5 min
// Acquisition window: give up after 2 min of hunting with NOTHING in
// view, but while satellites are visible the window extends (the
// receiver is converging on ephemeris - aborting now throws that work
// away) up to the hard cap below.
inline constexpr uint32_t PULSE_ACQUIRE_TIMEOUT_MS = 120000; // base: 2 min
inline constexpr uint32_t PULSE_ACQUIRE_MAX_MS     = 300000; // progress cap: 5 min
inline constexpr uint32_t PROGRESS_HOLD_MS         = 30000;  // "recently saw sats"
// Identification probe retry. begin() sends one probe; a reply lost to
// timing (the module mid-frame, or asleep - a sleeping M10 discards
// the bytes that wake it) used to leave the module unidentified until
// the next rail cycle. Re-probe at this cadence while unidentified;
// probe_module() no-ops once a module binds, so this costs nothing
// afterwards.
inline constexpr uint32_t PROBE_RETRY_MS           = 10000;
// M10 config delivery. WAKE_SETTLE: a PSMOO-inactive receiver discards
// the bytes that wake it and needs time to bring its UART back before
// it can hear a real frame; 150 ms is comfortably above the observed
// wake-to-output latency. ACK_WAIT: VALSET acknowledgements arrive
// within one navigation epoch (1 s) when the receiver is awake; 700 ms
// catches the awake case quickly so a sleeping receiver gets re-woken
// without long stalls. RETRIES bounds one request cycle; the scheduler
// re-requests later, so giving up here is never final.
inline constexpr uint32_t M10_CFG_WAKE_SETTLE_MS = 150;
inline constexpr uint32_t M10_CFG_ACK_WAIT_MS    = 700;
inline constexpr uint8_t  M10_CFG_RETRIES        = 3;
// In pulsed mode with PSMOO active the module schedules its own fixes;
// the firmware intervenes only when fixes go stale past this factor of
// the location interval plus a fixed grace (one module-side ACQPERIOD
// retry round) - distinguishing "running a little late" from "lost".
inline constexpr uint32_t PSMOO_STALE_FACTOR     = 2;
inline constexpr uint32_t PSMOO_STALE_GRACE_MS   = 60000;
inline constexpr uint32_t PULSE_RETRY_BACKOFF_MS   = 60000;  // try again 60 s later on first timeout
// Exponential backoff cap for consecutive failed pulses (no sky
// view): base << shift, clamped here.
inline constexpr uint32_t PULSE_RETRY_BACKOFF_MAX_MS = 30UL * 60UL * 1000UL;
inline constexpr uint8_t  PULSE_RETRY_BACKOFF_SHIFT_MAX = 9;

enum class PowerMode : uint8_t { Off, AlwaysOn, Pulsed };
enum class PulseState : uint8_t { Idle, Acquiring };
enum class Module : uint8_t { Unknown = 0, L76K = 1, MAXM10 = 2 };

// Which power configuration was last pushed to the MAX-M10. Forced
// back to NotApplied whenever the rail cycles, since the chip may
// have lost its RAM/BBR config without backup supply.
enum class M10Power : uint8_t { NotApplied, Full, Psmoo };

namespace _detail {
  inline HardwareSerial*& serial_ref() { static HardwareSerial* v = nullptr; return v; }
  inline Pins&           pins_ref()    { static Pins p{ -1, -1, -1, 9600 }; return p; }
  inline Fix&            fix_ref()     { static Fix f{}; return f; }
  inline uint8_t&        module_ref()  { static uint8_t m = 0; return m; }
  // Line buffer. NMEA sentence max is 82 bytes incl $ and \r\n.
  inline char (&line_buf_ref())[96]   { static char buf[96]; return buf; }
  inline size_t&         line_len_ref() { static size_t v = 0; return v; }
  // Last time we reported a time to TimeManager (millis()). 0 = never.
  inline uint32_t&       last_report_ms_ref() { static uint32_t v = 0; return v; }
  // Power-mode state. `pulse_last_rep_snapshot` is the value of
  // last_report_ms_ref() at the moment the current pulse started -
  // we use it to detect "did a fresh report happen *during this
  // pulse*" without confusing the retry-backoff path (which writes
  // an old timestamp into last_report_ms_ref() to schedule the next
  // pulse). Without this, the retry-backoff math made `last_rep`
  // numerically larger than `pulse_started_ms`, which fooled the
  // acquire-detection branch into firing back-to-back pulses.
  inline bool&           hw_powered_ref()     { static bool v = false; return v; }
  inline PulseState&     pulse_state_ref()    { static PulseState v = PulseState::Idle; return v; }
  inline uint32_t&       pulse_started_ms_ref(){ static uint32_t v = 0; return v; }
  inline uint32_t&       pulse_last_rep_snapshot() { static uint32_t v = 0; return v; }
  // Consecutive pulse-timeout count. Reset on every successful
  // acquire and on demand via reset_backoff() from the IMU motion
  // path. Drives the exponential retry cadence.
  inline uint8_t&        backoff_count_ref()  { static uint8_t v = 0; return v; }
  inline M10Power&       m10_power_ref()      { static M10Power v = M10Power::NotApplied; return v; }
  inline uint32_t&       m10_interval_ref()   { static uint32_t v = 0; return v; }
  // Location scheduling state - intentionally separate from
  // last_report_ms_ref(), which belongs to the CLOCK-sync path.
  inline uint32_t&       last_fix_ms_ref()    { static uint32_t v = 0; return v; }
  inline bool&           ever_fixed_ref()     { static bool v = false; return v; }
  inline uint32_t&       next_attempt_ms_ref(){ static uint32_t v = 0; return v; }
  inline uint32_t&       sats_seen_ms_ref()   { static uint32_t v = 0; return v; }
  // Non-blocking M10 config delivery (wake byte -> settle -> VALSETs
  // -> verified ACKs). A sleeping M10 discards the bytes that wake it
  // and the VALSET ACK is the only proof of delivery, so the cached
  // power state advances ONLY when every sent VALSET is acknowledged.
  enum class M10Cfg : uint8_t { Idle, Settling, AwaitAck };
  inline M10Cfg&         m10_cfg_state_ref()  { static M10Cfg v = M10Cfg::Idle; return v; }
  inline uint32_t&       m10_cfg_t_ref()      { static uint32_t v = 0; return v; }
  inline uint32_t&       m10_cfg_acks_ref()   { static uint32_t v = 0; return v; }
  inline uint32_t&       m10_cfg_naks_ref()   { static uint32_t v = 0; return v; }
  inline uint8_t&        m10_cfg_need_ref()   { static uint8_t v = 0; return v; }
  inline uint8_t&        m10_cfg_tries_ref()  { static uint8_t v = 0; return v; }
  inline M10Power&       m10_cfg_want_ref()   { static M10Power v = M10Power::NotApplied; return v; }
  inline uint32_t&       m10_cfg_iv_ref()     { static uint32_t v = 0; return v; }
}

inline PowerConfig& power_config() { static PowerConfig c; return c; }
inline void set_power_config(bool enabled, uint32_t interval_s) {
  power_config().enabled    = enabled;
  power_config().interval_s = interval_s;
}

inline Module module() { return (Module)_detail::module_ref(); }
inline const char* module_name() {
  switch (module()) {
    case Module::L76K:   return "L76K";
    case Module::MAXM10: return "MAX-M10";
    default:             return "unknown";
  }
}

// Send both identification queries; each chipset ignores the other's.
inline void probe_module() {
  HardwareSerial* s = _detail::serial_ref();
  if (s == nullptr || _detail::module_ref() != 0) return;
  L76K::query_product_info(*s);
  MaxM10::poll_mon_ver(*s);
}

namespace _detail {
  inline void bind_module(Module m, const char* how) {
    if (module_ref() != 0) return;
    module_ref() = (uint8_t)m;
    NOTICEF("GPS: module identified: %s (%s)", module_name(), how);
    HardwareSerial* s = serial_ref();
    if (m == Module::MAXM10 && s != nullptr) {
      MaxM10::on_identified(*s);
      m10_power_ref() = M10Power::NotApplied;  // force a power config pass
    }
  }
}

// Boot init. GPS_EN is held HIGH to power the module; the UART comes
// up at 9600 8N1 and both identification queries go out.
inline void begin(HardwareSerial& serial, const Pins& pins) {
  _detail::serial_ref() = &serial;
  _detail::pins_ref()   = pins;
  if (pins.en >= 0) {
    pinMode(pins.en, OUTPUT);
    digitalWrite(pins.en, HIGH);   // power the module
    delay(20);
  }
  serial.begin(pins.baud, SERIAL_8N1, pins.rx, pins.tx);
  // Power.h has already enabled ALDO4 at PMU init; mark the rail as
  // tracked so power_off() knows it's safe to cut it.
  _detail::hw_powered_ref() = true;
  NOTICEF("GPS: UART up on rx=%d tx=%d en=%d baud=%lu",
          pins.rx, pins.tx, pins.en, (unsigned long)pins.baud);
  probe_module();
}

// Bring the GNSS module back up - ALDO4 on, GPS_EN high. Idempotent.
inline void power_on() {
  if (PMU && !PMU->isPowerChannelEnable(XPOWERS_ALDO4)) {
    PMU->setPowerChannelVoltage(XPOWERS_ALDO4, 3300);
    PMU->enablePowerOutput(XPOWERS_ALDO4);
  }
  const int en = _detail::pins_ref().en;
  if (en >= 0) digitalWrite(en, HIGH);
  _detail::hw_powered_ref() = true;
  // The rail may have been down; whatever power mode the M10 held in
  // RAM/BBR could be gone. Reapply on the next pump pass.
  _detail::m10_power_ref() = M10Power::NotApplied;
  probe_module();   // no-op once identified
}

// Cut power to the GNSS module. ALDO4 off, GPS_EN low. Drops the in-
// flight NMEA parser state so partial lines don't bleed into the
// next power-on. Idempotent.
inline void power_off() {
  if (PMU) PMU->disablePowerOutput(XPOWERS_ALDO4);
  const int en = _detail::pins_ref().en;
  if (en >= 0) digitalWrite(en, LOW);
  _detail::hw_powered_ref()      = false;
  _detail::line_len_ref()        = 0;
  _detail::fix_ref().last_byte_ms = 0;
}

inline bool is_powered() { return _detail::hw_powered_ref(); }
inline PulseState pulse_state() { return _detail::pulse_state_ref(); }

// External knob to drop the exponential-backoff counter back to 0.
// Called by the IMU motion path: when the device gets moved we may
// have a fresh sky view and should retry sooner rather than wait out
// the (potentially 30-min) backoff window. Cheap; safe to call from
// the sensor poll loop.
inline void reset_backoff() {
  if (_detail::backoff_count_ref() != 0) {
    NOTICEF("GPS: backoff reset (was attempt %u)",
            (unsigned)_detail::backoff_count_ref());
    _detail::backoff_count_ref() = 0;
  }
}

// Decide the current target power mode from the SENSOR power config.
// The GPS time-source config plays no part here: clock-sync cadence
// must never wake or sleep the receiver.
inline PowerMode target_mode() {
  const PowerConfig& pc = power_config();
  if (!pc.enabled)                          return PowerMode::Off;
  if (pc.interval_s < PULSE_THRESHOLD_S)    return PowerMode::AlwaysOn;
  return PowerMode::Pulsed;
}

namespace _detail {

  // Time-source policy: report a fresh RMC epoch to TimeManager when
  // the CLOCK-sync interval has elapsed (or on the very first fix).
  // Clock interval 0 means "sync the clock once per boot"; any other
  // value re-syncs that often WHILE fixes happen to be flowing. This
  // path never influences receiver power.
  inline void maybe_report_time(double epoch) {
    if (epoch <= 0.0) return;
    const auto& cfg = Clock::Manager::get_config(Clock::Manager::Source::GPS);
    const uint32_t now = millis();
    const bool first_time = (last_report_ms_ref() == 0);
    const bool may_repoll = (cfg.interval_s > 0)
        && ((now - last_report_ms_ref()) >= cfg.interval_s * 1000UL);
    if (first_time || may_repoll) {
      if (Clock::Manager::report_time(Clock::Manager::Source::GPS, epoch)) {
        last_report_ms_ref() = now;
      }
    }
  }

  // The firmware rail pulse - the duty-cycle strategy for the L76K
  // (and unidentified modules). Ported behaviour: power up when due,
  // detect acquisition via the last_report snapshot, exponential
  // backoff on timeout.
  // ---- M10 config delivery (non-blocking, pump-driven) ----------------
  //
  // Request a power mode; the pump below walks wake -> settle ->
  // VALSETs -> ACK verification. m10_power_ref() advances only when
  // every VALSET in the batch is acknowledged, so a command swallowed
  // by a sleeping receiver is retried instead of silently lost.
  inline void m10_request_power(M10Power want, uint32_t interval_s) {
    if (m10_power_ref() == want
        && (want != M10Power::Psmoo || m10_interval_ref() == interval_s)) return;
    if (m10_cfg_state_ref() != M10Cfg::Idle
        && m10_cfg_want_ref() == want && m10_cfg_iv_ref() == interval_s) return;
    m10_cfg_want_ref()  = want;
    m10_cfg_iv_ref()    = interval_s;
    m10_cfg_tries_ref() = 0;
    m10_cfg_state_ref() = M10Cfg::Settling;
    m10_cfg_t_ref()     = millis();
    HardwareSerial* s = serial_ref();
    if (s) MaxM10::wake(*s);
  }

  inline void m10_cfg_pump(uint32_t now) {
    HardwareSerial* s = serial_ref();
    if (!s || m10_cfg_state_ref() == M10Cfg::Idle) return;
    switch (m10_cfg_state_ref()) {
      case M10Cfg::Settling:
        if (now - m10_cfg_t_ref() < M10_CFG_WAKE_SETTLE_MS) return;
        m10_cfg_acks_ref() = MaxM10::valset_acks();
        m10_cfg_naks_ref() = MaxM10::valset_naks();
        if (m10_cfg_want_ref() == M10Power::Full) {
          MaxM10::set_power(*s, /*full_power=*/true, 0);
          m10_cfg_need_ref() = 1;
        } else {
          MaxM10::set_power(*s, /*full_power=*/false, m10_cfg_iv_ref());
          m10_cfg_need_ref() = 3;   // period + acqperiod + operatemode
        }
        m10_cfg_t_ref()     = now;
        m10_cfg_state_ref() = M10Cfg::AwaitAck;
        return;
      case M10Cfg::AwaitAck: {
        const uint32_t acks = MaxM10::valset_acks() - m10_cfg_acks_ref();
        const uint32_t naks = MaxM10::valset_naks() - m10_cfg_naks_ref();
        if (naks == 0 && acks >= m10_cfg_need_ref()) {
          m10_power_ref()    = m10_cfg_want_ref();
          m10_interval_ref() = m10_cfg_iv_ref();
          m10_cfg_state_ref() = M10Cfg::Idle;
          NOTICEF("GPS: MAX-M10 %s acknowledged%s",
                  m10_cfg_want_ref() == M10Power::Full ? "full power" : "PSMOO",
                  m10_cfg_want_ref() == M10Power::Psmoo ? " (self-cycling)" : "");
          return;
        }
        if (naks != 0 || (now - m10_cfg_t_ref()) > M10_CFG_ACK_WAIT_MS) {
          if (++m10_cfg_tries_ref() < M10_CFG_RETRIES) {
            m10_cfg_state_ref() = M10Cfg::Settling;
            m10_cfg_t_ref()     = now;
            MaxM10::wake(*s);
          } else {
            WARNINGF("GPS: MAX-M10 config not acknowledged (%lu acks, %lu naks, 3 tries)",
                     (unsigned long)acks, (unsigned long)naks);
            m10_cfg_state_ref() = M10Cfg::Idle;
            // Leave m10_power_ref() untouched: a later scheduler pass
            // re-requests and the cycle starts again.
          }
        }
        return;
      }
      default: return;
    }
  }

  // ---- unified acquisition scheduler -----------------------------------
  //
  // One state machine for both modules. Acquisition ALWAYS runs at
  // full power: a receiver that has never fixed cannot download
  // ephemeris in short power-save wakes (it needs ~30 s of continuous
  // tracking per satellite), so cold receivers are never handed to a
  // duty cycle. The window is progress-aware: 2 min base, extended
  // while satellites are in view, capped at 5 min. A hopeless window
  // backs off exponentially (60 s doubling to 30 min, IMU motion
  // resets it); the L76K sleeps by rail cut, the M10 by RXM-PMREQ with
  // the rail up so backup RAM keeps the next attempt hot.
  //
  // After a fix: the L76K rail-pulses on the location interval as
  // before; the M10 is handed to PSMOO with the location interval and
  // self-cycles, the firmware only re-acquiring if fixes go stale
  // (2x interval + margin) - PSMOO with warm ephemeris re-fixes in
  // seconds, so its fixed 60 s ACQPERIOD is then appropriate.
  inline void scheduler_pump(uint32_t now, uint32_t interval_s) {
    const bool m10 = (module() == Module::MAXM10);
    const uint32_t interval_ms = interval_s * 1000UL;
    const uint32_t last_fix    = last_fix_ms_ref();

    if (pulse_state_ref() == PulseState::Idle) {
      bool due;
      if (!ever_fixed_ref()) {
        // Cold: attempt schedule is the backoff ladder.
        due = (next_attempt_ms_ref() == 0)
              || ((int32_t)(now - next_attempt_ms_ref()) >= 0);
      } else if (m10 && m10_power_ref() == M10Power::Psmoo) {
        // PSMOO is doing the cycling; intervene only on staleness.
        due = (now - last_fix)
              > (PSMOO_STALE_FACTOR * interval_ms + PSMOO_STALE_GRACE_MS);
      } else {
        due = (now - last_fix) >= interval_ms;
      }
      if (!due) {
        if (!m10 && hw_powered_ref()) power_off();
        return;
      }
      if (!hw_powered_ref()) power_on();
      if (m10) m10_request_power(M10Power::Full, 0);
      pulse_state_ref()      = PulseState::Acquiring;
      pulse_started_ms_ref() = now;
      sats_seen_ms_ref()     = 0;
      NOTICEF("GPS: acquisition start (interval=%lus, %s, %s)",
              (unsigned long)interval_s,
              ever_fixed_ref() ? "warm" : "cold",
              m10 ? "MAX-M10 full power" : "rail pulse");
      return;
    }

    // Acquiring.
    const bool acquired = ever_fixed_ref()
                          && last_fix != 0
                          && (int32_t)(last_fix - pulse_started_ms_ref()) >= 0;
    const uint32_t elapsed   = now - pulse_started_ms_ref();
    const bool progressing   = sats_seen_ms_ref() != 0
                               && (now - sats_seen_ms_ref()) < PROGRESS_HOLD_MS;
    const uint32_t window_ms = progressing ? PULSE_ACQUIRE_MAX_MS
                                           : PULSE_ACQUIRE_TIMEOUT_MS;
    if (acquired) {
      NOTICEF("GPS: acquired in %lums (attempt %u)",
              (unsigned long)elapsed, (unsigned)backoff_count_ref());
      backoff_count_ref() = 0;
      next_attempt_ms_ref() = 0;
      pulse_state_ref() = PulseState::Idle;
      if (m10) {
        m10_request_power(M10Power::Psmoo, interval_s);
      } else {
        power_off();
      }
      return;
    }
    if (elapsed > window_ms) {
      uint8_t& n = backoff_count_ref();
      const uint8_t shift = (n < PULSE_RETRY_BACKOFF_SHIFT_MAX) ? n : PULSE_RETRY_BACKOFF_SHIFT_MAX;
      uint32_t delay_ms = PULSE_RETRY_BACKOFF_MS << shift;
      if (delay_ms > PULSE_RETRY_BACKOFF_MAX_MS) delay_ms = PULSE_RETRY_BACKOFF_MAX_MS;
      if (n < 255) n++;
      WARNINGF("GPS: no fix after %lums (%s); backing off %lums (attempt %u)",
               (unsigned long)elapsed,
               progressing ? "had satellites in view" : "nothing in view",
               (unsigned long)delay_ms, (unsigned)n);
      next_attempt_ms_ref() = now + delay_ms;
      pulse_state_ref() = PulseState::Idle;
      HardwareSerial* s = serial_ref();
      if (m10 && s != nullptr) {
        // Timed backup sleep: rail up, BBR warm, self-wakes when due.
        MaxM10::sleep_for(*s, delay_ms);
      } else {
        power_off();
      }
      return;
    }
  }
}

// Drain the UART and drive the power-mode state machine. Call from
// the main loop; cheap when nothing's pending.
inline void pump() {
  HardwareSerial* s = _detail::serial_ref();
  if (!s) return;
  const PowerMode mode = target_mode();
  const uint32_t  now  = millis();
  Fix&            f    = _detail::fix_ref();

  // ---- power-mode transitions (location config only) ----
  if (mode == PowerMode::Off) {
    if (_detail::hw_powered_ref()) power_off();
    _detail::pulse_state_ref() = PulseState::Idle;
    return;                                         // no bytes will come; skip drain
  }
  if (mode == PowerMode::AlwaysOn) {
    if (!_detail::hw_powered_ref()) power_on();
    _detail::pulse_state_ref() = PulseState::Idle;
    // An M10 may still hold PSMOO from an earlier config (it persists
    // in RAM/BBR across ESP reboots while the rail stays up); put it
    // back to continuous operation, ACK-verified.
    if (module() == Module::MAXM10) {
      _detail::m10_request_power(M10Power::Full, 0);
    }
  }
  if (mode == PowerMode::Pulsed) {
    _detail::scheduler_pump(now, power_config().interval_s);
  }
  if (module() == Module::MAXM10) {
    _detail::m10_cfg_pump(now);
  }
  if (module() == Module::Unknown && _detail::hw_powered_ref()) {
    static uint32_t s_last_probe_ms = 0;
    if (now - s_last_probe_ms >= PROBE_RETRY_MS) {
      s_last_probe_ms = now;
      // Wake first in case the receiver is power-saving (the wake byte
      // is discarded; the probe frames behind it get through).
      MaxM10::wake(*s);
      probe_module();
    }
  }

  // ---- drain UART (only meaningful if powered) ----
  if (!_detail::hw_powered_ref()) return;
  while (s->available()) {
    const int c = s->read();
    if (c < 0) break;
    f.last_byte_ms = millis();
    // UBX frames interleave with NMEA on the M10 (and during the
    // identification window); the deframer consumes their bytes so
    // the line discipline below never sees them.
    if (module() != Module::L76K) {
      if (MaxM10::consume_byte((uint8_t)c, f)) {
        if (module() == Module::Unknown && MaxM10::seen_mon_ver()) {
          _detail::bind_module(Module::MAXM10, "UBX MON-VER reply");
        }
        continue;
      }
    }
    auto& buf  = _detail::line_buf_ref();
    auto& blen = _detail::line_len_ref();
    if (c == '\n') {
      buf[blen] = '\0';
      const char* txt = nullptr;
      const Nmea::Sentence kind = Nmea::parse_line(buf, blen, f, &txt);
      if (kind == Nmea::Sentence::RmcValid) {
        // Location bookkeeping (drives the power scheduler)...
        _detail::last_fix_ms_ref() = millis();
        _detail::ever_fixed_ref()  = true;
        // ...and clock bookkeeping (drives nothing but the RTC).
        _detail::maybe_report_time(f.unix_epoch);
      } else if (kind == Nmea::Sentence::Txt && txt != nullptr
                 && module() == Module::Unknown) {
        if (L76K::txt_is_casic(txt)) {
          _detail::bind_module(Module::L76K, "CASIC TXT banner");
        } else if (strstr(txt, "u-blox") != nullptr) {
          _detail::bind_module(Module::MAXM10, "u-blox TXT banner");
        }
      }
      blen = 0;
    }
    else if (blen + 1 < sizeof(buf)) {
      buf[blen++] = (char)c;
    }
    else {
      // Overrun - drop the line, resync on next \n.
      blen = 0;
    }
  }
  // Acquisition progress signal: any satellites in view keep the
  // current acquisition window open (see scheduler_pump).
  if (f.sats_visible > 0) _detail::sats_seen_ms_ref() = millis();
}

// Read access for /api/gps. Caller gets a copy of the current fix.
inline Fix last_fix() { return _detail::fix_ref(); }


inline bool has_serial() { return _detail::serial_ref() != nullptr; }

}  // namespace Gnss
}  // namespace Sensors
