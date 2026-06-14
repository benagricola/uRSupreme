// Copyright (C) 2026, Mark Qvist

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

#pragma once

// PSRAM-backed allocator for ArduinoJson v7. ArduinoJson 7's `Allocator`
// is a public abstract base; passing `JsonDocument(Allocator*)` routes
// every internal allocation through our overrides.
//
// Why this matters: ArduinoJson is the dominant source of small,
// transient heap allocations in this firmware. Every HTTP handler and
// every WebSocket broadcast constructs a JsonDocument that grows by
// allocating from the default heap (= internal SRAM under arduino-esp32
// v2.0.17's default allocator settings). Under web load these allocs
// fragment internal SRAM into many small holes - at which point the
// ESP-IDF WiFi driver's `esf_buf_alloc_dynamic(1626)` fails because no
// contiguous 1.6 KB block is available even though total free is
// nominally higher. Symptom: HTTP becomes unresponsive, the device
// looks hung until a full reset.
//
// Moving JsonDocument's backing store to PSRAM removes the small-alloc
// churn from internal SRAM entirely. The JsonDocument *object* itself
// (a small stack-frame holder of ~50 B) still lives where the caller
// placed it; only its dynamically allocated nodes go to PSRAM.
//
// Use via the PsramJsonDocument typedef below: `PsramJsonDocument doc;`
// is a drop-in replacement for `JsonDocument doc;` - same API, same
// operators, same serializeJson/deserializeJson semantics. Functions
// that accept `const JsonDocument&` or `JsonDocument&` parameters still
// work unchanged because PsramJsonDocument *is-a* JsonDocument.

#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <vector>
#include <new>
#include <cstddef>

namespace Common {

class PsramAllocator : public ArduinoJson::Allocator {
public:
  void* allocate(size_t size) override {
    // MALLOC_CAP_SPIRAM forces the allocation to PSRAM. If PSRAM is
    // exhausted (very unlikely - we have 8 MB) the call returns NULL
    // and ArduinoJson degrades to its low-memory mode (the document
    // grows up to but not past available memory).
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* ptr) override {
    // heap_caps_free is needed when the allocation came from a
    // non-default caps region; PSRAM blocks live in a different region
    // from the default heap.
    heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t new_size) override {
    // heap_caps_realloc with the same caps mask keeps the relocation
    // in PSRAM rather than potentially moving it back to internal SRAM.
    return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
  }

  static PsramAllocator* instance() {
    static PsramAllocator a;
    return &a;
  }
};

// Drop-in replacement for ArduinoJson::JsonDocument that defaults to
// PSRAM-backed storage. Inherits all API; passing a PsramJsonDocument
// to a function expecting JsonDocument& works via polymorphism (the
// allocator pointer is a runtime member, not a virtual).
class PsramJsonDocument : public ::ArduinoJson::JsonDocument {
public:
  PsramJsonDocument()
    : ::ArduinoJson::JsonDocument(PsramAllocator::instance()) {}
};

// Minimal PSRAM-backed std:: allocator, for large containers (the map
// extractor's tile-entry tables) that would otherwise fragment internal
// SRAM. Same rationale as PsramAllocator above; throws std::bad_alloc on
// exhaustion (exceptions are enabled on the Supreme build), so callers
// can fail the operation cleanly instead of dereferencing null.
template <class T>
struct PsramStdAllocator {
  using value_type = T;
  PsramStdAllocator() = default;
  template <class U> PsramStdAllocator(const PsramStdAllocator<U>&) noexcept {}
  T* allocate(std::size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM);
    if (!p) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }
  template <class U> bool operator==(const PsramStdAllocator<U>&) const noexcept { return true; }
  template <class U> bool operator!=(const PsramStdAllocator<U>&) const noexcept { return false; }
};

template <class T> using PsramVector = std::vector<T, PsramStdAllocator<T>>;

} // namespace Common
