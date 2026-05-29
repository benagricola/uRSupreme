// Auto-extracted domain handlers for Web::WebUI — heap / allocation
// diagnostics. Included from inside the class body of Web::WebUI in
// WebUI.h: this file has NO include guard, NO `#pragma once`, and is not
// a standalone translation unit. The static methods below stay
// implicit-inline because they sit inside the class body via the
// surrounding #include directive.

    // GET /api/diag/mem — internal-SRAM / DMA / PSRAM headroom snapshot.
    //
    // Bearer-auth gated on purpose: heap internals are not public, and
    // /api/info (which IS served unauthed for the login screen) no
    // longer carries them. Two distinct troughs are reported:
    //   * min_free_internal — exact since-boot low, tracked by IDF
    //     inside the allocator (never misses a transient dip).
    //   * window_low         — resettable per-window low fed by the
    //     esp_timer sampler (Common::HeapWatermark). POST to reset it so
    //     a measurement window can be compared without rebooting.
    static void handle_diag_mem(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["uptime_ms"]         = (uint32_t)millis();
      doc["free_internal"]     = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      doc["largest_internal"]  = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
      doc["min_free_internal"] = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
      doc["free_dma"]          = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DMA);
      doc["largest_dma"]       = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
      doc["free_psram"]        = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      doc["window_low"]        = Common::HeapWatermark::window_low();
      doc["window_start_ms"]   = Common::HeapWatermark::window_start_ms();
      send_json(req, 200, doc);
    }

    // POST /api/diag/mem — reset the window-low marker to the current
    // free-internal and stamp window_start_ms = now. Body ignored.
    static void handle_diag_mem_reset(AsyncWebServerRequest* req, JsonVariant& /*body*/) {
      if (require_auth(req).empty()) return;
      const uint32_t seed = Common::HeapWatermark::mark();
      Common::PsramJsonDocument doc;
      doc["status"]     = "marked";
      doc["window_low"] = seed;
      send_json(req, 200, doc);
    }
