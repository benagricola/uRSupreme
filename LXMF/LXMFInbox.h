#pragma once

#include <ArduinoJson.h>
#include <Log.h>
#include <microStore/FileSystem.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
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
    static constexpr size_t MAX_LINE_BYTES       = 768;

    LXMFInbox(const std::string& identity_dir,
              const char* filename,
              size_t ram_capacity = DEFAULT_RAM_CAPACITY,
              uint32_t ttl_seconds = 0)
      : _path(identity_dir + "/" + filename),
        _ram_capacity(ram_capacity),
        _ttl_seconds(ttl_seconds),
        _next_seq(0) {}

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
    // Prune entries with ts older than (now_epoch - ttl). No-op when
    // ttl is 0 (disabled) or the device clock is uncalibrated.
    void prune_expired() {
      if (_ttl_seconds == 0) return;
      // Late binding so we don't pull TimeManager.h here — caller
      // (LXMFGateway) sets the clock-source once after the inbox is
      // created. We just compare ts values.
      const double cutoff = _now_epoch() - (double)_ttl_seconds;
      if (cutoff <= 0.0) return;   // clock not calibrated
      const size_t before = _ring.size();
      for (auto it = _ring.begin(); it != _ring.end(); ) {
        if (it->ts > 0.0 && it->ts < cutoff) {
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
      doc["body"]    = rec.content;
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

      char line[MAX_LINE_BYTES];
      size_t n = serializeJson(doc, line, sizeof(line) - 1);
      if (n == 0 || n >= sizeof(line) - 1) {
        ERROR("LXMFInbox: append serialization failed or oversize");
        return false;
      }
      line[n] = '\n';

      microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
      if (!f) {
        ERRORF("LXMFInbox: cannot open %s for append", _path.c_str());
        return false;
      }
      size_t written = f.write(reinterpret_cast<const uint8_t*>(line), n + 1);
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
      rec.content  = (const char*)(doc["body"]  | "");
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
        doc["body"]    = copy.content;
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
        char line[MAX_LINE_BYTES];
        size_t n = serializeJson(doc, line, sizeof(line) - 1);
        if (n == 0 || n >= sizeof(line) - 1) continue;
        line[n] = '\n';
        microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
        if (!f) return;
        f.write(reinterpret_cast<const uint8_t*>(line), n + 1);
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
    size_t                     _ram_capacity;
    uint32_t                   _ttl_seconds = 0;
    uint32_t                   _next_seq;
    std::deque<MessageRecord>  _ring;
    std::function<void(const MessageRecord&)> _on_remove;
  };

} // namespace LXMF
