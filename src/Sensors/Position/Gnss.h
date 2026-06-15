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
//   power           Source enable + interval come from the GPS time
//                   source config (Clock::Manager):
//                     disabled              -> rail off
//                     interval <  5 min     -> always on
//                     interval >= 5 min     -> duty-cycled
//                   Duty cycle strategy is per module: the L76K (and
//                   Unknown) get the firmware rail pulse (power up,
//                   acquire, report, power down, exponential backoff
//                   on failure); the MAX-M10 manages itself via PSMOO
//                   with POSUPDATEPERIOD = interval_s and the rail
//                   held up, which keeps its backup RAM warm for hot
//                   starts instead of cold-starting every pulse.
//
//   time            On a valid RMC whose epoch is fresh per the
//                   configured interval, reports to Clock::Manager -
//                   the GPS time-source policy, unchanged.

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

// If the user's GPS poll interval is at or above this, duty-cycle the
// receiver (rail pulse or PSMOO depending on the module). Below it,
// keep it continuously powered - frequent rail cycling would shred
// the L76K's cold-start budget.
inline constexpr uint32_t PULSE_THRESHOLD_S    = 5 * 60;     // 5 min
inline constexpr uint32_t PULSE_ACQUIRE_TIMEOUT_MS = 120000; // give up after 2 min hunting
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

// Decide the current target power mode from the user's GPS time-
// source config. Source enable-disable is the master switch; the
// interval picks always-on vs duty-cycled.
inline PowerMode target_mode() {
  const auto& cfg = Clock::Manager::get_config(Clock::Manager::Source::GPS);
  if (!cfg.enabled)                              return PowerMode::Off;
  // interval_s == 0 -> "boot-only": power on long enough to acquire one
  // fix + report it to TimeManager, then power down and stay off until
  // reboot. Without this the AlwaysOn branch below catches 0 (since
  // 0 < PULSE_THRESHOLD_S) and the module would burn battery
  // indefinitely while every subsequent fix gets thrown away.
  if (cfg.interval_s == 0) {
    return (_detail::last_report_ms_ref() != 0) ? PowerMode::Off
                                                : PowerMode::AlwaysOn;
  }
  if (cfg.interval_s < PULSE_THRESHOLD_S)        return PowerMode::AlwaysOn;
  return PowerMode::Pulsed;
}

namespace _detail {

  // Time-source policy: report a fresh RMC epoch to TimeManager when
  // the configured interval has elapsed (or on the very first fix).
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
  inline void pulse_strategy(uint32_t now, uint32_t interval_s) {
    const uint32_t last_rep = last_report_ms_ref();
    const uint32_t interval_ms = interval_s * 1000UL;
    const bool first_time = (last_rep == 0);
    const bool due        = first_time || ((now - last_rep) >= interval_ms);
    if (pulse_state_ref() == PulseState::Idle) {
      if (due) {
        power_on();
        pulse_state_ref()         = PulseState::Acquiring;
        pulse_started_ms_ref()    = now;
        pulse_last_rep_snapshot() = last_rep;   // pin pre-pulse value
        NOTICEF("GPS: pulse start (interval=%lus, last=%lums ago)",
                (unsigned long)interval_s,
                (unsigned long)(first_time ? 0 : (now - last_rep)));
      } else {
        // Idle between polls - make sure power is off.
        if (hw_powered_ref()) power_off();
      }
    } else /* Acquiring */ {
      // A fresh report happened *during this pulse* iff
      // maybe_report_time bumped last_report_ms_ref above its
      // pre-pulse snapshot.
      const bool acquired = (last_rep != pulse_last_rep_snapshot());
      const bool timed_out = (now - pulse_started_ms_ref())
                              > PULSE_ACQUIRE_TIMEOUT_MS;
      if (acquired) {
        NOTICEF("GPS: pulse acquired in %lums",
                (unsigned long)(now - pulse_started_ms_ref()));
        backoff_count_ref() = 0;
        power_off();
        pulse_state_ref() = PulseState::Idle;
      } else if (timed_out) {
        // No fix within the window. Exponential backoff: base * 2^N,
        // capped. Indoors with no sky view this lifts retry cadence
        // from once a minute to once every ~30 min. Motion through
        // the IMU calls reset_backoff() to drop back to base.
        uint8_t& n = backoff_count_ref();
        const uint8_t shift = (n < PULSE_RETRY_BACKOFF_SHIFT_MAX) ? n : PULSE_RETRY_BACKOFF_SHIFT_MAX;
        uint32_t delay_ms = PULSE_RETRY_BACKOFF_MS << shift;
        if (delay_ms > PULSE_RETRY_BACKOFF_MAX_MS) delay_ms = PULSE_RETRY_BACKOFF_MAX_MS;
        if (n < 255) n++;
        WARNINGF("GPS: pulse timed out after %lums; backing off %lums (attempt %u)",
                 (unsigned long)PULSE_ACQUIRE_TIMEOUT_MS,
                 (unsigned long)delay_ms,
                 (unsigned)n);
        power_off();
        pulse_state_ref() = PulseState::Idle;
        // Set last_report_ms so the next due-check fires `delay_ms`
        // from now rather than interval_s from "never".
        if (interval_ms > delay_ms) {
          last_report_ms_ref() = now - (interval_ms - delay_ms);
        } else {
          last_report_ms_ref() = now;
        }
      }
    }
  }

  // The MAX-M10 duty-cycle strategy: rail stays up, the receiver
  // sleeps itself (PSMOO). POSUPDATEPERIOD maps 1:1 onto the user's
  // interval. Reapplied whenever the rail has cycled or the interval
  // changed.
  inline void psmoo_strategy(uint32_t interval_s) {
    if (!hw_powered_ref()) power_on();
    pulse_state_ref() = PulseState::Idle;
    HardwareSerial* s = serial_ref();
    if (s == nullptr) return;
    if (m10_power_ref() != M10Power::Psmoo || m10_interval_ref() != interval_s) {
      MaxM10::set_power(*s, /*full_power=*/false, interval_s);
      m10_power_ref()    = M10Power::Psmoo;
      m10_interval_ref() = interval_s;
      NOTICEF("GPS: MAX-M10 PSMOO, position update every %lus",
              (unsigned long)interval_s);
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
  const auto&     cfg  = Clock::Manager::get_config(Clock::Manager::Source::GPS);

  // ---- power-mode transitions ----
  if (mode == PowerMode::Off) {
    if (_detail::hw_powered_ref()) power_off();
    _detail::pulse_state_ref() = PulseState::Idle;
    return;                                         // no bytes will come; skip drain
  }
  if (mode == PowerMode::AlwaysOn) {
    if (!_detail::hw_powered_ref()) power_on();
    _detail::pulse_state_ref() = PulseState::Idle;
    // An M10 may still hold PSMOO from an earlier config (it persists
    // in RAM/BBR); put it back to continuous operation.
    if (module() == Module::MAXM10 &&
        _detail::m10_power_ref() != M10Power::Full) {
      MaxM10::set_power(*s, /*full_power=*/true, 0);
      _detail::m10_power_ref() = M10Power::Full;
    }
  }
  if (mode == PowerMode::Pulsed) {
    if (module() == Module::MAXM10) {
      _detail::psmoo_strategy(cfg.interval_s);
    } else {
      _detail::pulse_strategy(now, cfg.interval_s);
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
}

// Read access for /api/gps. Caller gets a copy of the current fix.
inline Fix last_fix() { return _detail::fix_ref(); }

inline bool has_serial() { return _detail::serial_ref() != nullptr; }

}  // namespace Gnss
}  // namespace Sensors
