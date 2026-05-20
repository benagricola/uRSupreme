#pragma once

#include <ArduinoJson.h>
#include <Log.h>
#include <microStore/FileSystem.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <stdint.h>

#include "LXMFTypes.h"

extern microStore::FileSystem filesystem;

namespace LXMF {

  // Bounded per-identity ring with JSONL spool persistence.
  // One instance per LXMFIdentity. Used for both inbox (incoming=true) and
  // outbox (incoming=false) — the on-disk schema is identical, only the
  // file name differs.
  //
  // Persistence: each append() writes one line to <dir>/<filename>. On boot
  // the constructor reads any existing file back into the RAM ring (up to
  // ram_capacity entries, most recent kept). Sequence numbers continue from
  // the highest seen on disk so SSE clients can resume after a reboot.
  //
  // No rotation in Phase 1: at ~256 B/message the JSONL grows to ~64 KB
  // after ~250 messages, which is well within the LittleFS partition.
  // Rotation can be added later if needed.
  class LXMFInbox {
  public:
    // Per-identity per-mailbox cap. At ~300 B per JSONL line for a
    // typical chat-sized message, 200 entries = ~60 KB / identity /
    // mailbox — comfortable on the LittleFS partition.
    //
    // Configurable globally via /lxmf/inbox_config.json:
    //   - ram_capacity: 200 (default) / 500 / 1000 / 5000 / UNLIMITED
    //   - ttl_seconds:  0 (off)       / 86400×N for "last N days"
    // Both checks run on each append() and at load(). TTL pruning
    // uses the LXMF `ts` field (sender wall-clock); requires a
    // calibrated local clock — the prune is a no-op while uncalibrated.
    static constexpr size_t DEFAULT_RAM_CAPACITY = 200;
    static constexpr size_t UNLIMITED_CAPACITY   = SIZE_MAX;
    static constexpr size_t MAX_LINE_BYTES       = 4096;
    // Bodies whose wire size needed Link+Resource transport (i.e. >319
    // bytes encoded; the LXMF in-link PACKET limit) spill to a per-
    // message file rather than embedding inline in the JSONL line.
    // Same threshold drives the outbound streaming refactor. Below it,
    // bodies fit single-packet OPPORTUNISTIC / in-link DATA, stay
    // inline in the JSONL for cheap single-disk-read lookup.
    static constexpr size_t BODY_SPILL_THRESHOLD = 319;

    // Body-storage callbacks. The path is the absolute filesystem path
    // the inbox wants to use (e.g. "/lxmf/<id>/inbox/<seq>.body"); the
    // caller's writer/reader implementation handles SD-first then-flash
    // routing the same way the attachment layer does — same path string
    // is tried against SD via the SDCard helpers, falls back to flash
    // via microStore::filesystem.
    using BodyWriter = std::function<bool(const std::string& path, const std::string& content)>;
    using BodyReader = std::function<bool(const std::string& path, std::string& out_content)>;
    using BodyRemover = std::function<void(const std::string& path)>;
    // Returns the body file's size in bytes, or 0 if missing/unreadable.
    // Used by load_from_jsonl to recover `body_size` for legacy records
    // persisted before that field existed (no full body read required —
    // size is a stat() call). Called at most once per legacy record at
    // boot. Optional; if not set, legacy records return body_size=0
    // (SPA will skip body display for those — acceptable degradation).
    using BodySizeReader = std::function<uint32_t(const std::string& path)>;
    // Streams the body file contents into the supplied buffer starting
    // at `offset`, writing up to `max_len` bytes. Returns bytes written.
    // 0 means EOF (or error — caller distinguishes via expected size).
    // This is the hot path for the streaming /body endpoint: each TCP
    // chunk callback invokes the reader for the next slice. The reader
    // is responsible for keeping the file open across calls if it wants
    // to (e.g. via a thread-local FILE* cache); the default fopen-per-
    // chunk implementation is fine since LittleFS open is cheap.
    using BodyChunkReader = std::function<size_t(const std::string& path,
                                                 size_t offset,
                                                 uint8_t* buf,
                                                 size_t max_len)>;

    LXMFInbox(const std::string& identity_dir,
              const char* filename,
              size_t ram_capacity = DEFAULT_RAM_CAPACITY,
              uint32_t ttl_seconds = 0)
      : _path(identity_dir + "/" + filename),
        _ram_capacity(ram_capacity),
        _ttl_seconds(ttl_seconds),
        _next_seq(0)
    {
      // mailbox_stem = filename without ".jsonl" extension (used for
      // body-spill subdir: <identity_dir>/<mailbox_stem>/<seq>.body).
      std::string f = filename;
      const size_t dot = f.find_last_of('.');
      _mailbox_stem = (dot != std::string::npos) ? f.substr(0, dot) : f;
      _body_dir = identity_dir + "/" + _mailbox_stem;
    }

    void set_body_storage(BodyWriter w, BodyReader r, BodyRemover d) {
      _body_writer  = std::move(w);
      _body_reader  = std::move(r);
      _body_remover = std::move(d);
    }
    void set_body_size_reader(BodySizeReader r) {
      _body_size_reader = std::move(r);
    }
    void set_body_chunk_reader(BodyChunkReader r) {
      _body_chunk_reader = std::move(r);
    }
    const BodyChunkReader& body_chunk_reader() const { return _body_chunk_reader; }
    // Absolute path for the body file of a given seq. The writer/reader
    // callbacks see this path and pick the backend (SD-first then flash).
    std::string body_path_for(uint32_t seq) const {
      return _body_dir + "/" + std::to_string(seq) + ".body";
    }

    // Runtime overrides. set_capacity rewrites the spool if it
    // tightens the bound below the current ring size. set_ttl_seconds
    // prunes anything older than `now - ttl` if `ttl > 0`.
    void set_capacity(size_t cap) {
      _ram_capacity = cap;
      bool changed = false;
      while (_ring.size() > _ram_capacity) { _ring.pop_front(); changed = true; }
      if (changed) rewrite_spool();
    }
    void set_ttl_seconds(uint32_t ttl) {
      _ttl_seconds = ttl;
      prune_expired();
    }
    // Per-conversation TTL override. Three semantic states:
    //   - absent from map     → inherit default (_ttl_seconds)
    //   - present with value 0 → keep forever for this peer
    //   - present with value N → expire after N seconds
    // Caller passes peer_hex (destination hash, lowercase hex) so the
    // map is owner-agnostic and round-trips cleanly through the
    // persistence layer. set_peer_ttl_override returns true if the
    // override map actually changed.
    bool set_peer_ttl_override(const std::string& peer_hex, uint32_t ttl) {
      auto it = _peer_ttl.find(peer_hex);
      if (it != _peer_ttl.end() && it->second == ttl) return false;
      _peer_ttl[peer_hex] = ttl;
      prune_expired();
      return true;
    }
    bool clear_peer_ttl_override(const std::string& peer_hex) {
      if (_peer_ttl.erase(peer_hex) == 0) return false;
      prune_expired();
      return true;
    }
    void clear_all_peer_ttl_overrides() { _peer_ttl.clear(); }
    // Returns the effective TTL applied to records from `peer_hex`,
    // taking the override map into account. 0 means "no expiry".
    uint32_t effective_ttl_for(const std::string& peer_hex) const {
      auto it = _peer_ttl.find(peer_hex);
      if (it != _peer_ttl.end()) return it->second;
      return _ttl_seconds;
    }
    const std::unordered_map<std::string, uint32_t>& peer_ttl_overrides() const {
      return _peer_ttl;
    }

    // Prune entries with ts older than (now_epoch - ttl). Per-record:
    // an override on the peer wins over the default _ttl_seconds. When
    // _peer_ttl is empty and _ttl_seconds is 0 there's no work to do.
    void prune_expired() {
      if (_ttl_seconds == 0 && _peer_ttl.empty()) return;
      // Late binding so we don't pull TimeManager.h here — caller
      // (LXMFGateway) sets the clock-source once after the inbox is
      // created. We just compare ts values.
      const double now = _now_epoch();
      if (now <= 0.0) return;   // clock not calibrated
      const size_t before = _ring.size();
      for (auto it = _ring.begin(); it != _ring.end(); ) {
        const uint32_t ttl = effective_ttl_for(it->peer_hash.toHex());
        if (ttl == 0 || it->ts <= 0.0) { ++it; continue; }
        const double cutoff = now - (double)ttl;
        if (it->ts < cutoff) {
          if (_on_remove) _on_remove(*it);
          it = _ring.erase(it);
        } else {
          ++it;
        }
      }
      if (_ring.size() != before) rewrite_spool();
    }
    static void set_now_epoch_provider(double (*fn)()) { _now_epoch_provider() = fn; }

    // Replay the on-disk JSONL into RAM. Idempotent.
    //
    // Also compacts the spool if it overflowed _ram_capacity entries:
    // parse_line trims the in-memory ring to _ram_capacity, so disk
    // entries older than that have effectively been dropped from RAM
    // anyway. Rewriting the spool here turns "newest 32 in RAM, all
    // history on flash" into "newest 32 in both", which bounds flash
    // usage at ~32 * MAX_LINE_BYTES ≈ 24 KiB per identity per mailbox.
    //
    // Stopgap until proper wall-clock-anchored TTL eviction (which
    // needs a persistent RTC or a synced device clock — neither exists
    // yet). For now, "the last 32 messages" is what a sane device
    // keeps. Increase ram_capacity via the ctor if more history is
    // needed.
    void load() {
      _ring.clear();
      _next_seq = 0;
      if (!filesystem.exists(_path.c_str())) return;
      size_t file_size = filesystem.size(_path.c_str());
      if (file_size == 0) return;

      // Buffer the whole file then split on newline. Keeping it simple at
      // the cost of memory; sized to roughly the JSONL spool max.
      std::vector<uint8_t> buf;
      if (filesystem.readFile(_path.c_str(), buf) == 0) return;
      buf.push_back('\n');

      size_t line_count = 0;
      size_t start = 0;
      for (size_t i = 0; i < buf.size(); ++i) {
        if (buf[i] != '\n') continue;
        if (i > start) {
          parse_line(reinterpret_cast<const char*>(&buf[start]), i - start);
          line_count++;
        }
        start = i + 1;
      }

      // If the file had more lines than the RAM ring kept, those older
      // entries are now lost — rewrite the spool to match so flash
      // stops growing.
      if (line_count > _ring.size()) {
        NOTICEF("LXMFInbox: compacting %s (disk=%u, ram=%u)",
                _path.c_str(), (unsigned)line_count, (unsigned)_ring.size());
        rewrite_spool();
      }
    }

    // Append a new record. Allocates a seq if rec.seq == 0. Writes one
    // JSONL line; trims the RAM ring to ram_capacity. If rec.received_ms
    // is 0 the caller has not stamped it — leave it 0 here too rather than
    // stamping a millis() that's unrelated to receipt time (this code path
    // also runs during JSONL load() via parse_line, which uses its own
    // route — see below).
    bool append(MessageRecord& rec) {
      if (rec.seq == 0) rec.seq = ++_next_seq;
      else if (rec.seq > _next_seq) _next_seq = rec.seq;

      JsonDocument doc;
      doc["seq"]     = rec.seq;
      doc["ts"]      = rec.ts;
      doc["boot"]    = rec.boot_epoch;
      doc["recv_ms"] = rec.received_ms;
      doc["peer"]    = rec.peer_hash.toHex();
      doc["title"]   = rec.title;
      // Body-spill: anything > BODY_SPILL_THRESHOLD (319 bytes — the
      // transport-derived watershed for Link+Resource sends) goes to a
      // per-message file rather than embedding inline in the JSONL line.
      // Falls back to inline if the writer isn't configured or the disk
      // write fails — the JSONL line might then be oversize and rejected
      // by the serialization check below, but that surfaces clearly
      // rather than silently truncating.
      // Capture body_size BEFORE any potential clear() so it survives
      // the post-spill drop of rec.content. Authoritative for downstream
      // consumers (SPA's "should I fetch /body?" check; streaming /body
      // endpoint's Content-Length).
      const uint32_t body_size = (uint32_t)rec.content.size();
      bool spilled = false;
      if (rec.content.size() > BODY_SPILL_THRESHOLD && _body_writer) {
        if (_body_writer(body_path_for(rec.seq), rec.content)) {
          doc["body_disk"] = true;
          spilled = true;
        }
      }
      if (!spilled) doc["body"] = rec.content;
      doc["body_size"] = body_size;
      rec.body_size = body_size;
      // Hot-path memory: once the body bytes are safely on disk, drop
      // the in-memory copy. Future reads route through the streaming
      // /body endpoint, which copies disk → small SRAM scratch → TCP
      // without materialising the full body in RAM. ESP-IDF puts
      // std::string allocations > 16 KiB in PSRAM, but PSRAM is still
      // contended with WiFi/lwIP DMA buffers and the persist-and-drop
      // model is cleaner regardless of allocator.
      if (spilled) {
        rec.content.clear();
        rec.content.shrink_to_fit();
        rec.body_on_disk = true;
      } else {
        rec.body_on_disk = false;
      }
      doc["in"]      = rec.incoming;
      doc["sig"]     = rec.signature_ok;
      doc["status"]  = outbox_status_name(rec.status);
      if (rec.packet_hash.size() > 0) doc["pkt"] = rec.packet_hash.toHex();
      // Attachment metadata (file references — the bytes live on
      // disk under <identity_dir>/attachments/). Omitted entirely
      // when the record has none, to keep typical chat-message
      // lines well under MAX_LINE_BYTES.
      if (!rec.attachments.empty()) {
        JsonArray arr = doc["att"].to<JsonArray>();
        for (const auto& a : rec.attachments) {
          JsonObject o = arr.add<JsonObject>();
          o["t"] = a.tag;
          o["s"] = a.size;
          o["f"] = a.filename;
          if (!a.display_name.empty()) o["d"] = a.display_name;
          if (!a.mime.empty()) o["m"] = a.mime;
          if (!a.backend.empty()) o["b"] = a.backend;
        }
      }

      auto line = std::make_unique<char[]>(MAX_LINE_BYTES);
      size_t n = serializeJson(doc, line.get(), MAX_LINE_BYTES - 1);
      if (n == 0 || n >= MAX_LINE_BYTES - 1) {
        ERROR("LXMFInbox: append serialization failed or oversize");
        return false;
      }
      line[n] = '\n';

      microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
      if (!f) {
        ERRORF("LXMFInbox: cannot open %s for append", _path.c_str());
        return false;
      }
      size_t written = f.write(reinterpret_cast<const uint8_t*>(line.get()), n + 1);
      f.close();
      if (written != n + 1) {
        ERRORF("LXMFInbox: short write to %s (%u of %u)", _path.c_str(), (unsigned)written, (unsigned)(n + 1));
        return false;
      }

      _ring.push_back(rec);
      // Capacity eviction. UNLIMITED_CAPACITY (SIZE_MAX) makes the
      // comparison effectively always-false. Pruning the spool here
      // is fine — it just trims oldest entries off the front of the
      // ring; rewrite happens on the next state-mutating call.
      while (_ring.size() > _ram_capacity) {
        if (_on_remove) _on_remove(_ring.front());
        _ring.pop_front();
      }
      // Wall-clock TTL eviction. Cheap when ttl_seconds==0 (no-op).
      // The append flow is the natural place to run it — it bounds
      // disk + RAM growth without an extra timer.
      prune_expired();
      return true;
    }

    // Update a previously-appended record's status by packet_hash lookup
    // (outbox use). Rewrites the JSONL spool from the RAM ring if any
    // entries changed. Records older than the RAM window are not updated.
    bool update_status(const RNS::Bytes& packet_hash, OutboxStatus status) {
      bool found = false;
      for (auto& rec : _ring) {
        if (rec.packet_hash == packet_hash) {
          rec.status = status;
          found = true;
        }
      }
      if (found) rewrite_spool();
      return found;
    }

    bool mark_delivered(const RNS::Bytes& packet_hash) {
      return update_status(packet_hash, OutboxStatus::Delivered);
    }

    // Walk every record and flip any attachment whose backend matches
    // `from` to `to`. Rewrites the spool if anything changed. Used by
    // the flash→SD migration to keep the SD-unavailable warning logic
    // in the SPA honest after files move between backends.
    size_t update_attachment_backends(const std::string& from,
                                      const std::string& to) {
      size_t changed = 0;
      for (auto& rec : _ring) {
        for (auto& att : rec.attachments) {
          if (att.backend == from) {
            att.backend = to;
            ++changed;
          }
        }
      }
      if (changed > 0) rewrite_spool();
      return changed;
    }

    // Remove every record whose peer_hash matches. Rewrites the spool to
    // shrink the JSONL file. Used by the per-conversation clear endpoint.
    size_t purge_peer(const RNS::Bytes& peer_hash) {
      const size_t before = _ring.size();
      for (auto it = _ring.begin(); it != _ring.end(); ) {
        if (it->peer_hash == peer_hash) {
          if (_on_remove) _on_remove(*it);
          it = _ring.erase(it);
        } else {
          ++it;
        }
      }
      const size_t removed = before - _ring.size();
      if (removed > 0) rewrite_spool();
      return removed;
    }

    // Most recent up-to-N records (oldest first within the slice).
    std::vector<MessageRecord> recent(size_t limit) const {
      std::vector<MessageRecord> out;
      size_t take = std::min(limit, _ring.size());
      out.reserve(take);
      auto it = _ring.end();
      for (size_t i = 0; i < take; ++i) --it;
      for (; it != _ring.end(); ++it) out.push_back(*it);
      return out;
    }

    // Records with seq > since_seq, oldest first. Used by the SSE pump.
    std::vector<MessageRecord> since(uint32_t since_seq) const {
      std::vector<MessageRecord> out;
      for (const auto& rec : _ring) {
        if (rec.seq > since_seq) out.push_back(rec);
      }
      return out;
    }

    uint32_t next_seq() const { return _next_seq; }
    size_t   size()     const { return _ring.size(); }

    // Fired for every record about to be evicted (capacity, TTL, or
    // peer-purge). Used by LXMFGateway to delete the on-disk attachment
    // files that hang off the record — without this, the JSONL line goes
    // away but `/lxmf/identities/<id>/attachments/<filename>` stays
    // forever and flash fills up.
    void set_on_remove(std::function<void(const MessageRecord&)> fn) {
      _on_remove = std::move(fn);
    }

  private:
    void parse_line(const char* p, size_t n) {
      JsonDocument doc;
      if (deserializeJson(doc, p, n) != DeserializationError::Ok) return;
      MessageRecord rec;
      rec.seq         = (uint32_t)(doc["seq"]    | 0);
      rec.ts          = (double)(doc["ts"]       | 0.0);
      rec.boot_epoch  = (uint32_t)(doc["boot"]    | 0);
      rec.received_ms = (uint32_t)(doc["recv_ms"] | 0);
      std::string peer_hex = doc["peer"]    | "";
      rec.peer_hash.assignHex(peer_hex.c_str());
      rec.title    = (const char*)(doc["title"] | "");
      // body_disk records keep `content` empty in RAM — body is read
      // on demand by the streaming /body endpoint. body_size is the
      // authoritative size (used by the SPA to decide whether to
      // fetch lazily). For legacy records without body_size, fall back
      // to file_size lookup; for inline (non-spilled) records, content
      // size IS the body size.
      if (doc["body_disk"] | false) {
        rec.content = "";
        rec.body_on_disk = true;
        rec.body_size = (uint32_t)(doc["body_size"] | 0);
        if (rec.body_size == 0 && _body_size_reader) {
          // Legacy record: persisted before body_size existed. Probe
          // the spill file directly for its size. This is one stat()
          // call per legacy record at boot — acceptable.
          rec.body_size = _body_size_reader(body_path_for(rec.seq));
        }
      } else {
        rec.content = (const char*)(doc["body"]  | "");
        rec.body_on_disk = false;
        rec.body_size = (uint32_t)rec.content.size();
      }
      rec.incoming = (bool)(doc["in"]  | false);
      rec.signature_ok = (bool)(doc["sig"] | false);
      const char* status_str = doc["status"] | "delivered";
      rec.status = OutboxStatus::Delivered;
      if (strcmp(status_str, "queued")    == 0) rec.status = OutboxStatus::Queued;
      else if (strcmp(status_str, "sent") == 0) rec.status = OutboxStatus::Sent;
      else if (strcmp(status_str, "failed") == 0) rec.status = OutboxStatus::Failed;
      if (doc["pkt"].is<const char*>()) {
        std::string pkt_hex = doc["pkt"] | "";
        rec.packet_hash.assignHex(pkt_hex.c_str());
      }
      if (doc["att"].is<JsonArrayConst>()) {
        for (JsonObjectConst o : doc["att"].as<JsonArrayConst>()) {
          AttachmentMeta m;
          m.tag          = (uint8_t)(o["t"] | 0);
          m.size         = (uint32_t)(o["s"] | 0);
          m.filename     = (const char*)(o["f"] | "");
          m.display_name = (const char*)(o["d"] | "");
          m.mime         = (const char*)(o["m"] | "");
          m.backend      = (const char*)(o["b"] | "flash");  // pre-#122 records default to flash
          rec.attachments.push_back(m);
        }
      }

      if (rec.seq > _next_seq) _next_seq = rec.seq;
      _ring.push_back(rec);
      while (_ring.size() > _ram_capacity) _ring.pop_front();
    }

    void rewrite_spool() {
      filesystem.remove(_path.c_str());
      for (const auto& rec : _ring) {
        MessageRecord copy = rec;
        copy.seq = rec.seq;  // keep existing seq
        // Inline a single-shot append without re-bumping _next_seq.
        JsonDocument doc;
        doc["seq"]     = copy.seq;
        doc["ts"]      = copy.ts;
        doc["boot"]    = copy.boot_epoch;
        doc["recv_ms"] = copy.received_ms;
        doc["peer"]    = copy.peer_hash.toHex();
        doc["title"]   = copy.title;
        // Body persistence during rewrite. Three cases:
        //  1. body_on_disk already (post-#172): in-memory content is
        //     empty by design. The body file stays where it is — we
        //     just preserve body_disk + body_size in the new JSONL line.
        //  2. Inline above threshold (legacy or freshly-appended in this
        //     boot before drop): spill to disk now, then drop in-RAM
        //     content to free std::string allocation.
        //  3. Inline below threshold: write inline.
        bool spilled = false;
        if (copy.body_on_disk) {
          doc["body_disk"] = true;
          spilled = true;
        } else if (copy.content.size() > BODY_SPILL_THRESHOLD && _body_writer) {
          if (_body_writer(body_path_for(copy.seq), copy.content)) {
            doc["body_disk"] = true;
            spilled = true;
          }
        }
        if (!spilled) doc["body"] = copy.content;
        doc["body_size"] = copy.body_size > 0 ? copy.body_size : (uint32_t)copy.content.size();
        doc["in"]      = copy.incoming;
        doc["sig"]     = copy.signature_ok;
        doc["status"]  = outbox_status_name(copy.status);
        if (copy.packet_hash.size() > 0) doc["pkt"] = copy.packet_hash.toHex();
        if (!copy.attachments.empty()) {
          JsonArray arr = doc["att"].to<JsonArray>();
          for (const auto& a : copy.attachments) {
            JsonObject o = arr.add<JsonObject>();
            o["t"] = a.tag;
            o["s"] = a.size;
            o["f"] = a.filename;
            if (!a.mime.empty()) o["m"] = a.mime;
          }
        }
        auto line = std::make_unique<char[]>(MAX_LINE_BYTES);
        size_t n = serializeJson(doc, line.get(), MAX_LINE_BYTES - 1);
        if (n == 0 || n >= MAX_LINE_BYTES - 1) continue;
        line[n] = '\n';
        microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
        if (!f) return;
        f.write(reinterpret_cast<const uint8_t*>(line.get()), n + 1);
        f.close();
      }
    }

    // Read-only accessors for /api/inbox_config to surface state.
    size_t   ram_capacity() const { return _ram_capacity; }
    uint32_t ttl_seconds()  const { return _ttl_seconds; }

  private:
    static double (*&_now_epoch_provider())() {
      static double (*fn)() = nullptr;
      return fn;
    }
    static double _now_epoch() {
      auto fn = _now_epoch_provider();
      return fn ? fn() : 0.0;
    }

    std::string                _path;
    std::string                _mailbox_stem;  // "inbox" or "outbox"
    std::string                _body_dir;      // <identity_dir>/<mailbox_stem>
    size_t                     _ram_capacity;
    uint32_t                   _ttl_seconds = 0;
    std::unordered_map<std::string, uint32_t> _peer_ttl;  // peer_hex → ttl_seconds
    uint32_t                   _next_seq;
    std::deque<MessageRecord>  _ring;
    std::function<void(const MessageRecord&)> _on_remove;
    BodyWriter                 _body_writer;
    BodyReader                 _body_reader;
    BodyRemover                _body_remover;
    BodySizeReader             _body_size_reader;
    BodyChunkReader            _body_chunk_reader;
  };

} // namespace LXMF
