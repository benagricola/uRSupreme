#pragma once

// Passive internal-SRAM low-water tracker.
//
// Sampled periodically from the IDF esp_timer service task (see
// RNode_Firmware.ino setup) so there is no dedicated task and no extra
// stack: the esp_timer task already exists in every IDF/Arduino build.
// One heap read per tick, no allocation, no lock held by us.
//
// Two distinct numbers are exposed over /api/diag/mem:
//   * min_free_internal  — heap_caps_get_minimum_free_size(), the EXACT
//     since-boot trough. IDF updates it inside the allocator at the
//     moment of each allocation, so it never misses a transient dip.
//   * window_low          — the lowest free-internal this sampler has
//     observed since the last mark(). Resettable (POST /api/diag/mem),
//     so a measurement window can be compared without rebooting. It is
//     sample-rate-limited (a sub-interval spike can be missed), which is
//     fine precisely because min_free_internal catches spikes exactly.
//
// For a fully rigorous per-window number, reboot before the window: the
// since-boot minimum then equals the window minimum.

#include <stdint.h>
#include <Arduino.h>          // millis()
#include <esp_heap_caps.h>

namespace Common {

  class HeapWatermark {
  public:
    // High-frequency hook. Safe to call from the esp_timer task context.
    static void sample() {
      const uint32_t f = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      if (f < s_window_low) s_window_low = f;
    }

    // Reset the per-window trough to "now". Returns the seed value.
    static uint32_t mark() {
      s_window_low   = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      s_window_start = millis();
      return s_window_low;
    }

    static uint32_t window_low()      { return s_window_low; }
    static uint32_t window_start_ms() { return s_window_start; }

  private:
    // Seeds to UINT32_MAX so the first sample() (before any mark()) still
    // captures the real free-internal as the initial low.
    static uint32_t s_window_low;
    static uint32_t s_window_start;
  };

  inline uint32_t HeapWatermark::s_window_low   = UINT32_MAX;
  inline uint32_t HeapWatermark::s_window_start = 0;

}
