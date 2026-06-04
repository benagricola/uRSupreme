// Internal-SRAM allocation-site tracker (build-flag gated: URTN_HEAP_TRACE).
//
// The precompiled Arduino SDK ships with CONFIG_HEAP_TRACING_OFF, so the
// standard heap_trace API is unavailable. Instead we intercept the heap
// allocators at link time and keep a lossy, lock-free, direct-mapped table (in
// PSRAM) of every live INTERNAL-SRAM block keyed by pointer, recording the two
// innermost return addresses. Aggregating live bytes per caller and diffing two
// snapshots (GET /api/diag/heaptrace) taken minutes apart reveals the allocation
// site whose live bytes only ever grow - i.e. the leak. addr2line then maps the
// address to a source line.
//
// Two modes, selected by URTN_HEAP_TRACE_MIN_SIZE:
//   * unset / 0 (default) - leak-finder: track only INTERNAL-SRAM blocks, on
//     any size. This is the mode for chasing internal-SRAM growth.
//   * N > 0 - threshold survey: track every block >= N bytes on ANY heap and
//     record where it landed (internal vs PSRAM). Used to see exactly what
//     HEAP_EXTMEM_THRESHOLD routes off-chip vs what an explicit allocator does.
//
// IMPORTANT: --wrap only redirects INTER-object references, so wrapping
// heap_caps_malloc alone misses the malloc -> heap_caps_malloc_default ->
// heap_caps_malloc path (all intra-libheap.a). The newlib entry points must be
// wrapped too. Build (and flash) with:
//
//   PLATFORMIO_BUILD_FLAGS="$BASE -DURTN_HEAP_TRACE \
//     -Wl,--wrap=heap_caps_malloc -Wl,--wrap=heap_caps_realloc \
//     -Wl,--wrap=heap_caps_free -Wl,--wrap=heap_caps_calloc \
//     -Wl,--wrap=heap_caps_aligned_alloc -Wl,--wrap=heap_caps_aligned_free \
//     -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free"
//
// It is gated off by default (adds an alloc-path branch + a PSRAM write per
// internal allocation) and is a diagnostic only.
#pragma once

#if defined(URTN_HEAP_TRACE)
#include <stdint.h>
#include <stddef.h>

namespace HeapTrace {
  // Allocate the PSRAM tracking table and begin recording. Call once, early,
  // after PSRAM is up. Allocations before this are untracked (harmless).
  void init();

  struct Agg { void* ra0; void* ra1; uint32_t count; uint32_t bytes; bool internal; };
  // Aggregate live blocks by innermost return address into `out` (capacity
  // `max`), sorted by bytes descending. Returns the number of distinct sites.
  int aggregate(Agg* out, int max);
}
#endif
