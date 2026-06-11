// Quectel L76K driver - the CASIC side of the GNSS stack.
//
// The L76K is an NMEA receiver on a CASIC core; everything position-
// and time-shaped already happens in Nmea.h and the Gnss.h facade.
// What is genuinely L76K-specific lives here:
//
//   identification  $PCAS06,0 asks the CASIC core for product info;
//                   it answers in $GPTXT. The core also prints
//                   "CASIC"/"URANUS" TXT banners at power-on. A
//                   MAX-M10 ignores $PCAS entirely.
//
//   power           The L76K has CASIC standby commands ($PCAS12),
//                   but its backup domain is not wired usefully for
//                   us, so the duty-cycle strategy is the generic
//                   firmware rail pulse in Gnss.h - power the rail,
//                   acquire, report, cut the rail, with exponential
//                   backoff while there is no sky view.

#pragma once

#include <Arduino.h>
#include <HardwareSerial.h>
#include <string.h>

namespace Sensors {
namespace L76K {

// CASIC product-information query; the reply (and the power-on
// banners) arrive as $GPTXT sentences. Checksum precomputed for the
// fixed body "PCAS06,0".
inline void query_product_info(HardwareSerial& s) {
  s.print("$PCAS06,0*1B\r\n");
}

// Does a TXT sentence body identify a CASIC-cored module?
inline bool txt_is_casic(const char* txt) {
  if (txt == nullptr) return false;
  return strstr(txt, "CASIC")   != nullptr
      || strstr(txt, "URANUS")  != nullptr
      || strstr(txt, "Quectel") != nullptr;
}

}  // namespace L76K
}  // namespace Sensors
