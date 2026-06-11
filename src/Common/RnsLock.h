// Recursive mutex guarding all RNS / Reticulum library state.
//
// RNS isn't reentrant: the path table and known-destinations map can
// race against concurrent access from the WebServer task (core 0) and
// the main loop (core 1). The same lock is taken by WebUI handlers,
// the LXMFGateway loop, Discovery::Announcer's stamp callbacks, and
// the main loop's reticulum.loop() invocation - every code path that
// touches RNS state goes through this guard.
//
// Recursive so a handler that holds it can call any number of nested
// RNS helpers without deadlocking itself. Lives in Common/ (rather
// than Web/) because it's used from Discovery/, LXMF/, and the main
// loop - not just from the web layer.

#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>

namespace Common {
namespace RnsLock {

inline SemaphoreHandle_t& handle() {
  static SemaphoreHandle_t s = nullptr;
  if (!s) s = xSemaphoreCreateRecursiveMutex();
  return s;
}

inline bool acquire(uint32_t timeout_ms = portMAX_DELAY) {
  return xSemaphoreTakeRecursive(handle(),
                                 timeout_ms == portMAX_DELAY ? portMAX_DELAY
                                                             : pdMS_TO_TICKS(timeout_ms))
         == pdTRUE;
}

inline void release() {
  xSemaphoreGiveRecursive(handle());
}

// RAII helper. `ok` is false if acquire timed out; caller should check
// before touching RNS state.
struct Guard {
  bool ok;
  explicit Guard(uint32_t timeout_ms = portMAX_DELAY)
    : ok(acquire(timeout_ms)) {}
  ~Guard() { if (ok) release(); }
  explicit operator bool() const { return ok; }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};

}  // namespace RnsLock
}  // namespace Common
