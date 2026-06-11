#if defined(URTN_HEAP_TRACE)
#include "HeapTrace.h"
#include <string.h>
#include "esp_heap_caps.h"
#include "soc/soc_memory_types.h"   // esp_ptr_internal (IDF 4.x)

extern "C" {
  void* __real_heap_caps_malloc(size_t size, uint32_t caps);
  void* __real_heap_caps_realloc(void* ptr, size_t size, uint32_t caps);
  void* __real_heap_caps_calloc(size_t n, size_t size, uint32_t caps);
  void  __real_heap_caps_free(void* ptr);
  void* __real_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
  void  __real_heap_caps_aligned_free(void* ptr);
  // newlib entry points (external to libheap.a, so app/lib references to them
  // ARE redirected - unlike intra-libheap calls to heap_caps_malloc).
  void* __real_malloc(size_t size);
  void* __real_calloc(size_t n, size_t size);
  void* __real_realloc(void* ptr, size_t size);
  void  __real_free(void* ptr);
}

namespace HeapTrace {

  // ptr is written/cleared last (release order) so a lock-free reader that
  // checks ptr first sees a consistent ra/size for live slots. Collisions
  // overwrite (lossy) - acceptable for a statistical leak finder, and avoids
  // any locking in the allocation hot path.
  struct Rec { void* ra0; void* ra1; uint32_t size; void* ptr; };
  static const uint32_t SLOTS = 1u << 16;   // 65536 slots * 16 B = 1 MB PSRAM
  static Rec* g_table = nullptr;
  static volatile bool g_active = false;

  static inline uint32_t slot_of(void* p) {
    uintptr_t x = ((uintptr_t)p) >> 4;       // drop alignment bits
    x *= 2654435761u;                         // Knuth multiplicative hash
    return (uint32_t)(x & (SLOTS - 1));
  }
  static inline void rec(void* p, void* ra0, void* ra1, uint32_t size) {
#if defined(URTN_HEAP_TRACE_MIN_SIZE) && (URTN_HEAP_TRACE_MIN_SIZE > 0)
    // Threshold-investigation mode: track allocations >= MIN_SIZE on ANY heap,
    // so PSRAM-via-config, PSRAM-via-explicit-allocator and forced-internal all
    // appear. Lets us see exactly what HEAP_EXTMEM_THRESHOLD routes off-chip.
    if (size < (uint32_t)URTN_HEAP_TRACE_MIN_SIZE) return;
#else
    // Leak-finder mode (default): only internal-SRAM allocations.
    if (!esp_ptr_internal(p)) return;
#endif
    Rec& r = g_table[slot_of(p)];
    r.ra0 = ra0; r.ra1 = ra1; r.size = size;
    __atomic_store_n(&r.ptr, p, __ATOMIC_RELEASE);
  }
  static inline void unrec(void* p) {
    Rec& r = g_table[slot_of(p)];
    if (r.ptr == p) __atomic_store_n(&r.ptr, (void*)0, __ATOMIC_RELEASE);
  }

  void init() {
    if (g_table) return;
    g_table = (Rec*)__real_heap_caps_malloc(SLOTS * sizeof(Rec), MALLOC_CAP_SPIRAM);
    if (g_table) {
      memset(g_table, 0, SLOTS * sizeof(Rec));
      g_active = true;
    }
  }

  int aggregate(Agg* out, int max) {
    if (!g_table) return 0;
    int n = 0;
    for (uint32_t i = 0; i < SLOTS; ++i) {
      void* p = __atomic_load_n(&g_table[i].ptr, __ATOMIC_ACQUIRE);
      if (!p) continue;
      void* ra0 = g_table[i].ra0;
      uint32_t sz = g_table[i].size;
      int j = 0;
      for (; j < n; ++j) if (out[j].ra0 == ra0) break;
      if (j == n) {
        if (n >= max) continue;
        out[n].ra0 = ra0; out[n].ra1 = g_table[i].ra1;
        out[n].count = 0; out[n].bytes = 0;
        out[n].internal = esp_ptr_internal(p);   // where this site's blocks land
        n++;
      }
      out[j].count++; out[j].bytes += sz;
    }
    for (int a = 0; a < n; ++a)
      for (int b = a + 1; b < n; ++b)
        if (out[b].bytes > out[a].bytes) { Agg t = out[a]; out[a] = out[b]; out[b] = t; }
    return n;
  }
}

extern "C" {

void* __wrap_heap_caps_malloc(size_t size, uint32_t caps) {
  void* p = __real_heap_caps_malloc(size, caps);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)size);
  return p;
}

void __wrap_heap_caps_free(void* ptr) {
  if (HeapTrace::g_active && ptr) HeapTrace::unrec(ptr);
  __real_heap_caps_free(ptr);
}

void* __wrap_heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
  if (HeapTrace::g_active && ptr) HeapTrace::unrec(ptr);
  void* p = __real_heap_caps_realloc(ptr, size, caps);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)size);
  return p;
}

void* __wrap_heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
  void* p = __real_heap_caps_calloc(n, size, caps);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)(n * size));
  return p;
}

void* __wrap_heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
  void* p = __real_heap_caps_aligned_alloc(alignment, size, caps);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)size);
  return p;
}

void __wrap_heap_caps_aligned_free(void* ptr) {
  if (HeapTrace::g_active && ptr) HeapTrace::unrec(ptr);
  __real_heap_caps_aligned_free(ptr);
}

// newlib entry points. Wrapping these catches the bulk of allocations (C++ new,
// lwIP/AsyncTCP via libc malloc) with the real caller as the innermost return
// address - the intra-libheap path to heap_caps_malloc is not --wrap-redirected.
void* __wrap_malloc(size_t size) {
  void* p = __real_malloc(size);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)size);
  return p;
}

void* __wrap_calloc(size_t n, size_t size) {
  void* p = __real_calloc(n, size);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)(n * size));
  return p;
}

void* __wrap_realloc(void* ptr, size_t size) {
  if (HeapTrace::g_active && ptr) HeapTrace::unrec(ptr);
  void* p = __real_realloc(ptr, size);
  if (HeapTrace::g_active && p)
    HeapTrace::rec(p, __builtin_return_address(0), __builtin_return_address(1), (uint32_t)size);
  return p;
}

void __wrap_free(void* ptr) {
  if (HeapTrace::g_active && ptr) HeapTrace::unrec(ptr);
  __real_free(ptr);
}

}
#endif
