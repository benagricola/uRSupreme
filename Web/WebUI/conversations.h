// Auto-extracted from Web/WebUI.h on the SPA-migration branch.
// Included from inside the class body of Web::WebUI in WebUI.h —
// this file has NO include guard, NO `#pragma once`, and is not a
// standalone translation unit. The static method definitions below
// remain implicit-inline because they sit inside a class body via
// the surrounding #include directive.

    static void handle_clear_conversation(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      std::string peer_hex = std::string(req->pathArg(1).c_str());
      RNS::Bytes peer = hex_to_bytes(peer_hex, LXMF::HASH_LEN);
      if (peer.size() != LXMF::HASH_LEN) {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "Peer address must be 32 hex characters (got %u).",
                 (unsigned)peer_hex.size());
        send_error_with_message(req, 400, "invalid_peer_hash", msg);
        return;
      }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a) { send_error(req, 404, "unknown_identity"); return; }
      size_t inbox_removed  = a->inbox  ? a->inbox->purge_peer(peer)  : 0;
      size_t outbox_removed = a->outbox ? a->outbox->purge_peer(peer) : 0;
      NOTICEF("WebUI: cleared conversation %s <-> %s (inbox=%u outbox=%u)",
              requested.c_str(), peer_hex.c_str(),
              (unsigned)inbox_removed, (unsigned)outbox_removed);
      Common::PsramJsonDocument doc;
      doc["inbox_removed"]  = (uint32_t)inbox_removed;
      doc["outbox_removed"] = (uint32_t)outbox_removed;
      send_json(req, 200, doc);
    }

    // GET /api/identities/{id}/conversations/{peer_hex}/config
    //
    // Returns the per-conversation TTL override (if any) plus the
    // Per-chat retention surface. Returns the per-peer override plus
    // the identity-wide default so the SPA can label "Use default
    // (X days)" / "Use default (N messages)" without a second
    // round-trip.
    static void handle_conversation_config_get(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      std::string peer_hex = std::string(req->pathArg(1).c_str());
      RNS::Bytes peer = hex_to_bytes(peer_hex, LXMF::HASH_LEN);
      if (peer.size() != LXMF::HASH_LEN) {
        send_error_with_message(req, 400, "invalid_peer_hash",
                                "Peer address must be 32 hex characters.");
        return;
      }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a || !a->inbox) { send_error(req, 404, "unknown_identity"); return; }
      const auto& overrides = a->inbox->peer_retention_overrides();
      auto it = overrides.find(peer_hex);
      const bool has_override = (it != overrides.end());
      const LXMF::Retention default_r = LXMF::InboxConfig::current().default_retention;
      const LXMF::Retention effective = has_override ? it->second : default_r;
      Common::PsramJsonDocument doc;
      if (has_override) {
        JsonObject o = doc["retention"].to<JsonObject>();
        o["kind"]  = LXMF::retention_kind_name(it->second.kind);
        o["value"] = it->second.value;
      } else {
        doc["retention"] = nullptr;
      }
      {
        JsonObject o = doc["effective_retention"].to<JsonObject>();
        o["kind"]  = LXMF::retention_kind_name(effective.kind);
        o["value"] = effective.value;
      }
      {
        JsonObject o = doc["default_retention"].to<JsonObject>();
        o["kind"]  = LXMF::retention_kind_name(default_r.kind);
        o["value"] = default_r.value;
      }
      send_json(req, 200, doc);
    }

    // POST /api/identities/{id}/conversations/{peer_hex}/config
    //
    // Body: { "retention": { "kind", "value" } | null }
    //   - null          → clear the override (inherit identity default)
    //   - kind:"none"   → keep forever for this peer
    //   - kind:"time"   → expire records older than value seconds
    //   - kind:"count"  → keep newest value messages from this peer
    static void handle_conversation_config_post(AsyncWebServerRequest* req,
                                                 JsonVariant& body) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      std::string peer_hex = std::string(req->pathArg(1).c_str());
      RNS::Bytes peer = hex_to_bytes(peer_hex, LXMF::HASH_LEN);
      if (peer.size() != LXMF::HASH_LEN) {
        send_error_with_message(req, 400, "invalid_peer_hash",
                                "Peer address must be 32 hex characters.");
        return;
      }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a || !a->inbox || !a->outbox) { send_error(req, 404, "unknown_identity"); return; }
      JsonVariant v = body["retention"];
      if (v.isNull()) {
        a->inbox->clear_peer_retention(peer_hex);
        a->outbox->clear_peer_retention(peer_hex);
      } else if (v.is<JsonObject>()) {
        LXMF::Retention r;
        r.kind  = LXMF::retention_kind_from_str(v["kind"] | "none");
        r.value = (uint32_t)(v["value"] | 0);
        if (r.kind == LXMF::Retention::Kind::Time
            && r.value > 10UL * 365UL * 86400UL) {
          send_error_with_message(req, 400, "ttl_too_large",
            "Retention time must be no more than 10 years.");
          return;
        }
        if (r.kind == LXMF::Retention::Kind::Count
            && r.value > LXMF::LXMFInbox::DEFAULT_RAM_CAPACITY) {
          send_error_with_message(req, 400, "count_too_large",
            "Per-chat message count cannot exceed the per-identity ring capacity.");
          return;
        }
        a->inbox->set_peer_retention(peer_hex, r);
        a->outbox->set_peer_retention(peer_hex, r);
      } else {
        send_error_with_message(req, 400, "invalid_retention",
                                "retention must be null or { kind, value }.");
        return;
      }
      LXMF::LXMFGateway::persist_conversation_config(*a, a->inbox->peer_retention_overrides());
      handle_conversation_config_get(req);
    }

    // GET /api/identities/{id}/attachments/{filename}
    // Bearer-auth gated. Streams the requested attachment blob from
    // <identity_dir>/attachments/<filename>. Filenames are exactly the
    // ones the inbox JSONL stores (see LXMFGateway attachment-persist
    // callback: "<msg_hash_hex>_<tag>_<idx>.bin"). The filename is
    // strictly validated against [0-9a-f_].bin to forbid traversal.
    // Per-request staging-buffer ID. The chunk-callback sets it during
    // UPLOAD_FILE_START; the final-handler reports it back to the
    // client. Reset to 0 at each request via UPLOAD_FILE_START so a
    // failed request doesn't leak its id into the next one.
    static uint32_t& _current_upload_staging_id() { static uint32_t v = 0; return v; }
    // Sticky error string for the final-handler response when the
    // chunk path bailed.
    static const char*& _current_upload_error() { static const char* v = nullptr; return v; }

    // Per-chunk multipart-upload handler. AsyncWebServer invokes this
    // repeatedly during the upload: index==0 marks the first chunk
    // (allocate the buffer), final==true marks the last (validate +
    // commit), in-between calls append bytes. The actual HTTP
    // response is produced by handle_outbound_upload_final below.
    static void handle_outbound_upload_chunk(AsyncWebServerRequest* req,
                                             const String& /*filename*/,
                                             size_t index, uint8_t* data,
                                             size_t len, bool final) {
      auto& staging_id = _current_upload_staging_id();
      auto& err        = _current_upload_error();
      if (index == 0) {
        staging_id = 0;
        err        = nullptr;
        // Total size is the X-Total-Length header — query args are
        // not reliable during multipart parsing. strtoull lets us
        // reject >4 GiB values before narrowing to size_t.
        if (!req->hasHeader("X-Total-Length")) {
          err = "Missing X-Total-Length header.";
          return;
        }
        const String hdr_total = req->header("X-Total-Length");
        char* end = nullptr;
        const unsigned long long total64 = strtoull(hdr_total.c_str(), &end, 10);
        if (hdr_total.length() == 0 || end == hdr_total.c_str() || total64 == 0) {
          err = "Invalid X-Total-Length header.";
          return;
        }
        const size_t eff_max = Storage::Config::effective_max_send();
        if (total64 > (unsigned long long)eff_max) {
          err = "Requested upload size exceeds the configured send cap.";
          return;
        }
        const size_t total = (size_t)total64;
        // Cross-check against Content-Length: must be ≥ total
        // (multipart wrapping adds bytes), and within 16 KiB of it.
        if (req->hasHeader("Content-Length")) {
          const unsigned long long clen =
            strtoull(req->header("Content-Length").c_str(), nullptr, 10);
          if (clen > 0 && clen < total64) {
            err = "Content-Length is smaller than declared `total`.";
            return;
          }
          if (clen > total64 + 16 * 1024) {
            err = "Content-Length is far larger than declared `total`.";
            return;
          }
        }
        const uint32_t id = Storage::OutboundStaging::allocate(total);
        if (id == 0) {
          err = "Allocation rejected — file too large or PSRAM/SD unavailable.";
          return;
        }
        staging_id = id;
      }
      if (staging_id == 0) return;  // error already set
      if (len > 0 && !Storage::OutboundStaging::append(staging_id, data, len)) {
        // append() refuses any write that would push past the
        // allocated size; treat as a hard fault.
        err = "Chunk write failed (overrun or backing-store error).";
        Storage::OutboundStaging::release(staging_id);
        staging_id = 0;
        return;
      }
      if (final) {
        if (!Storage::OutboundStaging::complete(staging_id)) {
          err = "Upload ended before all bytes were received.";
          Storage::OutboundStaging::release(staging_id);
          staging_id = 0;
        }
      }
    }

    // Final handler — runs once after the upload completes (or fails).
    // Reports the staging_id the client should hand to /send.
    static void handle_outbound_upload_final(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) {
        // Auth fail. Drop any staging buffer the chunk path may have
        // built up — we shouldn't keep bytes for an unauthorized peer.
        uint32_t id = _current_upload_staging_id();
        if (id) Storage::OutboundStaging::release(id);
        _current_upload_staging_id() = 0;
        _current_upload_error()      = nullptr;
        return;
      }
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) {
        uint32_t id = _current_upload_staging_id();
        if (id) Storage::OutboundStaging::release(id);
        _current_upload_staging_id() = 0;
        _current_upload_error()      = nullptr;
        send_error(req, 403, "forbidden");
        return;
      }
      const char* err = _current_upload_error();
      const uint32_t id = _current_upload_staging_id();
      _current_upload_staging_id() = 0;
      _current_upload_error()      = nullptr;
      if (err) {
        send_error_with_message(req, 400, "upload_failed", err);
        return;
      }
      if (id == 0) {
        send_error_with_message(req, 400, "upload_failed",
          "Upload completed but no staging buffer was created.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["staging_id"] = id;
      doc["total_bytes"] = (uint32_t)Storage::OutboundStaging::total_bytes(id);
      doc["backend"]     = Storage::OutboundStaging::backend_name(
                              Storage::OutboundStaging::backend_of(id));
      send_json(req, 200, doc);
    }

    static void handle_attachment_get(AsyncWebServerRequest* req) {
      // Auth + identity lookup needs the RNS lock because require_auth
      // walks the token table that LXMFGateway also mutates. EVERYTHING
      // ELSE in this handler — file open, size(), the streaming
      // producer — runs OUTSIDE the lock so a concurrent inbound
      // Resource transfer (which holds the lock heavily for its own
      // packet/link/resource state) can't block attachment downloads.
      //
      // Before this scoping: holding RnsLockGuard for the whole
      // setup caused the SX webserver to wedge entirely when a
      // background Resource transfer was inbound — the radio task
      // owned the lock continuously, the HTTP handler queued behind
      // it on `SD.open() + sd_f.size()`, and AsyncTCP backed up to
      // the point of being unrecoverable without a reboot.
      std::string requested;
      std::string fname;
      std::string identity_dir;
      {
        RnsLockGuard _g;
        LXMF::IdentityId caller = require_auth(req);
        if (caller.empty()) return;
        requested = std::string(req->pathArg(0).c_str());
        if (caller != requested) { send_error(req, 403, "forbidden"); return; }
        fname = std::string(req->pathArg(1).c_str());
        const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
        if (!a) { send_error(req, 404, "unknown_identity"); return; }
        identity_dir = a->dir();
      }
      // Out of the lock from here on. Validation + I/O on the locals
      // captured above — no further state lookups against the RNS
      // gateway are needed.
      // [ATTDBG] Log the raw filename (full hex dump) so we can pin-point
      // any URL-decode quirks or stray whitespace.
      {
        std::string hex;
        char buf[8];
        for (size_t i = 0; i < fname.size(); ++i) {
          snprintf(buf, sizeof(buf), "%02x ", (unsigned char)fname[i]);
          hex += buf;
        }
        NOTICEF("[ATTDBG] handle_attachment_get: fname.size()=%u hex=[%s] full=\"%s\"",
                (unsigned)fname.size(), hex.c_str(), fname.c_str());
      }
      // Allow [0-9a-fA-F_.] only and require the .bin suffix; rejects
      // any "..", "/", "\", or unexpected character outright. The
      // generated names use only lowercase hex + underscore + ".bin"
      // (see LXMFGateway attachment-persist callback).
      bool ok = !fname.empty() && fname.size() < 96
                && fname.size() > 4
                && fname.compare(fname.size() - 4, 4, ".bin") == 0;
      if (!ok) {
        NOTICEF("[ATTDBG] reject reason: empty=%d size_lt_96=%d size_gt_4=%d ends_bin=%d",
                (int)fname.empty(), (int)(fname.size() < 96),
                (int)(fname.size() > 4),
                (int)(fname.size() >= 4 &&
                      fname.compare(fname.size() - 4, 4, ".bin") == 0));
      }
      // Validate the stem (everything before the literal ".bin"
      // suffix). The suffix itself is fixed; only the stem needs to
      // be locked down to hex/underscore/dot (e.g. "msg_06_0").
      const size_t stem_len = ok ? fname.size() - 4 : 0;
      for (size_t i = 0; ok && i < stem_len; ++i) {
        char c = fname[i];
        bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                     || c == '_' || c == '.';
        if (!valid) {
          NOTICEF("[ATTDBG] invalid char at %u: 0x%02x", (unsigned)i, (unsigned char)c);
          ok = false;
        }
      }
      if (!ok) {
        send_error_with_message(req, 400, "invalid_attachment_name",
          "Attachment filename must match <msg_hex>_<tag>_<idx>.bin.");
        return;
      }
      const std::string full = identity_dir + "/attachments/" + fname;
      // Backend dispatch. Try SD first if a card is mounted —
      // big attachments live there; small/pre-SD ones on LittleFS. If
      // neither has the file, return 404.
      const bool on_sd    = Storage::SDCard::present() && Storage::SDCard::exists(full.c_str());
      const bool on_flash = !on_sd && filesystem.exists(full.c_str());
      if (!on_sd && !on_flash) {
        send_error(req, 404, "attachment_not_found");
        return;
      }
      NOTICEF("[ATTDBG] handle_attachment_get: path=%s backend=%s free_heap=%u psram=%u",
              full.c_str(), on_sd ? "sd" : "flash",
              (unsigned)esp_get_free_heap_size(),
              (unsigned)ESP.getFreePsram());
      // Stream the file in 32 KiB chunks via AsyncTCP's producer lambda.
      // Two earlier failure modes to avoid:
      //
      //   - Per-call SD reads. AsyncTCP invokes the lambda with maxLen
      //     ≈ TCP MSS (~1460 B). A 1 MiB attachment naively means ~700
      //     SD reads. Reading a bigger chunk into a scratch buffer and
      //     draining it across many lambda calls amortises the SD-read
      //     cost ~22x.
      //   - Use-after-free on lambda re-entry. The previous code did
      //     `delete st` when read returned 0, then returned 0. AsyncTCP
      //     can (and sometimes does) call the lambda again after a 0
      //     return — at which point st is freed and any access is UB.
      //     This is the root of the "request never returns + webserver
      //     wedges" crash that survived earlier attempts at fixing the
      //     lock scope and the FATFS mutex contention.
      //
      // Fix: own StreamState by std::shared_ptr captured by VALUE into
      // the lambda. The lambda holds a refcount for as long as
      // AsyncWebServer retains it; when the response is destroyed (for
      // any reason — completion, client disconnect, internal abort)
      // the lambda is destroyed and the last refcount drops. The
      // StreamState destructor closes the file handle and frees the
      // scratch. Idempotent and crash-safe regardless of how many
      // times the lambda is invoked.
      const size_t SCRATCH_SIZE = 32 * 1024;
      struct StreamState {
        bool             on_sd          = false;
        File             sd_f;
        microStore::File flash_f;
        uint8_t*         scratch        = nullptr;
        size_t           scratch_size   = 0;
        size_t           scratch_valid  = 0;
        size_t           scratch_offset = 0;
        bool             eof            = false;
        ~StreamState() {
          if (on_sd && sd_f)  sd_f.close();
          else if (flash_f)   flash_f.close();
          if (scratch)        heap_caps_free(scratch);
        }
      };
      auto st = std::make_shared<StreamState>();
      st->on_sd        = on_sd;
      st->scratch_size = SCRATCH_SIZE;
      if (on_sd) {
        st->sd_f = Storage::SDCard::open_read(full.c_str());
        if (!st->sd_f) {
          NOTICEF("[ATTDBG] SD open_read failed for %s", full.c_str());
          send_error(req, 500, "attachment_open_failed");
          return;
        }
      } else {
        st->flash_f = filesystem.open(full.c_str(), microStore::File::ModeRead);
        if (!st->flash_f) {
          NOTICEF("[ATTDBG] flash open failed for %s", full.c_str());
          send_error(req, 500, "attachment_open_failed");
          return;
        }
      }
      const size_t total = on_sd ? (size_t)st->sd_f.size() : (size_t)st->flash_f.size();
      NOTICEF("[ATTDBG] opened ok, total=%u bytes", (unsigned)total);
      st->scratch = (uint8_t*)heap_caps_malloc(SCRATCH_SIZE,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!st->scratch) st->scratch = (uint8_t*)heap_caps_malloc(SCRATCH_SIZE, MALLOC_CAP_8BIT);
      if (!st->scratch) {
        // StreamState's destructor will close the file handle for us
        // when `st` goes out of scope at the end of this function.
        send_error_with_message(req, 503, "out_of_memory",
          "Not enough memory to start the attachment download. Try again in a moment.");
        return;
      }
      AsyncWebServerResponse* resp = req->beginResponse(
          "application/octet-stream", total,
          [st](uint8_t* dst, size_t maxLen, size_t /*index*/) -> size_t {
            // Refill scratch on demand; one SD/FS hit per 32 KiB
            // instead of per TCP segment.
            if (!st->eof && st->scratch_offset >= st->scratch_valid) {
              const size_t got = st->on_sd
                  ? (size_t)st->sd_f.read(st->scratch, st->scratch_size)
                  : (size_t)st->flash_f.read(st->scratch, st->scratch_size);
              st->scratch_offset = 0;
              st->scratch_valid  = got;
              if (got == 0) st->eof = true;
            }
            if (st->eof && st->scratch_offset >= st->scratch_valid) {
              return 0;   // shared_ptr cleanup happens when AsyncWebServer
                          // destroys the lambda; no explicit delete here.
            }
            const size_t avail = st->scratch_valid - st->scratch_offset;
            const size_t n     = avail < maxLen ? avail : maxLen;
            memcpy(dst, st->scratch + st->scratch_offset, n);
            st->scratch_offset += n;
            return n;
          });
      resp->addHeader("Content-Disposition",
                      String("attachment; filename=\"") + fname.c_str() + "\"");
      req->send(resp);
    }

    static void emit_messages_array(JsonArray arr,
                                    const std::deque<LXMF::MessageRecord>& ring,
                                    const MessageFilter& include) {
      for (const auto& m : ring) {
        if (!include(m)) continue;
        JsonObject obj = arr.add<JsonObject>();
        obj["seq"]         = m.seq;
        obj["ts"]          = m.ts;
        obj["boot_epoch"]  = m.boot_epoch;
        obj["received_ms"] = m.received_ms;
        obj["peer"]        = m.peer_hash.toHex();
        obj["title"]       = m.title;
        // Body delivery: always inline. Body sizes are capped at
        // LXMF_MAX_BODY_BYTES (4 KiB) on the send path so this is
        // bounded RAM. The earlier body_on_disk spill + lazy /body
        // endpoint was dead infrastructure (the SPA never called the
        // endpoint) and added complexity to every code path that
        // touched MessageRecord.
        obj["body"]        = m.content;
        obj["body_size"]   = m.body_size;
        obj["sig_ok"]      = m.signature_ok;
        obj["status"]      = LXMF::outbox_status_name(m.status);
        // Attachments (LXMF fields-dict file/image/audio blobs). The
        // bytes themselves live on disk under
        // /lxmf/identities/<id>/attachments/<filename>; this just
        // surfaces the metadata so the SPA can show "📎 (filename, size)"
        // chips on the message bubble. A future GET endpoint will let
        // the SPA download the bytes for thumbnail / play.
        if (!m.attachments.empty()) {
          JsonArray atts = obj["attachments"].to<JsonArray>();
          for (const auto& a : m.attachments) {
            JsonObject o = atts.add<JsonObject>();
            o["tag"]      = a.tag;
            o["size"]     = a.size;
            o["filename"] = a.filename;
            if (!a.display_name.empty()) o["display_name"] = a.display_name;
            if (!a.mime.empty()) o["mime"] = a.mime;
            if (!a.backend.empty()) o["backend"] = a.backend;
          }
        }
      }
    }

    static void handle_inbox(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a || !a->inbox) { send_error(req, 404, "unknown_identity"); return; }
      uint32_t since = (uint32_t)req->arg("since").toInt();
      size_t   limit = (size_t)req->arg("limit").toInt();
      if (limit == 0 || limit > 50) limit = 50;
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["messages"].to<JsonArray>();
      // Iterate the deque oldest→newest. For "since" pagination emit
      // every record whose seq exceeds the cursor; for the default
      // "most recent N" view emit the trailing slice. No vector copy.
      const auto& ring = a->inbox->ring();
      if (since > 0) {
        emit_messages_array(arr, ring,
          [since](const LXMF::MessageRecord& m){ return m.seq > since; });
      } else {
        const size_t skip = (ring.size() > limit) ? (ring.size() - limit) : 0;
        size_t idx = 0;
        emit_messages_array(arr, ring,
          [skip, &idx](const LXMF::MessageRecord&){ return idx++ >= skip; });
      }
      doc["next_since"] = a->inbox->next_seq();
      send_json(req, 200, doc);
    }

    static void handle_outbox(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a || !a->outbox) { send_error(req, 404, "unknown_identity"); return; }
      uint32_t since = (uint32_t)req->arg("since").toInt();
      size_t   limit = (size_t)req->arg("limit").toInt();
      if (limit == 0 || limit > 50) limit = 50;
      Common::PsramJsonDocument doc;
      JsonArray arr = doc["messages"].to<JsonArray>();
      const auto& ring = a->outbox->ring();
      if (since > 0) {
        emit_messages_array(arr, ring,
          [since](const LXMF::MessageRecord& m){ return m.seq > since; });
      } else {
        const size_t skip = (ring.size() > limit) ? (ring.size() - limit) : 0;
        size_t idx = 0;
        emit_messages_array(arr, ring,
          [skip, &idx](const LXMF::MessageRecord&){ return idx++ >= skip; });
      }
      doc["next_since"] = a->outbox->next_seq();
      send_json(req, 200, doc);
    }

    // POST /api/identities/{id}/outbox/{seq}/retry — manually re-queue
    // a Failed outbox entry whose auto-retry budget was exhausted. The
    // outbox seq -> MessageRecord -> packet_hash (== PendingLinkSend
    // record_hash) lookup gives us the entry; LXMFMinimal::manual_retry
    // resets the budget and schedules an immediate retry.
    static void handle_outbox_retry(AsyncWebServerRequest* req) {
      RnsLockGuard _g;
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      const LXMF::LXMFIdentity* a = LXMF::LXMFGateway::identity_by_id(requested);
      if (!a || !a->outbox) { send_error(req, 404, "unknown_identity"); return; }
      const uint32_t seq = (uint32_t)atoi(req->pathArg(1).c_str());
      // Direct seq lookup into the deque — no copy, no window guess.
      const LXMF::MessageRecord* rec = a->outbox->find_by_seq(seq);
      if (!rec) {
        send_error_with_message(req, 404, "outbox_seq_not_found",
          "No outbox entry with that sequence number was found in the recent window.");
        return;
      }
      if (rec->status != LXMF::OutboxStatus::Failed) {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "Outbox entry %lu is %s, not Failed — only Failed entries can be retried.",
                 (unsigned long)seq, LXMF::outbox_status_name(rec->status));
        send_error_with_message(req, 409, "outbox_not_failed", msg);
        return;
      }
      if (rec->packet_hash.size() != LXMF::HASH_LEN) {
        send_error_with_message(req, 409, "outbox_no_packet_hash",
          "Outbox entry has no link-hash; cannot manual-retry.");
        return;
      }
      if (!LXMF::LXMFMinimal::manual_retry(rec->packet_hash)) {
        // Wire bytes were dropped (server reboot or stale_failed prune)
        // — the user has to re-send the message manually.
        send_error_with_message(req, 410, "outbox_state_gone",
          "The original send-state is no longer available (likely after a reboot). Send the message again.");
        return;
      }
      Common::PsramJsonDocument doc;
      doc["status"]      = "retry_scheduled";
      doc["queued_seq"]  = seq;
      send_json(req, 202, doc);
    }

    static void handle_send(AsyncWebServerRequest* req, JsonVariant& body) {
      RnsLockGuard _g;
      // Big-body diagnostic — log heap state on entry / exit of large
      // POSTs so we can see whether the wedge happens during the HTTP
      // ingest (this handler), during outbox write (later in this
      // function), or only later in the LXMF gateway loop's encrypt.
      const size_t content_hint = body.containsKey("content")
        ? strlen(body["content"] | "")
        : strlen(body["body"] | "");
      const bool diag_send = content_hint > 1024;
      if (diag_send) {
        NOTICEF("handle_send[ENTER] content_len=%u dma_free=%u dma_largest=%u sram_free=%u",
               (unsigned)content_hint,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      }
      LXMF::IdentityId caller = require_auth(req);
      if (caller.empty()) return;
      std::string requested = std::string(req->pathArg(0).c_str());
      if (caller != requested) { send_error(req, 403, "forbidden"); return; }
      std::string to_hex  = (const char*)(body["to"]      | "");
      std::string title   = (const char*)(body["title"]   | "");
      // The SPA sends `content`; legacy / external clients may still send
      // `body`. Accept either, preferring `content`.
      std::string content = (const char*)(body["content"] | (body["body"] | ""));
      // Cap body length to LXMF_MAX_BODY_BYTES (4 KiB). Matches the
      // WhatsApp/Telegram convention; well above any reasonable typed
      // text but below anything that'd stress the LoRa airtime budget
      // or the on-device ring. Reject loudly so the SPA can show the
      // user a useful "message too long" warning rather than silently
      // truncating.
      if (content.size() > LXMF::LXMF_MAX_BODY_BYTES) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Message body too long: %u characters, cap is %u.",
                 (unsigned)content.size(),
                 (unsigned)LXMF::LXMF_MAX_BODY_BYTES);
        send_error_with_message(req, 413, "body_too_long", msg);
        return;
      }
      if (title.size() > LXMF::LXMF_MAX_TITLE_BYTES) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Title too long: %u characters, cap is %u.",
                 (unsigned)title.size(),
                 (unsigned)LXMF::LXMF_MAX_TITLE_BYTES);
        send_error_with_message(req, 413, "title_too_long", msg);
        return;
      }
      if (body["attachments"].is<JsonArrayConst>()
          && body["attachments"].as<JsonArrayConst>().size() > LXMF::LXMF_MAX_ATTACHMENTS) {
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "Too many attachments: %u, cap is %u per message.",
                 (unsigned)body["attachments"].as<JsonArrayConst>().size(),
                 (unsigned)LXMF::LXMF_MAX_ATTACHMENTS);
        send_error_with_message(req, 413, "too_many_attachments", msg);
        return;
      }
      RNS::Bytes to = hex_to_bytes(to_hex, LXMF::HASH_LEN);
      if (to.size() != LXMF::HASH_LEN) {
        char msg[120];
        snprintf(msg, sizeof(msg),
                 "Destination address must be 32 hex characters (got %u).",
                 (unsigned)to_hex.size());
        send_error_with_message(req, 400, "invalid_destination_hash", msg);
        return;
      }

      // Attachments are referenced by `staging_id` only — bytes were
      // uploaded ahead of time via /attachment/upload. This keeps
      // the JSON body tiny regardless of attachment size, and lets the
      // backing buffer live in PSRAM (or SD when present) rather than
      // RAM-doubling through base64.
      //
      // Each entry: { tag, staging_id, filename?, mime?, ext?, audio_mode? }.
      // send_message() takes ownership of the staging buffers and
      // releases them on success or failure.
      std::vector<LXMF::LXMFMinimal::OutgoingAttachment> attachments;
      if (body["attachments"].is<JsonArrayConst>()) {
        for (JsonObjectConst a : body["attachments"].as<JsonArrayConst>()) {
          int tag = a["tag"] | 0;
          if (tag != LXMF::FIELD_FILE_ATTACHMENTS
              && tag != LXMF::FIELD_IMAGE
              && tag != LXMF::FIELD_AUDIO) {
            send_error_with_message(req, 400, "invalid_attachment_tag",
              "Attachment tag must be 5 (file), 6 (image), or 7 (audio).");
            return;
          }
          uint32_t sid = (uint32_t)(a["staging_id"] | 0);
          if (sid == 0) {
            send_error_with_message(req, 400, "missing_staging_id",
              "Attachment is missing staging_id — upload bytes via /attachment/upload first.");
            return;
          }
          if (!Storage::OutboundStaging::complete(sid)) {
            send_error_with_message(req, 409, "staging_incomplete",
              "Attachment staging buffer hasn't finished uploading.");
            return;
          }
          LXMF::LXMFMinimal::OutgoingAttachment oa;
          oa.tag                  = (uint8_t)tag;
          oa.staging_id           = sid;
          oa.staging_total_bytes  = Storage::OutboundStaging::total_bytes(sid);
          oa.filename             = a["filename"] | "";
          oa.mime                 = a["mime"]     | "";
          if (oa.filename.size() > LXMF::LXMF_MAX_ATTACHMENT_NAME) {
            send_error_with_message(req, 413, "attachment_filename_too_long",
              "Attachment filename exceeds the configured limit.");
            return;
          }
          if (oa.mime.size() > LXMF::LXMF_MAX_ATTACHMENT_MIME) {
            send_error_with_message(req, 413, "attachment_mime_too_long",
              "Attachment mime type exceeds the configured limit.");
            return;
          }
          // Per Sideband convention, FIELD_IMAGE carries an `ext` string
          // (e.g. "webp"). Derive from mime "image/xyz" if the SPA didn't
          // send an explicit ext, else fall back to the filename suffix
          // or a safe default.
          if (oa.tag == LXMF::FIELD_IMAGE) {
            const char* explicit_ext = a["ext"] | "";
            if (*explicit_ext) {
              oa.ext = explicit_ext;
            } else if (oa.mime.rfind("image/", 0) == 0) {
              oa.ext = oa.mime.substr(6);
            } else {
              const size_t dot = oa.filename.find_last_of('.');
              oa.ext = (dot != std::string::npos) ? oa.filename.substr(dot + 1) : std::string("bin");
            }
          }
          if (oa.tag == LXMF::FIELD_AUDIO) {
            // SPA may pass an explicit audio_mode (AM_* int). Default to
            // AM_CUSTOM (0xFF) which lets the receiver sniff the payload.
            oa.audio_mode = (uint8_t)((int)(a["audio_mode"] | 0xFF) & 0xFF);
          }
          attachments.push_back(std::move(oa));
        }
      }

      if (content.empty() && attachments.empty()) {
        send_error_with_message(req, 400, "missing_content",
            "Message body is empty. Type something or attach a file before sending.");
        return;
      }
      LXMF::MessageRecord rec;
      const char* err = nullptr;
      if (!LXMF::LXMFGateway::send(requested, to, title, content,
                                   attachments.empty() ? nullptr : &attachments,
                                   rec, &err)) {
        send_error_with_message(req, 503, "send_failed",
                                err ? err : "Send failed for an unknown reason.");
        return;
      }
      // The 202 response returns the full server-authoritative shape
      // of the just-created outbox record so the SPA's optimistic
      // entry can be inserted with the same identifiers the firmware
      // uses. Without packet_hash here, subsequent outbox_status
      // events (queued → sent → delivered) can't match by link hash
      // and the status pill stays stuck on "queued". Without
      // server-authoritative ts/boot_epoch/received_ms, the optimistic
      // record sorts in the wrong place when the device clock differs
      // from the browser. Without the persisted attachments array,
      // the bubble's inline preview can't be rendered because the
      // SPA only knows the user-supplied filename, not the
      // hash-based on-disk filename.
      Common::PsramJsonDocument doc;
      doc["queued_seq"]  = rec.seq;
      doc["status"]      = LXMF::outbox_status_name(rec.status);
      doc["ts"]          = rec.ts;
      doc["boot_epoch"]  = rec.boot_epoch;
      doc["received_ms"] = rec.received_ms;
      if (rec.packet_hash.size() > 0) doc["packet_hash"] = rec.packet_hash.toHex();
      if (!rec.attachments.empty()) {
        JsonArray atts = doc["attachments"].to<JsonArray>();
        for (const auto& a : rec.attachments) {
          JsonObject o = atts.add<JsonObject>();
          o["tag"]          = a.tag;
          o["size"]         = a.size;
          o["filename"]     = a.filename;
          if (!a.display_name.empty()) o["display_name"] = a.display_name;
          if (!a.mime.empty())         o["mime"]         = a.mime;
          if (!a.backend.empty())      o["backend"]      = a.backend;
        }
      }
      send_json(req, 202, doc);
      if (diag_send) {
        NOTICEF("handle_send[EXIT] seq=%u status=%s dma_free=%u dma_largest=%u sram_free=%u",
               (unsigned)rec.seq, LXMF::outbox_status_name(rec.status),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      }
    }

