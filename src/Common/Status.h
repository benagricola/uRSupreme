#pragma once

// Common::Status - a transient-status-message bus for surfaces that
// want to communicate operational state to the user.
//
// Producers (anywhere in the firmware):
//
//     Common::Status::say("WiFi: connecting to MyNetwork…");
//     Common::Status::say("WiFi: connected, IP 192.168.1.42", /*ttl_ms=*/5000);
//     Common::Status::clear();
//
// Consumers (currently: nobody - the OLED integration is deliberately
// deferred until a display layout has been chosen). The intended sink
// is a small scrolling marquee on the OLED, rendered in the same
// Picopixel font the identity-code page uses for the 6-char code
// (Display.h:852). Picopixel is monospace and teeny, which is exactly
// the constraint on a status strip - fits the most characters per
// line on a 128×64 panel and reads cleanly without leaning.
// The marquee can scroll horizontally for messages wider than the
// screen's effective text width.
//
// A future SPA-side consumer (e.g. a topbar tag in the chat UI) can
// poll latest() or subscribe via WebSocket; the API surface is the
// same shape regardless.
//
// Threading: producers may call from any task (HTTP handler, main
// loop, sensor reader). The internal state is guarded by a small
// FreeRTOS mutex. Consumers should call get/latest from the main
// loop only - that's the only place we know the OLED canvas is safe
// to touch.
//
// Memory: messages are copied into a fixed-size ring (8 slots, 64
// bytes each = 512 B of internal SRAM) so producers don't have to
// worry about pointer lifetime. Older entries are evicted FIFO when
// the ring is full.

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <cstring>
#include <cstdint>

namespace Common {
namespace Status {

  static constexpr size_t   MAX_MESSAGE_LEN = 64;
  static constexpr size_t   RING_DEPTH      = 8;
  static constexpr uint32_t DEFAULT_TTL_MS  = 0;   // 0 == sticky until cleared / replaced

  struct Message {
    char     text[MAX_MESSAGE_LEN];   // null-terminated; truncated at MAX_MESSAGE_LEN-1
    uint32_t posted_ms;
    uint32_t ttl_ms;                  // 0 means "no expiry"
  };

  namespace _detail {
    // Forward declarations - defined in the inline impls below so this
    // stays header-only (consistent with the rest of the project's
    // Common/* style).
    inline SemaphoreHandle_t& mtx_handle() {
      static SemaphoreHandle_t mtx = nullptr;
      return mtx;
    }
    inline void ensure_mtx() {
      if (mtx_handle() == nullptr) {
        mtx_handle() = xSemaphoreCreateMutex();
      }
    }
    struct Ring {
      Message  slots[RING_DEPTH];
      uint8_t  count = 0;       // number of live slots, 0..RING_DEPTH
      uint8_t  head  = 0;       // index of the newest slot
    };
    inline Ring& ring() {
      static Ring r;
      return r;
    }
  }

  // Post a status message. `text` is copied; safe to pass a String's
  // c_str(), a stack buffer, anything. ttl_ms=0 means the message is
  // sticky until something else replaces it (or clear() is called).
  inline void say(const char* text, uint32_t ttl_ms = DEFAULT_TTL_MS) {
    if (text == nullptr) return;
    _detail::ensure_mtx();
    if (xSemaphoreTake(_detail::mtx_handle(), pdMS_TO_TICKS(50)) != pdTRUE) return;
    auto& r = _detail::ring();
    uint8_t next_head = (r.head + 1) % RING_DEPTH;
    Message& m = r.slots[next_head];
    strncpy(m.text, text, MAX_MESSAGE_LEN - 1);
    m.text[MAX_MESSAGE_LEN - 1] = '\0';
    m.posted_ms = millis();
    m.ttl_ms    = ttl_ms;
    r.head      = next_head;
    if (r.count < RING_DEPTH) r.count++;
    xSemaphoreGive(_detail::mtx_handle());
  }

  // Convenience overload for Arduino String / std::string fluency.
  inline void say(const String& s, uint32_t ttl_ms = DEFAULT_TTL_MS) {
    say(s.c_str(), ttl_ms);
  }

  // Replace the most recent message in place (no new ring slot) - use
  // when you're updating a long-running operation's progress and don't
  // want to spam history (e.g. "AP closing in 2:00" → "1:59" → …).
  inline void update(const char* text, uint32_t ttl_ms = DEFAULT_TTL_MS) {
    if (text == nullptr) return;
    _detail::ensure_mtx();
    if (xSemaphoreTake(_detail::mtx_handle(), pdMS_TO_TICKS(50)) != pdTRUE) return;
    auto& r = _detail::ring();
    if (r.count == 0) {
      // Nothing to update - promote to a regular say().
      uint8_t next_head = (r.head + 1) % RING_DEPTH;
      r.head = next_head;
      r.count = 1;
    }
    Message& m = r.slots[r.head];
    strncpy(m.text, text, MAX_MESSAGE_LEN - 1);
    m.text[MAX_MESSAGE_LEN - 1] = '\0';
    m.posted_ms = millis();
    m.ttl_ms    = ttl_ms;
    xSemaphoreGive(_detail::mtx_handle());
  }

  // Drop all messages. Useful when an error condition is resolved and
  // we want the status surface to go quiet.
  inline void clear() {
    _detail::ensure_mtx();
    if (xSemaphoreTake(_detail::mtx_handle(), pdMS_TO_TICKS(50)) != pdTRUE) return;
    _detail::ring().count = 0;
    xSemaphoreGive(_detail::mtx_handle());
  }

  // Returns the newest non-expired message into `out` (length-truncated
  // to out_cap-1 + null terminator). Returns false if the ring is empty
  // or the newest entry has expired (and walks back through the ring
  // until it finds a live one). out is left untouched on false.
  inline bool latest(char* out, size_t out_cap) {
    if (out == nullptr || out_cap == 0) return false;
    _detail::ensure_mtx();
    if (xSemaphoreTake(_detail::mtx_handle(), pdMS_TO_TICKS(50)) != pdTRUE) return false;
    auto& r = _detail::ring();
    bool found = false;
    const uint32_t now = millis();
    for (uint8_t i = 0; i < r.count; ++i) {
      uint8_t idx = (r.head + RING_DEPTH - i) % RING_DEPTH;
      const Message& m = r.slots[idx];
      if (m.ttl_ms == 0 || (now - m.posted_ms) < m.ttl_ms) {
        strncpy(out, m.text, out_cap - 1);
        out[out_cap - 1] = '\0';
        found = true;
        break;
      }
    }
    xSemaphoreGive(_detail::mtx_handle());
    return found;
  }

  // Copy up to `cap` non-expired entries into `out`, newest first.
  // Returns the count actually written. Use for the SPA polling
  // endpoint or for the future OLED ticker that wants a queue, not
  // just the latest.
  inline size_t snapshot(Message* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    _detail::ensure_mtx();
    if (xSemaphoreTake(_detail::mtx_handle(), pdMS_TO_TICKS(50)) != pdTRUE) return 0;
    auto& r = _detail::ring();
    const uint32_t now = millis();
    size_t written = 0;
    for (uint8_t i = 0; i < r.count && written < cap; ++i) {
      uint8_t idx = (r.head + RING_DEPTH - i) % RING_DEPTH;
      const Message& m = r.slots[idx];
      if (m.ttl_ms == 0 || (now - m.posted_ms) < m.ttl_ms) {
        out[written++] = m;
      }
    }
    xSemaphoreGive(_detail::mtx_handle());
    return written;
  }

}  // namespace Status
}  // namespace Common
