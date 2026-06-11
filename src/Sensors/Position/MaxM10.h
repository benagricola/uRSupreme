// u-blox MAX-M10 driver - the UBX side of the GNSS stack.
//
// The M10 speaks NMEA like the L76K, so Nmea.h keeps feeding the Fix
// either way; what this driver adds is the UBX protocol the L76K does
// not have. All message and configuration constants below are from
// the u-blox M10 SPG 5.10 interface description (UBX-21035062 R03):
//   UBX-MON-VER    0x0a 0x04          (identification poll)
//   UBX-NAV-PVT    0x01 0x07, 92 B    (accuracy + fix detail, 3.15.11)
//   UBX-CFG-VALSET 0x06 0x8a          (configuration, 3.10.5)
//   UBX-MON-RF     0x0a 0x38, 4+24n B (RF + jamming status, 3.14.6)
//   CFG-MSGOUT-UBX_NAV_PVT_UART1  0x20910007 U1   (p.107)
//   CFG-MSGOUT-UBX_MON_RF_UART1   0x2091035a U1
//   CFG-PM-OPERATEMODE            0x20d00001 E1   FULL=0 PSMOO=1 PSMCT=2 (4.9.15)
//   CFG-PM-POSUPDATEPERIOD        0x40d00002 U4 s (>= 5 s)
//   CFG-PM-ACQPERIOD              0x40d00003 U4 s
//
// What we use it for:
//   * identification - a framed MON-VER reply IS the M10 handshake
//     (the CASIC core in the L76K ignores UBX entirely)
//   * UBX-NAV-PVT once per second - real horizontal/vertical accuracy
//     estimates (hAcc/vAcc, millimetres) and satellite count; the
//     NMEA RMC/GGA stream stays the source for position and time, so
//     a PVT parsing fault can never cost a fix
//   * UBX-MON-RF every few seconds - the jamming/interference
//     monitor (jammingState; live on SPG 5.10, which predates the
//     UBX-SEC-SIG replacement), CW suppression level and antenna
//     state, surfaced as diagnostics
//   * power: instead of the firmware rail-pulsing the module (which
//     destroys its backup RAM and forces cold starts), PSMOO lets the
//     receiver duty-cycle itself with the rail held up -
//     POSUPDATEPERIOD maps 1:1 onto the user's interval_s
//
// The deframer is streaming: payloads up to NAV-PVT size are
// buffered, larger frames (MON-VER replies run ~200 B) have their
// class/id noted and bytes skipped while the checksum is still
// verified, so identification needs no big buffer.

#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <stdint.h>
#include <string.h>

#include <Log.h>
#include "Nmea.h"

namespace Sensors {
namespace MaxM10 {

inline constexpr uint8_t  CLS_NAV = 0x01;
inline constexpr uint8_t  ID_NAV_PVT = 0x07;
inline constexpr uint8_t  CLS_MON = 0x0A;
inline constexpr uint8_t  ID_MON_VER = 0x04;
inline constexpr uint8_t  ID_MON_RF  = 0x38;
inline constexpr uint8_t  CLS_CFG = 0x06;
inline constexpr uint8_t  ID_CFG_VALSET = 0x8A;

inline constexpr uint32_t KEY_MSGOUT_NAV_PVT_UART1 = 0x20910007;
inline constexpr uint32_t KEY_MSGOUT_MON_RF_UART1  = 0x2091035a;
// Jamming/interference monitor master switch (CFG-ITFM, 4.9.9). Off
// by default; MON-RF's jammingState reads 0 (unknown) until enabled.
inline constexpr uint32_t KEY_ITFM_ENABLE          = 0x1041000d;
// MON-RF once every N navigation epochs - jamming status does not
// need 1 Hz.
inline constexpr uint8_t  MON_RF_RATE = 5;
inline constexpr uint32_t KEY_PM_OPERATEMODE       = 0x20d00001;
inline constexpr uint32_t KEY_PM_POSUPDATEPERIOD   = 0x40d00002;
inline constexpr uint32_t KEY_PM_ACQPERIOD         = 0x40d00003;
inline constexpr uint8_t  PM_FULL  = 0;
inline constexpr uint8_t  PM_PSMOO = 1;
// VALSET layers bitfield: bit0 RAM, bit1 BBR. Both, so the setting
// survives the receiver's own PSMOO backup phases.
inline constexpr uint8_t  VALSET_LAYERS = 0x03;
// POSUPDATEPERIOD's documented floor.
inline constexpr uint32_t PSMOO_MIN_PERIOD_S = 5;
// Retry acquisition once a minute if a scheduled fix fails.
inline constexpr uint32_t PSMOO_ACQ_PERIOD_S = 60;

namespace _detail {
  // Deframer state.
  enum class S : uint8_t { Sync1, Sync2, Cls, Id, Len1, Len2, Payload, Skip, CkA, CkB };
  struct Frame {
    S        st = S::Sync1;
    uint8_t  cls = 0, id = 0;
    uint16_t len = 0, got = 0;
    uint8_t  ck_a = 0, ck_b = 0;       // running Fletcher checksum
    uint8_t  want_a = 0, want_b = 0;   // received checksum
    uint8_t  payload[96];              // NAV-PVT is 92 B; larger frames skip
  };
  inline Frame& frame() { static Frame f; return f; }
  inline bool& seen_mon_ver() { static bool b = false; return b; }

  inline void ck(Frame& f, uint8_t b) { f.ck_a += b; f.ck_b += f.ck_a; }
}

// Build + send one UBX frame.
inline void send_frame(HardwareSerial& s, uint8_t cls, uint8_t id,
                       const uint8_t* payload, uint16_t len) {
  uint8_t hdr[4] = { cls, id, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
  uint8_t ck_a = 0, ck_b = 0;
  for (uint8_t b : hdr) { ck_a += b; ck_b += ck_a; }
  for (uint16_t i = 0; i < len; ++i) { ck_a += payload[i]; ck_b += ck_a; }
  s.write((uint8_t)0xB5); s.write((uint8_t)0x62);
  s.write(hdr, sizeof(hdr));
  if (len) s.write(payload, len);
  s.write(ck_a); s.write(ck_b);
}

inline void poll_mon_ver(HardwareSerial& s) {
  send_frame(s, CLS_MON, ID_MON_VER, nullptr, 0);
}

// CFG-VALSET with one or two key/value pairs (version 0, RAM+BBR).
inline void valset_u1(HardwareSerial& s, uint32_t key, uint8_t value) {
  uint8_t p[4 + 4 + 1] = { 0x00, VALSET_LAYERS, 0x00, 0x00 };
  memcpy(&p[4], &key, 4);     // little-endian, same as the ESP32
  p[8] = value;
  send_frame(s, CLS_CFG, ID_CFG_VALSET, p, sizeof(p));
}

inline void valset_u4(HardwareSerial& s, uint32_t key, uint32_t value) {
  uint8_t p[4 + 4 + 4] = { 0x00, VALSET_LAYERS, 0x00, 0x00 };
  memcpy(&p[4], &key, 4);
  memcpy(&p[8], &value, 4);
  send_frame(s, CLS_CFG, ID_CFG_VALSET, p, sizeof(p));
}

// Driver bring-up once the module is identified: ask for NAV-PVT once
// per navigation epoch on our UART.
inline void on_identified(HardwareSerial& s) {
  valset_u1(s, KEY_MSGOUT_NAV_PVT_UART1, 1);
  valset_u1(s, KEY_MSGOUT_MON_RF_UART1, MON_RF_RATE);
  valset_u1(s, KEY_ITFM_ENABLE, 1);
}

// Power strategy. full_power=true puts the receiver in FULL mode
// (continuous); otherwise PSMOO with the user's interval as the
// position update period - the receiver sleeps itself between fixes
// with the rail held up, keeping backup RAM warm for hot starts.
inline void set_power(HardwareSerial& s, bool full_power, uint32_t interval_s) {
  if (full_power) {
    valset_u1(s, KEY_PM_OPERATEMODE, PM_FULL);
    return;
  }
  uint32_t period = interval_s < PSMOO_MIN_PERIOD_S ? PSMOO_MIN_PERIOD_S : interval_s;
  valset_u4(s, KEY_PM_POSUPDATEPERIOD, period);
  valset_u4(s, KEY_PM_ACQPERIOD, PSMOO_ACQ_PERIOD_S);
  // OPERATEMODE last, per the integration manual's ordering note.
  valset_u1(s, KEY_PM_OPERATEMODE, PM_PSMOO);
}

inline bool seen_mon_ver() { return _detail::seen_mon_ver(); }

// RF / interference status from UBX-MON-RF block 0 (3.14.6).
// jamming_state: 0 unknown, 1 ok, 2 warning (interference visible,
// fix OK), 3 critical (interference visible, no fix). cw_jam: CW
// interference suppression level, 0 (none) .. 255 (strong). agc:
// automatic gain control monitor, 0..8191 = percentage of maximum
// receiver gain - meaningful RF health even with zero satellites.
struct RfStatus {
  bool     valid = false;
  uint8_t  jamming_state = 0;
  uint8_t  cw_jam = 0;
  uint8_t  ant_status = 0;
  uint16_t noise_per_ms = 0;
  uint16_t agc = 0;          // automatic gain control, 0..8191 = 100%
  uint32_t at_ms = 0;
};

namespace _detail {
  inline RfStatus& rf_ref() { static RfStatus r; return r; }
}

inline RfStatus rf_status() { return _detail::rf_ref(); }

inline void _handle_mon_rf(const uint8_t* p, uint16_t len) {
  // version@0, nBlocks@1; block 0: flags@5 (bits 1..0 jammingState),
  // antStatus@6, noisePerMS@16 U2, cwSuppression@20 U1.
  if (len < 4 + 24 || p[1] < 1) return;
  RfStatus& r = _detail::rf_ref();
  r.jamming_state = p[5] & 0x03;
  r.ant_status    = p[6];
  memcpy(&r.noise_per_ms, &p[16], 2);
  memcpy(&r.agc, &p[18], 2);
  r.cw_jam        = p[20];
  r.valid         = true;
  r.at_ms         = millis();
}

// NAV-PVT fields we consume (offsets per 3.15.11): fixType@20,
// flags@21 (bit0 gnssFixOK), numSV@23, hAcc@40 U4 mm, vAcc@44 U4 mm.
inline void _handle_nav_pvt(const uint8_t* p, uint16_t len, Nmea::Fix& f) {
  if (len < 92) return;
  const uint8_t  fix_type = p[20];
  const bool     fix_ok   = (p[21] & 0x01) != 0;
  uint32_t hacc_mm, vacc_mm;
  memcpy(&hacc_mm, &p[40], 4);
  memcpy(&vacc_mm, &p[44], 4);
  if (fix_type >= 2 && fix_ok) {
    f.sats      = p[23];
    f.hacc_m    = (float)hacc_mm / 1000.0f;
    f.vacc_m    = (float)vacc_mm / 1000.0f;
    f.acc_valid = true;
  } else {
    f.acc_valid = false;
  }
}

// Feed one received byte to the deframer. Returns true if the byte
// belonged to a UBX frame (so the caller's NMEA line discipline can
// ignore it). Complete NAV-PVT frames update `f`; a complete MON-VER
// reply latches seen_mon_ver() - the identification signal.
inline bool consume_byte(uint8_t b, Nmea::Fix& f) {
  using namespace _detail;
  Frame& fr = frame();
  switch (fr.st) {
    case S::Sync1:
      if (b == 0xB5) { fr.st = S::Sync2; return true; }
      return false;
    case S::Sync2:
      if (b == 0x62) { fr.st = S::Cls; fr.ck_a = fr.ck_b = 0; return true; }
      fr.st = S::Sync1;
      return false;
    case S::Cls:  fr.cls = b; ck(fr, b); fr.st = S::Id;   return true;
    case S::Id:   fr.id  = b; ck(fr, b); fr.st = S::Len1; return true;
    case S::Len1: fr.len = b; ck(fr, b); fr.st = S::Len2; return true;
    case S::Len2:
      fr.len |= (uint16_t)b << 8;
      ck(fr, b);
      fr.got = 0;
      if (fr.len == 0)                        fr.st = S::CkA;
      else if (fr.len <= sizeof(fr.payload))  fr.st = S::Payload;
      else                                    fr.st = S::Skip;
      return true;
    case S::Payload:
      fr.payload[fr.got++] = b;
      ck(fr, b);
      if (fr.got >= fr.len) fr.st = S::CkA;
      return true;
    case S::Skip:
      fr.got++;
      ck(fr, b);
      if (fr.got >= fr.len) fr.st = S::CkA;
      return true;
    case S::CkA: fr.want_a = b; fr.st = S::CkB; return true;
    case S::CkB:
      fr.want_b = b;
      fr.st = S::Sync1;
      if (fr.ck_a == fr.want_a && fr.ck_b == fr.want_b) {
        if (fr.cls == CLS_MON && fr.id == ID_MON_VER) {
          _detail::seen_mon_ver() = true;
        } else if (fr.cls == CLS_NAV && fr.id == ID_NAV_PVT
                   && fr.len <= sizeof(fr.payload)) {
          _handle_nav_pvt(fr.payload, fr.len, f);
        } else if (fr.cls == CLS_MON && fr.id == ID_MON_RF
                   && fr.len <= sizeof(fr.payload)) {
          _handle_mon_rf(fr.payload, fr.len);
        }
      }
      return true;
  }
  return false;
}

}  // namespace MaxM10
}  // namespace Sensors
