// AXP2101 PMU driver — raw accessors over the global XPowersLib
// interface. Owns nothing; the actual PMU instance is created and
// initialised by Power.h at boot and exposed via the `PMU` global. We
// just wrap that here so callers don't have to touch XPowersLib types
// directly (and so swapping in a different PMU later is a one-file
// change).
//
// `Telemetry::Battery` consumes this to produce the higher-level
// dV/dt sampling + Snapshot struct the SPA reads.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <XPowersLib.h>

extern XPowersLibInterface* PMU;

namespace Sensors {
namespace AXP2101 {

inline bool present() { return PMU != nullptr; }

inline uint16_t voltage_mv() {
  if (!PMU) return 0;
  return PMU->getBattVoltage();
}

inline bool battery_connected() {
  if (!PMU) return false;
  return PMU->isBatteryConnect();
}

inline bool is_charging()    { return PMU && PMU->isCharging(); }
inline bool is_discharging() { return PMU && PMU->isDischarge(); }
inline bool vbus_in()        { return PMU && PMU->isVbusIn(); }

inline uint16_t vbus_voltage_mv() {
  if (!PMU) return 0;
  return PMU->getVbusVoltage();
}

inline uint8_t chip_model() {
  if (!PMU) return 0;
  return PMU->getChipModel();
}

// AXP2101 die temperature. The 18650 cell on T-Beam Supreme has no
// thermistor wired to the PMU, so this reads the PMU IC itself —
// useful for spotting heat on charge or hot-load. Power.h enables
// the internal temp ADC channel at boot. Returns 0 and sets *ok to
// false on unsupported chips.
inline float temperature_c(bool* ok = nullptr) {
  if (PMU && PMU->getChipModel() == XPOWERS_AXP2101) {
    const float t = ((XPowersAXP2101*)PMU)->getTemperature();
    if (t > -40.0f && t < 125.0f) {
      if (ok) *ok = true;
      return t;
    }
  }
  if (ok) *ok = false;
  return 0.0f;
}

// AXP192-only: actual battery discharge current (mA). The AXP2101
// the T-Beam Supreme ships with does NOT expose this — callers should
// fall back to dV/dt slope inference (see Telemetry::Battery) on this
// hardware. Returns 0 and sets *ok to false on AXP2101 / no PMU.
inline float discharge_ma(bool* ok = nullptr) {
  if (PMU && PMU->getChipModel() == XPOWERS_AXP192) {
    if (ok) *ok = true;
    return ((XPowersAXP192*)PMU)->getBattDischargeCurrent();
  }
  if (ok) *ok = false;
  return 0.0f;
}

}  // namespace AXP2101
}  // namespace Sensors
