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
      // Leak-vs-fragmentation discriminator. int_allocated rising over time is
      // a real internal-SRAM leak (something holds ever more bytes). Flat
      // int_allocated with int_free_blocks rising and largest_internal falling
      // is pure fragmentation (same bytes, more scattered). int_alloc_blocks
      // rising flags accumulation of many small long-lived allocations.
      multi_heap_info_t hi;
      heap_caps_get_info(&hi, MALLOC_CAP_INTERNAL);
      doc["int_allocated"]     = (uint32_t)hi.total_allocated_bytes;
      doc["int_alloc_blocks"]  = (uint32_t)hi.allocated_blocks;
      doc["int_free_blocks"]   = (uint32_t)hi.free_blocks;
      // WebSocket backpressure: a growing aggregate send-queue means the client
      // drains slower than we broadcast, so AsyncWebSocket/AsyncTCP retain
      // frames (internal-SRAM message objects + TX buffers) - a prime suspect
      // for internal-SRAM growth that tracks WS activity.
      size_t ws_queue = 0; uint32_t ws_clients = 0;
      for (const auto& c : Web::WS::clients()) {
        if (!c.authed) continue;
        ws_clients++;
        AsyncWebSocketClient* wc = Web::WS::server().client(c.client_id);
        if (wc) ws_queue += wc->queueLen();
      }
      doc["ws_clients"]        = ws_clients;
      doc["ws_queue"]          = (uint32_t)ws_queue;
      // Outbound LoRa packets dropped because the TX ring was full (radio
      // couldn't keep up — e.g. a duty-cycle airtime lock holding the queue).
      // Non-zero + climbing means user/announce TX is being silently lost.
      doc["lora_tx_dropped"]   = (uint32_t)lora_tx_dropped;
      // Bytes currently held in the TX flow-control queue (frames waiting for
      // ring space rather than being dropped). Climbs during a burst / airtime
      // lock and drains back to 0 as the radio catches up; a held frame that
      // would have been a drop in the old code.
      doc["lora_tx_held"]      = (uint32_t)tx_hold_bytes;
      // Announce egress health. `drained` climbing over time proves queued
      // re-broadcasts are reaching the wire (the headline transport-port fix);
      // `queued`/`queued_max` are the live per-interface egress backlog (sitting
      // near MAX_QUEUED_ANNOUNCES under heavy traffic is expected rate-limiting,
      // *not* a fault, as long as `drained` keeps rising).
      uint32_t aq_total = 0, aq_max = 0, aq_ifaces = 0, br_max = 0;
      for (auto& kv : RNS::Transport::get_interfaces()) {
        uint32_t n = (uint32_t)kv.second.announce_queue().size();
        aq_total += n;
        if (n > aq_max) aq_max = n;
        uint32_t br = kv.second.bitrate();
        if (br > br_max) br_max = br;
        aq_ifaces++;
      }
      JsonObject ae = doc["announce_egress"].to<JsonObject>();
      ae["queued"]      = aq_total;
      ae["queued_max"]  = aq_max;
      ae["interfaces"]  = aq_ifaces;
      ae["drained"]     = RNS::Interface::drained_announces();
      ae["bitrate_max"] = br_max;
      ae["rate_blocks"] = RNS::Transport::announce_rate_blocks();
      ae["rate_table"]  = (uint32_t)RNS::Transport::get_announce_rate_table().size();
      // Bounded path-request table (was the dominant internal-SRAM leak; should
      // now hold at its cap instead of growing toward tens of thousands).
      ae["path_requests"] = (uint32_t)RNS::Transport::path_requests_size();
      // Inbound safety drops (malformed/misflagged packets rejected before
      // parsing). `ifac` should stay ~0 on the open backbone network; a climbing
      // value would mean legitimate traffic is being dropped by the IFAC-flag
      // guard, so it is worth watching during a soak.
      JsonObject idr = doc["inbound_drops"].to<JsonObject>();
      idr["runt"] = RNS::Transport::runt_drops();
      idr["ifac"] = RNS::Transport::ifac_flagged_drops();
#if defined(URTN_REBROADCAST_DIAG)
      // Announce re-broadcast leak instrumentation. `live` per site (allocs-frees)
      // growing pinpoints which site leaks; live_hashes lists un-freed objects by
      // announce-hash prefix so individual announces can be followed.
      JsonObject rb = doc["rebroadcast"].to<JsonObject>();
      static const char* RB_NAMES[3] = { "announce_entry", "retx_dest", "retx_packet" };
      for (int s = 0; s < 3; s++) {
        JsonObject so = rb[RB_NAMES[s]].to<JsonObject>();
        so["live"]   = RNS::RebroadcastDiag::live((RNS::RebroadcastDiag::Site)s);
        so["allocs"] = RNS::RebroadcastDiag::allocs((RNS::RebroadcastDiag::Site)s);
        so["frees"]  = RNS::RebroadcastDiag::frees((RNS::RebroadcastDiag::Site)s);
      }
      rb["overflow"]        = RNS::RebroadcastDiag::overflow();
      rb["untracked_frees"] = RNS::RebroadcastDiag::untracked_frees();
      rb["live_count"]      = RNS::RebroadcastDiag::live_count();
      RNS::RebroadcastDiag::Live snap[40];
      int sn = RNS::RebroadcastDiag::snapshot(snap, 40);
      JsonArray live_hashes = rb["live_hashes"].to<JsonArray>();
      for (int i = 0; i < sn; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x:%u",
                 snap[i].hash[0], snap[i].hash[1], snap[i].hash[2],
                 snap[i].hash[3], snap[i].hash[4], snap[i].hash[5], snap[i].site);
        live_hashes.add(buf);
      }
#endif
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

#if defined(URTN_HEAP_TRACE)
    // GET /api/diag/heaptrace — live allocations aggregated by caller (innermost
    // two return addresses), sorted by bytes, each tagged `where`:internal|psram.
    // In leak-finder mode (default) only internal-SRAM blocks appear; fetch twice
    // minutes apart and the site whose `bytes` grows is the leak. In threshold-
    // survey mode (URTN_HEAP_TRACE_MIN_SIZE>0) every block >= that size appears
    // with its landing heap. Decode ra0/ra1 with xtensa-esp32s3-elf-addr2line.
    static void handle_diag_heaptrace(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      static HeapTrace::Agg aggs[256];
      int n = HeapTrace::aggregate(aggs, 256);
      uint32_t total = 0, psram_bytes = 0, int_bytes = 0;
      for (int i = 0; i < n; i++) {
        total += aggs[i].bytes;
        if (aggs[i].internal) int_bytes += aggs[i].bytes; else psram_bytes += aggs[i].bytes;
      }
      JsonArray sites = doc["sites"].to<JsonArray>();
      for (int i = 0; i < n && i < 200; i++) {
        JsonObject o = sites.add<JsonObject>();
        char b0[16], b1[16];
        snprintf(b0, sizeof(b0), "%p", aggs[i].ra0);
        snprintf(b1, sizeof(b1), "%p", aggs[i].ra1);
        o["ra0"]   = b0;
        o["ra1"]   = b1;
        o["count"] = aggs[i].count;
        o["bytes"] = aggs[i].bytes;
        o["avg"]   = aggs[i].count ? aggs[i].bytes / aggs[i].count : 0;
        o["where"] = aggs[i].internal ? "internal" : "psram";
      }
      doc["distinct_sites"] = n;
      doc["tracked_bytes"]  = total;
      doc["psram_bytes"]    = psram_bytes;
      doc["internal_bytes"] = int_bytes;
      send_json(req, 200, doc);
    }
#endif

#if defined(URTN_LOOP_DIAG)
    // GET /api/diag/loop — per-section maxima (microseconds) of the main loop,
    // to find which step holds the single-threaded loop long enough to starve
    // LoRa receive. POST zeroes them so a caller can measure a clean window.
    // Compiled in only for -DURTN_LOOP_DIAG instrumented builds.
    static void handle_diag_loop(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["loop"]      = Common::LoopTiming::max_loop_us;
      doc["reticulum"] = Common::LoopTiming::max_reticulum_us;
      doc["tcp"]       = Common::LoopTiming::max_tcp_us;
      doc["wifi"]      = Common::LoopTiming::max_wifi_us;
      doc["lxmf"]      = Common::LoopTiming::max_lxmf_us;
      doc["webui"]     = Common::LoopTiming::max_webui_us;
      doc["txq"]       = Common::LoopTiming::max_txq_us;
      doc["modem"]     = Common::LoopTiming::max_modem_us;
      doc["prune"]     = Common::LoopTiming::max_prune_us;
      doc["serial"]    = Common::LoopTiming::max_serial_us;
      // Breakdown of the reticulum section into its housekeeping steps (in
      // milliseconds; the section maxima above are microseconds). Pins which
      // step holds the loop when a table-scaling stall shows up: jobs() runs
      // the periodic persist/clean passes, interfaces is the per-interface
      // loop + announce-egress/held-announce drains, fs is flash persistence,
      // and txloop is Transport::loop().
      JsonObject rs = doc["reticulum_sections_ms"].to<JsonObject>();
      rs["jobs"]       = RNS::Reticulum::loop_jobs_ms();
      rs["interfaces"] = RNS::Reticulum::loop_interfaces_ms();
      rs["fs"]         = RNS::Reticulum::loop_fs_ms();
      rs["txloop"]     = RNS::Reticulum::loop_txloop_ms();
      // Largest backbone TCP backlog seen at a service() entry since reset —
      // confirms bursts are landing (the freeze condition) and that the per-call
      // drain budget is keeping the loop bounded despite them.
      doc["tcp_max_burst_bytes"] = TCPClientInterface::max_burst_bytes;
      send_json(req, 200, doc);
    }

    static void handle_diag_loop_reset(AsyncWebServerRequest* req, JsonVariant& /*body*/) {
      if (require_auth(req).empty()) return;
      Common::LoopTiming::reset();
      RNS::Reticulum::reset_loop_timing();
      TCPClientInterface::max_burst_bytes = 0;
      Common::PsramJsonDocument doc;
      doc["status"] = "reset";
      send_json(req, 200, doc);
    }
#endif  // URTN_LOOP_DIAG

    // POST /api/diag/persist — force a known-destinations persist now and report
    // how long the write took (ms). Diagnostics only: measures the full-blob
    // persist cost on demand (the #95 main-loop stall) without waiting for the
    // hourly job. After the #95 store migration this drops to ~0 (no full blob).
    static void handle_diag_persist(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      if (require_auth(req).empty()) return;
      RNS::Identity::mark_known_destinations_dirty();
      const uint32_t t0 = millis();
      const bool ok = RNS::Identity::save_known_destinations();
      const uint32_t ms = millis() - t0;
      Common::PsramJsonDocument doc;
      doc["persist_ms"]         = ms;
      doc["ok"]                 = ok;
      doc["known_destinations"] = (uint32_t)RNS::Identity::known_destinations_count();
      send_json(req, 200, doc);
    }

    // GET /api/diag/transport — forwarding counters for a transit (bridge) node.
    // linkreqs_* count link requests received / relayed onward / terminated
    // here; link_transit_* count link/resource packets relayed between
    // interfaces (in, forwarded, forwarded-onto-LoRa, dropped on hop mismatch).
    static void handle_diag_transport(AsyncWebServerRequest* req) {
      if (require_auth(req).empty()) return;
      Common::PsramJsonDocument doc;
      doc["linkreqs_rx"]           = RNS::Transport::linkreqs_rx();
      doc["linkreqs_fwd"]          = RNS::Transport::linkreqs_fwd();
      doc["linkreqs_local"]        = RNS::Transport::linkreqs_local();
      doc["link_transit_in"]       = RNS::Transport::link_transit_in();
      doc["link_transit_fwd"]      = RNS::Transport::link_transit_fwd();
      doc["link_transit_fwd_lora"] = RNS::Transport::link_transit_fwd_lora();
      doc["link_transit_drop"]     = RNS::Transport::link_transit_drop();
      doc["packets_received"]      = RNS::Transport::packets_received();
      // Path-store (microStore) write/compaction counters since boot. Sample
      // the delta over a window to get puts/sec + compactions/window — the
      // write-rate reference for the known-destinations store migration (#95).
      {
        auto ps = RNS::Transport::path_store_stats();
        JsonObject pstat = doc["path_store"].to<JsonObject>();
        pstat["puts"]               = ps.puts;
        pstat["removes"]            = ps.removes;
        pstat["compacts"]           = ps.compacts;
        pstat["bytes_written"]      = (uint32_t)ps.bytes_written;
        pstat["live_recs"]          = ps.live_recs;
        pstat["dead_since_compact"] = ps.dead_since_compact;
      }
#if defined(URTN_LOOP_DIAG)
      // Live row counts of the in-memory routing tables. These are the O(n)
      // inputs to the per-loop housekeeping scans, so a long reticulum loop
      // section is read against whichever of these has grown large. Only in
      // -DURTN_LOOP_DIAG instrumented builds.
      JsonObject tbl = doc["tables"].to<JsonObject>();
      tbl["path"]                    = (uint32_t)RNS::Transport::path_table_size();
      tbl["announce"]                = (uint32_t)RNS::Transport::announce_table_size();
      tbl["reverse"]                 = (uint32_t)RNS::Transport::reverse_table_size();
      tbl["link"]                    = (uint32_t)RNS::Transport::link_table_size();
      tbl["held_announces"]          = (uint32_t)RNS::Transport::held_announces_size();
      tbl["announce_rate"]           = (uint32_t)RNS::Transport::announce_rate_table_size();
      tbl["path_requests"]           = (uint32_t)RNS::Transport::path_requests_size();
      tbl["discovery_path_requests"] = (uint32_t)RNS::Transport::discovery_path_requests_size();
      tbl["tunnels"]                 = (uint32_t)RNS::Transport::tunnels_size();
      tbl["path_states"]             = (uint32_t)RNS::Transport::path_states_size();
      // Optional per-destination probe (?dest=<hex>): confirm a destination's
      // path stays at its expected hop count and isn't (wrongly) marked
      // unresponsive by the path_states producer.
      if (req->hasParam("dest")) {
        RNS::Bytes dh; dh.assignHex(req->getParam("dest")->value().c_str());
        JsonObject q = doc["dest"].to<JsonObject>();
        q["hash"]         = req->getParam("dest")->value();
        q["has_path"]     = RNS::Transport::has_path(dh);
        q["hops"]         = (uint32_t)RNS::Transport::hops_to(dh);
        q["unresponsive"] = RNS::Transport::path_is_unresponsive(dh);
      }
      // LXMF DIRECT link reuse (#90). reuses rising while opens stays flat after
      // the first send to a peer proves the cache is reusing warm links.
      JsonObject lr = doc["link_reuse"].to<JsonObject>();
      lr["reuses"] = LXMF::LXMFMinimal::link_reuses;
      lr["opens"]  = LXMF::LXMFMinimal::link_opens;
      lr["cached"] = (uint32_t)LXMF::LXMFMinimal::direct_links_size();
#endif  // URTN_LOOP_DIAG
      send_json(req, 200, doc);
    }
