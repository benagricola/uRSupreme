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
  // No rotation for now: at ~256 B/message the JSONL grows to ~64 KB
  // after ~250 messages, which is well within the LittleFS partition.
  // Rotation can be added later if needed.
  class LXMFInbox {
  public:
    // Per-identity per-mailbox cap. At ~300 B per JSONL line for a
    // typical chat-sized message, 200 entries = ~60 KB / identity /
    // mailbox — comfortable on the LittleFS partition.
    //
    // Retention model:
    //   * _ram_capacity is the per-identity-per-mailbox SAFETY bound on
    //     the in-RAM ring. Not user-configurable today; defends against
    //     unbounded growth.
    //   * _default_retention is the global default: applied to any peer
    //     that DOESN'T have an explicit per-peer override.
    //   * _peer_retention[peer_hex] is the per-peer override. Absent
    //     entry = inherit (the default is consulted every prune).
    //     The per-chat retention modal's "Use identity default" radio
    //     clears the override, putting the peer back into inherit-mode
    //     so subsequent default changes cascade.
    //
    // TTL/Time pruning uses the LXMF `ts` field (sender wall-clock);
    // requires a calibrated local clock — the prune is a no-op while
    // uncalibrated.
    static constexpr size_t DEFAULT_RAM_CAPACITY = 200;
    static constexpr size_t UNLIMITED_CAPACITY   = SIZE_MAX;
    LXMFInbox(const std::string& identity_dir,
              const char* filename,
              size_t ram_capacity = DEFAULT_RAM_CAPACITY,
              Retention default_retention = Retention{})
      : _path(identity_dir + "/" + filename),
        _ram_capacity(ram_capacity),
        _default_retention(default_retention),
        _next_seq(0)
    {
      std::string f = filename;
      const size_t dot = f.find_last_of('.');
      _mailbox_stem = (dot != std::string::npos) ? f.substr(0, dot) : f;
    }

    void set_capacity(size_t cap) {
      _ram_capacity = cap;
      bool changed = false;
      while (_ring.size() > _ram_capacity) { _ring.pop_front(); changed = true; }
      if (changed) rewrite_spool();
    }
    // Global default. Inherit-by-default: every peer without an
    // explicit override consults this at prune time. Changing this
    // immediately affects all inheriting peers on the next prune.
    void set_default_retention(Retention r) {
      _default_retention = r;
      prune_expired();
    }
    Retention default_retention() const { return _default_retention; }

    // Per-conversation retention override. Two cases:
    //   - absent from map: inherit-mode. _default_retention applies at
    //     every prune.
    //   - present:         explicit per-chat setting. May be Kind::None
    //     (keep forever for this peer specifically), Kind::Time, or
    //     Kind::Count.
    // Returns true if the map actually changed.
    bool set_peer_retention(const std::string& peer_hex, Retention r) {
      auto it = _peer_retention.find(peer_hex);
      if (it != _peer_retention.end() && it->second == r) return false;
      _peer_retention[peer_hex] = r;
      prune_expired();
      return true;
    }
    bool clear_peer_retention(const std::string& peer_hex) {
      if (_peer_retention.erase(peer_hex) == 0) return false;
      prune_expired();
      return true;
    }
    void clear_all_peer_retention() { _peer_retention.clear(); }

    // Returns the effective retention for `peer_hex`: the per-peer
    // override if present, else _default_retention.
    Retention effective_retention_for(const std::string& peer_hex) const {
      auto it = _peer_retention.find(peer_hex);
      if (it != _peer_retention.end()) return it->second;
      return _default_retention;
    }
    const std::unordered_map<std::string, Retention>& peer_retention_overrides() const {
      return _peer_retention;
    }

    // Apply both time-based and count-based retention to the ring.
    // Time pruning runs first (records older than the cutoff for their
    // peer drop out); count pruning then keeps only the newest N
    // messages per peer where the peer's Retention is Count. Hard
    // _ram_capacity cap remains the outer safety bound.
    void prune_expired() {
      if (_peer_retention.empty()
          && _default_retention.kind == Retention::Kind::None) {
        // Still enforce the hard cap even when no policy is set, since
        // append() relies on prune to keep ring growth bounded over
        // very long uptimes.
        while (_ring.size() > _ram_capacity) _ring.pop_front();
        return;
      }
      const double now = _now_epoch();
      const size_t before = _ring.size();

      // Pass 1: Time-based eviction. Skipped if clock isn't calibrated
      // (no wall-clock anchor, no way to compute age).
      if (now > 0.0) {
        for (auto it = _ring.begin(); it != _ring.end(); ) {
          const Retention r = effective_retention_for(it->peer_hash.toHex());
          if (r.kind != Retention::Kind::Time || r.value == 0 || it->ts <= 0.0) {
            ++it;
            continue;
          }
          const double cutoff = now - (double)r.value;
          if (it->ts < cutoff) {
            if (_on_remove) _on_remove(*it);
            it = _ring.erase(it);
          } else {
            ++it;
          }
        }
      }

      // Pass 2: Count-based eviction per peer. Walk the ring (oldest →
      // newest), bucket indices by peer. For peers with Kind::Count,
      // drop the oldest entries until the bucket fits in the cap.
      // Pass operates from oldest to newest because deque is amortised-
      // constant for pop_front; an erase-by-index pass is simpler than
      // tracking iterators across mutations.
      std::unordered_map<std::string, std::vector<size_t>> by_peer;
      by_peer.reserve(_peer_retention.size());
      for (size_t i = 0; i < _ring.size(); ++i) {
        by_peer[_ring[i].peer_hash.toHex()].push_back(i);
      }
      std::vector<size_t> drop;
      drop.reserve(8);
      for (auto& kv : by_peer) {
        const Retention r = effective_retention_for(kv.first);
        if (r.kind != Retention::Kind::Count || r.value == 0) continue;
        if (kv.second.size() <= r.value) continue;
        const size_t excess = kv.second.size() - r.value;
        for (size_t k = 0; k < excess; ++k) drop.push_back(kv.second[k]);
      }
      if (!drop.empty()) {
        std::sort(drop.begin(), drop.end(), std::greater<size_t>());
        for (size_t idx : drop) {
          if (idx < _ring.size()) {
            if (_on_remove) _on_remove(_ring[idx]);
            _ring.erase(_ring.begin() + idx);
          }
        }
      }

      // Outer safety bound — always enforced.
      while (_ring.size() > _ram_capacity) _ring.pop_front();
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
      // Outer safety cap: keep the ring under _ram_capacity regardless
      // of retention. UNLIMITED_CAPACITY (SIZE_MAX) makes this a no-op.
      while (_ring.size() > _ram_capacity) {
        if (_on_remove) _on_remove(_ring.front());
        _ring.pop_front();
      }
      // Per-peer retention pass (time + count). Cheap when nothing
      // is configured; the append flow is the natural place to run
      // it so disk + RAM growth stay bounded without an extra timer.
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

    // Apply an arbitrary mutation to the record with this seq and
    // persist by rewriting the spool. Used by the stamped-send path to
    // flip a generating_stamp record into its dispatched form (status,
    // packet_hash, stamp value) in place — the record was appended
    // before generation so it survives a reboot, so dispatch must
    // update rather than append a duplicate.
    bool mutate_by_seq(uint32_t seq, const std::function<void(MessageRecord&)>& fn) {
      for (auto& rec : _ring) {
        if (rec.seq != seq) continue;
        fn(rec);
        rewrite_spool();
        return true;
      }
      return false;
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
          if (att.backend == from.c_str()) {
            att.backend = to.c_str();
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
    const MessageRing& ring() const { return _ring; }

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
    // Reserve the seq a future append() will carry, without writing a record.
    // Used by the auto-send queue so an optimistic "finding route" bubble and
    // the eventual real outbox record share one seq (the SPA dedups on it).
    uint32_t reserve_seq() { return ++_next_seq; }
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
      else if (strcmp(status_str, "generating_stamp") == 0) rec.status = OutboxStatus::GeneratingStamp;
      // Delivery-stamp state — present only when a stamp policy applied.
      if (doc["stamp_ok"].is<bool>()) {
        rec.stamp_checked = true;
        rec.stamp_valid   = (bool)doc["stamp_ok"];
        rec.stamp_value   = (int16_t)(doc["stampv"] | -1);
      }
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
      if (rec.stamp_checked) {
        doc["stamp_ok"] = rec.stamp_valid;
        if (rec.stamp_value >= 0) doc["stampv"] = rec.stamp_value;
      }
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

    // Read-only accessors for the SPA / API.
    size_t ram_capacity() const { return _ram_capacity; }

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
    Retention                  _default_retention;
    std::unordered_map<std::string, Retention> _peer_retention;
    uint32_t                   _next_seq;
    MessageRing                _ring;
    std::function<void(const MessageRecord&)> _on_remove;
  };

} // namespace LXMF
