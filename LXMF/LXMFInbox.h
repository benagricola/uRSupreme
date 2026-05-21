#pragma once

#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
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
    // Bodies whose wire size needed Link+Resource transport (i.e. >319
    LXMFInbox(const std::string& identity_dir,
              const char* filename,
              size_t ram_capacity = DEFAULT_RAM_CAPACITY,
              uint32_t ttl_seconds = 0)
      : _path(identity_dir + "/" + filename),
        _ram_capacity(ram_capacity),
        _ttl_seconds(ttl_seconds),
        _next_seq(0)
    {
      // mailbox_stem retained for any future per-mailbox subdir use
      // (attachment storage already uses the parent identity dir).
      std::string f = filename;
      const size_t dot = f.find_last_of('.');
      _mailbox_stem = (dot != std::string::npos) ? f.substr(0, dot) : f;
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
    // usage at ~32 records per identity per mailbox.
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
      if (filesystem.size(_path.c_str()) == 0) return;

      // microStore::File inherits from Arduino's Stream, which means
      // ArduinoJson can read one self-delimited JSON document at a
      // time directly from the file — JSON's matching braces tell
      // it when one record ends. No manual line accumulator, no
      // per-line size cap. Records are bounded purely by the field-
      // level caps applied at the trust boundary (title, body,
      // attachment count + name + mime).
      microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeRead);
      if (!f) return;

      size_t parsed = 0;
      size_t errors = 0;
      while (f.available() > 0) {
        // Skip any inter-record whitespace (typically just '\n').
        int peek;
        while ((peek = f.peek()) != -1
               && (peek == '\n' || peek == '\r' || peek == ' ' || peek == '\t')) {
          f.read();
        }
        if (f.peek() == -1) break;

        Common::PsramJsonDocument doc;
        DeserializationError err = deserializeJson(doc, f);
        if (err) {
          // Malformed record — scan to the next newline and resume.
          // Doesn't drop subsequent records, just the corrupt one.
          errors++;
          int c;
          while (f.available() > 0 && (c = f.read()) != '\n' && c != -1) {}
          continue;
        }
        apply_doc_to_ring(doc);
        parsed++;
      }
      f.close();
      if (errors) {
        WARNINGF("LXMFInbox: %u parse error(s) in %s — corrupt records skipped",
                 (unsigned)errors, _path.c_str());
      }

      // Compact if the disk had more records than the RAM ring kept.
      if (parsed > _ring.size()) {
        NOTICEF("LXMFInbox: compacting %s (disk=%u, ram=%u)",
                _path.c_str(), (unsigned)parsed, (unsigned)_ring.size());
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
      rec.body_size = (uint32_t)rec.content.size();

      Common::PsramJsonDocument doc;
      record_to_doc(rec, doc);

      microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
      if (!f) {
        ERRORF("LXMFInbox: cannot open %s for append", _path.c_str());
        return false;
      }
      // Stream the JSON directly to the file via Arduino's Print
      // interface (microStore::File : public Stream). No intermediate
      // contiguous buffer — record size is bounded by the field caps
      // already enforced at the trust boundary.
      const size_t n = serializeJson(doc, f);
      const size_t nl = f.write((uint8_t)'\n');
      f.close();
      if (n == 0 || nl != 1) {
        ERRORF("LXMFInbox: short write to %s", _path.c_str());
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

    // Read-only access to the in-memory ring. Callers iterate
    // directly + apply their own filter without forcing a vector copy.
    // The previous recent()/since() accessors returned vectors by
    // value — each call to /api/identities/{}/inbox or /outbox copied
    // up to ~200 MessageRecords (each with std::string title +
    // attachments vector) on the default heap, ~10-20 KiB transient
    // per request on an ESP32 with constrained internal SRAM.
    const std::deque<MessageRecord>& ring() const { return _ring; }

    // Direct seq lookup — returns nullptr if not in the ring. Used by
    // the outbox-retry endpoint to find one record by seq without
    // walking a copy of the entire ring.
    const MessageRecord* find_by_seq(uint32_t seq) const {
      for (const auto& rec : _ring) {
        if (rec.seq == seq) return &rec;
      }
      return nullptr;
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
    // Decode a parsed JsonDocument into a MessageRecord. Used by load()
    // for each streamed record from the JSONL spool.
    static void doc_to_record(const JsonDocument& doc, MessageRecord& rec) {
      rec.seq         = (uint32_t)(doc["seq"]    | 0);
      rec.ts          = (double)(doc["ts"]       | 0.0);
      rec.boot_epoch  = (uint32_t)(doc["boot"]    | 0);
      rec.received_ms = (uint32_t)(doc["recv_ms"] | 0);
      std::string peer_hex = doc["peer"]    | "";
      rec.peer_hash.assignHex(peer_hex.c_str());
      rec.title    = (const char*)(doc["title"] | "");
      rec.content   = (const char*)(doc["body"]  | "");
      rec.body_size = (uint32_t)(doc["body_size"] | (uint32_t)rec.content.size());
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
          m.backend      = (const char*)(o["b"] | "flash");
          rec.attachments.push_back(m);
        }
      }
    }

    // Encode a MessageRecord into a JsonDocument. Used by append() and
    // rewrite_spool() to share one serialization layout.
    static void record_to_doc(const MessageRecord& rec, JsonDocument& doc) {
      doc["seq"]       = rec.seq;
      doc["ts"]        = rec.ts;
      doc["boot"]      = rec.boot_epoch;
      doc["recv_ms"]   = rec.received_ms;
      doc["peer"]      = rec.peer_hash.toHex();
      doc["title"]     = rec.title;
      doc["body"]      = rec.content;
      doc["body_size"] = rec.body_size > 0 ? rec.body_size : (uint32_t)rec.content.size();
      doc["in"]        = rec.incoming;
      doc["sig"]       = rec.signature_ok;
      doc["status"]    = outbox_status_name(rec.status);
      if (rec.packet_hash.size() > 0) doc["pkt"] = rec.packet_hash.toHex();
      if (!rec.attachments.empty()) {
        JsonArray arr = doc["att"].to<JsonArray>();
        for (const auto& a : rec.attachments) {
          JsonObject o = arr.add<JsonObject>();
          o["t"] = a.tag;
          o["s"] = a.size;
          o["f"] = a.filename;
          if (!a.display_name.empty()) o["d"] = a.display_name;
          if (!a.mime.empty())         o["m"] = a.mime;
          if (!a.backend.empty())      o["b"] = a.backend;
        }
      }
    }

    // load()-side post-deserialise step: build a MessageRecord and add
    // it to the in-RAM ring, with seq + capacity bookkeeping.
    void apply_doc_to_ring(const JsonDocument& doc) {
      MessageRecord rec;
      doc_to_record(doc, rec);
      if (rec.seq > _next_seq) _next_seq = rec.seq;
      _ring.push_back(std::move(rec));
      while (_ring.size() > _ram_capacity) _ring.pop_front();
    }

    void rewrite_spool() {
      filesystem.remove(_path.c_str());
      // Open the spool once for the whole batch; one file handle per
      // record (the old shape) was ~3× slower and produced extra
      // wear on flash with no benefit.
      microStore::File f = filesystem.open(_path.c_str(), microStore::File::ModeAppend, true);
      if (!f) {
        ERRORF("LXMFInbox: cannot open %s for rewrite", _path.c_str());
        return;
      }
      for (const auto& rec : _ring) {
        Common::PsramJsonDocument doc;
        record_to_doc(rec, doc);
        if (serializeJson(doc, f) == 0) continue;
        f.write((uint8_t)'\n');
      }
      f.close();
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
    size_t                     _ram_capacity;
    uint32_t                   _ttl_seconds = 0;
    std::unordered_map<std::string, uint32_t> _peer_ttl;  // peer_hex → ttl_seconds
    uint32_t                   _next_seq;
    std::deque<MessageRecord>  _ring;
    std::function<void(const MessageRecord&)> _on_remove;
  };

} // namespace LXMF
