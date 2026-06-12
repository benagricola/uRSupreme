// Per-peer telemetry sharing: one-shot attaches, time-boxed live
// GRANTS (we answer the peer's requests), and live FEEDS (we request
// from the peer at the rate their offer named).
//
// Attaching telemetry to a message (the compose popover, or the OLED
// messenger's position flag) packs the selected sensor items as a
// FIELD_TELEMETRY blob - the convention Sideband embeds in its own
// messages and renders natively (core.py:4462). When the sender picks
// a live window, that send also records a GRANT for the recipient:
// while the grant lasts, the peer may send Commands.TELEMETRY_REQUEST
// (FIELD_COMMANDS 0x09, key 0x01 - Sideband's request flow,
// core.py:1356/5244) and this device answers with fresh readings of
// the granted items. A newer telemetry send to the same peer
// supersedes the grant - items, window and rate - from that moment.
//
// The same send carries the OFFER to the recipient in
// FIELD_CUSTOM_META (LXMF.py:36, the app-extension field):
// {"urtn_live": [for_s, every_s]}. A receiver running this firmware
// starts a FEED: it sends TELEMETRY_REQUEST every `every_s` until the
// window ends, keeps the latest answer for the UI, and stops early
// when the user cancels the feed or the sender stops answering
// (MAX_FEED_MISSES consecutive unanswered requests). Sideband
// receivers ignore the custom meta and can still request manually
// inside the window. Either side cancels independently: the sender
// revokes the grant (requests go unanswered), the receiver drops the
// feed (requests stop).
//
// Grants and feeds live in RAM only, on purpose: a reboot ends all
// live shares, which errs on the private side. Bounds: MAX_GRANTS /
// MAX_FEEDS peers, answers rate-limited per peer, pending-answer ring
// of 8, feed blobs capped at MAX_TELEMETRY_BLOB.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "../Common/MsgPack.h"
#include "../Common/RnsLock.h"
#include "../Telemetry/Telemeter.h"
#include "LXMFTypes.h"
#include "LXMFGateway.h"

// WS push for a feed's fresh readings - defined in Web/WebSocket.h,
// which includes us via WebUI.h (the usual cycle-breaking hook).
namespace Web { namespace WS {
  void publish_telemetry_update(const LXMF::IdentityId& identity_id,
                                const RNS::Bytes& peer,
                                const RNS::Bytes& blob);
} }

namespace LXMF {
namespace TelemetryShare {

// Item bitmask - mirrors Telemetry::Telemeter::Include.
inline constexpr uint8_t ITEM_LOCATION    = 0x01;
inline constexpr uint8_t ITEM_ENVIRONMENT = 0x02;
inline constexpr uint8_t ITEM_BATTERY     = 0x04;
inline constexpr uint8_t ITEM_COMPASS     = 0x08;

inline constexpr size_t   MAX_GRANTS          = 16;
inline constexpr size_t   MAX_FEEDS           = 8;
inline constexpr size_t   MAX_PENDING         = 8;
inline constexpr uint32_t MIN_ANSWER_GAP_MS   = 5000;
inline constexpr uint32_t MAX_SHARE_S         = 24 * 3600;
inline constexpr uint32_t MIN_RATE_S          = 15;
inline constexpr uint32_t MAX_RATE_S          = 3600;
inline constexpr uint32_t MAX_FEED_MISSES     = 3;

struct Grant {
  IdentityId iden;            // identity that granted
  RNS::Bytes peer;            // recipient allowed to request
  uint8_t    items = 0;
  double     expires_epoch = 0.0;
  uint32_t   last_answer_ms = 0;
};

// A live feed FROM a peer: their offer told us the window and the
// rate; we poll and keep the latest answer for the UI.
struct Feed {
  IdentityId iden;
  RNS::Bytes peer;
  double     expires_epoch = 0.0;
  uint32_t   every_s       = 60;
  uint32_t   last_req_ms   = 0;
  bool       awaiting      = false;  // request out, no answer yet
  uint32_t   misses        = 0;
  RNS::Bytes latest;                 // newest packed Telemeter blob
  uint32_t   latest_ms     = 0;      // millis() of the newest blob
};

namespace _detail {
  inline std::vector<Grant>& grants() { static std::vector<Grant> v; return v; }
  inline std::vector<Feed>&  feeds()  { static std::vector<Feed>  v; return v; }
  struct Pending { IdentityId iden; RNS::Bytes peer; };
  inline std::vector<Pending>& pending() { static std::vector<Pending> v; return v; }

  inline double now_epoch(const IdentityId& iden) {
    const LXMFIdentity* a = LXMFGateway::identity_by_id(iden);
    return a ? a->lxmf.get_timestamp() : 0.0;
  }

  inline Grant* find_grant(const IdentityId& iden, const RNS::Bytes& peer) {
    for (auto& g : grants()) {
      if (g.iden == iden && g.peer == peer) return &g;
    }
    return nullptr;
  }

  inline Feed* find_feed(const IdentityId& iden, const RNS::Bytes& peer) {
    for (auto& f : feeds()) {
      if (f.iden == iden && f.peer == peer) return &f;
    }
    return nullptr;
  }

  template <typename T>
  inline void erase_entry(std::vector<T>& v, T* e) {
    v.erase(v.begin() + (e - v.data()));
  }

  // Evict the entry closest to expiry to make room.
  template <typename T>
  inline void evict_soonest(std::vector<T>& v) {
    size_t victim = 0;
    for (size_t i = 1; i < v.size(); ++i) {
      if (v[i].expires_epoch < v[victim].expires_epoch) victim = i;
    }
    v.erase(v.begin() + victim);
  }
}

inline Telemetry::Telemeter::Include include_for(uint8_t items) {
  Telemetry::Telemeter::Include inc;
  inc.location    = (items & ITEM_LOCATION)    != 0;
  inc.environment = (items & ITEM_ENVIRONMENT) != 0;
  inc.battery     = (items & ITEM_BATTERY)     != 0;
  inc.magnetic    = (items & ITEM_COMPASS)     != 0;
  return inc;
}

// Pack the selected items into a FIELD_TELEMETRY blob. Empty result
// means nothing useful was available (e.g. location-only with no fix).
inline RNS::Bytes pack_items(const IdentityId& iden, uint8_t items) {
  if (items == 0) return RNS::Bytes{};
  uint8_t packed[Telemetry::Telemeter::MAX_PACKED];
  const size_t n = Telemetry::Telemeter::pack(
      packed, sizeof(packed), include_for(items), _detail::now_epoch(iden));
  if (n == 0) return RNS::Bytes{};
  return RNS::Bytes(packed, n);
}

// The FIELD_CUSTOM_META value carried to the recipient. Up to two
// keys: "urtn_live": [for_s, every_s] - the live-update offer (for_s 0
// tells the receiver to stop an existing feed) - and "urtn_msg": 1,
// which marks a deliberately composed telemetry-only send so the
// receiving end shows it as a message instead of swallowing it as
// machinery. Either may be absent; empty result = no meta field.
inline RNS::Bytes pack_meta(uint32_t for_s, uint32_t every_s, bool mark_message) {
  using namespace Common::MsgPack;
  const bool live = every_s > 0 || for_s > 0;
  size_t entries = (live ? 1 : 0) + (mark_message ? 1 : 0);
  if (entries == 0) return RNS::Bytes{};
  uint8_t buf[48];
  size_t pos = 0, n = 0;
  n = pack_map_header(buf, sizeof(buf), entries);
  if (n == 0) return RNS::Bytes{};
  pos += n;
  if (live) {
    n = pack_str(&buf[pos], sizeof(buf) - pos, "urtn_live");
    if (n == 0) return RNS::Bytes{};
    pos += n;
    n = pack_array_header(&buf[pos], sizeof(buf) - pos, 2);
    if (n == 0) return RNS::Bytes{};
    pos += n;
    n = pack_int(&buf[pos], sizeof(buf) - pos, (int64_t)for_s);
    if (n == 0) return RNS::Bytes{};
    pos += n;
    n = pack_int(&buf[pos], sizeof(buf) - pos, (int64_t)every_s);
    if (n == 0) return RNS::Bytes{};
    pos += n;
  }
  if (mark_message) {
    n = pack_str(&buf[pos], sizeof(buf) - pos, "urtn_msg");
    if (n == 0) return RNS::Bytes{};
    pos += n;
    if (pos >= sizeof(buf)) return RNS::Bytes{};
    buf[pos++] = 0x01;
  }
  return RNS::Bytes(buf, pos);
}

// True when the custom meta carries the urtn_msg marker (see
// pack_meta). Consulted by the gateway's inbox gate.
inline bool meta_marks_message(const RNS::Bytes& raw) {
  const uint8_t* d = raw.data();
  const size_t len = raw.size();
  size_t off = 0;
  if (len == 0) return false;
  const uint8_t mt = d[off];
  size_t entries = 0;
  if ((mt & 0xF0) == 0x80) { entries = mt & 0x0F; off += 1; }
  else if (mt == 0xDE && off + 2 < len) {
    entries = ((size_t)d[off + 1] << 8) | d[off + 2]; off += 3;
  } else return false;
  for (size_t i = 0; i < entries && off < len; ++i) {
    const std::string key = Common::MsgPack::read_bin_or_str(d, len, off);
    if (key.empty()) return false;
    if (key == "urtn_msg") return true;
    if (!Common::MsgPack::skip_element(d, len, off)) return false;
  }
  return false;
}

// Sideband's telemetry request: fields[FIELD_COMMANDS] =
// [{Commands.TELEMETRY_REQUEST: [timebase, is_collector_request]}]
// (core.py:1357). We always request as a plain peer, not a collector.
inline RNS::Bytes pack_telemetry_request(double timebase) {
  using namespace Common::MsgPack;
  uint8_t buf[24];
  size_t pos = 0;
  buf[pos++] = 0x91;        // fixarray(1)
  buf[pos++] = 0x81;        // fixmap(1)
  buf[pos++] = 0x01;        // Commands.TELEMETRY_REQUEST (sense.py:13)
  buf[pos++] = 0x92;        // fixarray(2)
  size_t n = pack_float64(&buf[pos], sizeof(buf) - pos, timebase);
  if (n == 0) return RNS::Bytes{};
  pos += n;
  buf[pos++] = 0xC2;        // is_collector_request: false
  return RNS::Bytes(buf, pos);
}

// Record (or supersede) the live-share grant for `peer`. share_s = 0
// removes any grant - the attach was one-shot.
inline void record_grant(const IdentityId& iden, const RNS::Bytes& peer,
                         uint8_t items, uint32_t share_s) {
  auto& v = _detail::grants();
  Grant* g = _detail::find_grant(iden, peer);
  if (share_s == 0 || items == 0) {
    if (g != nullptr) _detail::erase_entry(v, g);
    return;
  }
  if (share_s > MAX_SHARE_S) share_s = MAX_SHARE_S;
  const double expires = _detail::now_epoch(iden) + (double)share_s;
  if (g == nullptr) {
    if (v.size() >= MAX_GRANTS) _detail::evict_soonest(v);
    v.push_back(Grant{});
    g = &v.back();
    g->iden = iden;
    g->peer = peer;
  }
  // A newer telemetry send supersedes the allowed set and the window.
  g->items         = items;
  g->expires_epoch = expires;
  NOTICEF("TelemetryShare: grant for %s items=0x%02x for %lus",
          peer.toHex().c_str(), (unsigned)items, (unsigned long)share_s);
}

inline void remove_grant(const IdentityId& iden, const RNS::Bytes& peer) {
  Grant* g = _detail::find_grant(iden, peer);
  if (g != nullptr) _detail::erase_entry(_detail::grants(), g);
}

inline void remove_feed(const IdentityId& iden, const RNS::Bytes& peer) {
  Feed* f = _detail::find_feed(iden, peer);
  if (f != nullptr) _detail::erase_entry(_detail::feeds(), f);
}

// Read-only views for the web layer (/api/telemetry/shares).
inline const std::vector<Grant>& grants() { return _detail::grants(); }
inline const std::vector<Feed>&  feeds()  { return _detail::feeds();  }
inline double share_now_epoch(const IdentityId& iden) { return _detail::now_epoch(iden); }

// Inbound FIELD_COMMANDS handler, called from the gateway's delivery
// path with the raw msgpack value (an array of one-key maps,
// core.py:1356). Only Commands.TELEMETRY_REQUEST (0x01) is acted on,
// and only when the requester holds an unexpired grant; everything
// else is ignored. Answers are queued for the next tick - sending
// from inside the delivery callback would re-enter the LXMF stack.
inline void on_commands(const IdentityId& iden, const RNS::Bytes& peer,
                        const RNS::Bytes& raw) {
  const uint8_t* d = raw.data();
  const size_t len = raw.size();
  size_t off = 0;
  if (len == 0) return;
  // Array header.
  size_t n = 0;
  const uint8_t t = d[off];
  if ((t & 0xF0) == 0x90) { n = t & 0x0F; off += 1; }
  else if (t == 0xDC && off + 2 < len) {
    n = ((size_t)d[off + 1] << 8) | d[off + 2]; off += 3;
  } else return;
  bool requested = false;
  for (size_t i = 0; i < n && off < len; ++i) {
    // Each command is a one-entry map {command_id: args}.
    const uint8_t mt = d[off];
    size_t entries = 0;
    if ((mt & 0xF0) == 0x80) { entries = mt & 0x0F; off += 1; }
    else return;
    for (size_t e = 0; e < entries && off < len; ++e) {
      const uint8_t key = d[off];
      if (key > 0x7F) return;   // command ids are small fixints
      off += 1;
      if (key == 0x01) requested = true;   // Commands.TELEMETRY_REQUEST
      if (!Common::MsgPack::skip_element(d, len, off)) return;
    }
  }
  if (!requested) return;
  Grant* g = _detail::find_grant(iden, peer);
  if (g == nullptr) return;
  if (_detail::now_epoch(iden) > g->expires_epoch) return;
  auto& q = _detail::pending();
  for (const auto& p : q) {
    if (p.iden == iden && p.peer == peer) return;   // already queued
  }
  if (q.size() >= MAX_PENDING) return;
  q.push_back(_detail::Pending{ iden, peer });
}

// Inbound live-update offer (FIELD_CUSTOM_META {"urtn_live":
// [for_s, every_s]}), called from the gateway's delivery path. Starts
// or supersedes the feed; for_s 0 stops it.
inline void on_live_offer(const IdentityId& iden, const RNS::Bytes& peer,
                          const RNS::Bytes& raw) {
  const uint8_t* d = raw.data();
  const size_t len = raw.size();
  size_t off = 0;
  if (len == 0) return;
  const uint8_t mt = d[off];
  size_t entries = 0;
  if ((mt & 0xF0) == 0x80) { entries = mt & 0x0F; off += 1; }
  else if (mt == 0xDE && off + 2 < len) {
    entries = ((size_t)d[off + 1] << 8) | d[off + 2]; off += 3;
  } else return;
  for (size_t i = 0; i < entries && off < len; ++i) {
    const std::string key = Common::MsgPack::read_bin_or_str(d, len, off);
    if (key.empty()) return;
    if (key != "urtn_live") {
      if (!Common::MsgPack::skip_element(d, len, off)) return;
      continue;
    }
    if (off >= len || (d[off] & 0xF0) != 0x90 || (d[off] & 0x0F) < 2) return;
    off += 1;
    // Both values are small unsigned ints (fixint / uint8-32).
    auto read_uint = [&](uint32_t* out) -> bool {
      if (off >= len) return false;
      const uint8_t v = d[off];
      if (v <= 0x7F)  { *out = v; off += 1; return true; }
      size_t width = 0;
      if (v == 0xCC) width = 1; else if (v == 0xCD) width = 2;
      else if (v == 0xCE) width = 4; else return false;
      if (off + 1 + width > len) return false;
      uint32_t acc = 0;
      for (size_t b = 0; b < width; ++b) acc = (acc << 8) | d[off + 1 + b];
      *out = acc;
      off += 1 + width;
      return true;
    };
    uint32_t for_s = 0, every_s = 0;
    if (!read_uint(&for_s) || !read_uint(&every_s)) return;

    auto& v = _detail::feeds();
    Feed* f = _detail::find_feed(iden, peer);
    if (for_s == 0) {
      if (f != nullptr) _detail::erase_entry(v, f);
      return;
    }
    if (for_s > MAX_SHARE_S) for_s = MAX_SHARE_S;
    if (every_s < MIN_RATE_S) every_s = MIN_RATE_S;
    if (every_s > MAX_RATE_S) every_s = MAX_RATE_S;
    if (f == nullptr) {
      if (v.size() >= MAX_FEEDS) _detail::evict_soonest(v);
      v.push_back(Feed{});
      f = &v.back();
      f->iden = iden;
      f->peer = peer;
    }
    // A newer offer supersedes the window and the rate. The carrying
    // message brought fresh telemetry, so the first poll waits a full
    // interval.
    f->expires_epoch = _detail::now_epoch(iden) + (double)for_s;
    f->every_s       = every_s;
    f->last_req_ms   = millis();
    f->awaiting      = false;
    f->misses        = 0;
    NOTICEF("TelemetryShare: live feed from %s every %lus for %lus",
            peer.toHex().c_str(), (unsigned long)every_s, (unsigned long)for_s);
    return;
  }
}

// Inbound telemetry from a peer we hold a feed for: keep the latest
// blob for the UI and push it over the WS.
inline void on_telemetry(const IdentityId& iden, const RNS::Bytes& peer,
                         const RNS::Bytes& blob) {
  Feed* f = _detail::find_feed(iden, peer);
  if (f == nullptr) return;
  if (blob.size() > MAX_TELEMETRY_BLOB) return;
  f->latest    = blob;
  f->latest_ms = millis();
  f->awaiting  = false;
  f->misses    = 0;
  Web::WS::publish_telemetry_update(iden, peer, blob);
}

// Main-loop tick: answer queued telemetry requests (rate-limited per
// peer) and run the feed request loop. Sends take the RNS lock here
// like any web handler - the loop calls us outside its guarded
// section.
inline void tick() {
  // While any peer holds an active live-share grant to our telemetry,
  // keep the shared I2C sensors polling fast (the same request_live
  // demand the screens and the web popover use) so each grant answer
  // packs a fresh reading. Location rides the GPS power schedule and
  // battery is always cheap to read, so only environment and compass
  // need the demand.
  {
    uint8_t live_items = 0;
    for (const auto& g : _detail::grants()) {
      if (_detail::now_epoch(g.iden) <= g.expires_epoch) live_items |= g.items;
    }
    if (live_items & ITEM_ENVIRONMENT) Sensors::BME280::request_live();
    if (live_items & ITEM_COMPASS)     Sensors::QMC6310::request_live();
  }

  // --- Grant answers ---------------------------------------------
  auto& q = _detail::pending();
  if (!q.empty()) {
    const uint32_t now = millis();
    auto it = q.begin();
    while (it != q.end()) {
      Grant* g = _detail::find_grant(it->iden, it->peer);
      if (g == nullptr || _detail::now_epoch(it->iden) > g->expires_epoch) {
        it = q.erase(it);
        continue;
      }
      if (g->last_answer_ms != 0 && (now - g->last_answer_ms) < MIN_ANSWER_GAP_MS) {
        ++it;
        continue;
      }
      ExtraFields extra;
      extra.telemetry = pack_items(it->iden, g->items);
      g->last_answer_ms = now;
      const IdentityId iden = it->iden;
      const RNS::Bytes peer = it->peer;
      it = q.erase(it);
      if (extra.telemetry.size() == 0) continue;
      MessageRecord rec;
      const char* err = nullptr;
      bool queued = false;
      Common::RnsLock::Guard rns_guard;
      LXMFGateway::send(iden, peer, "", "", nullptr, rec, &err, &queued,
                        /*use_seq=*/0, &extra);
    }
  }

  // --- Feed request loop ------------------------------------------
  auto& v = _detail::feeds();
  if (v.empty()) return;
  const uint32_t now = millis();
  for (auto it = v.begin(); it != v.end(); ) {
    if (_detail::now_epoch(it->iden) > it->expires_epoch) {
      it = v.erase(it);   // window over
      continue;
    }
    if (now - it->last_req_ms < it->every_s * 1000UL) {
      ++it;
      continue;
    }
    // The previous request is still unanswered - count the miss, and
    // stop polling a sender that has stopped answering (revoked, or
    // out of range).
    if (it->awaiting) {
      it->misses += 1;
      if (it->misses >= MAX_FEED_MISSES) {
        NOTICEF("TelemetryShare: feed from %s dropped after %u unanswered requests",
                it->peer.toHex().c_str(), (unsigned)it->misses);
        it = v.erase(it);
        continue;
      }
    }
    ExtraFields extra;
    extra.commands = pack_telemetry_request(_detail::now_epoch(it->iden));
    it->last_req_ms = now;
    it->awaiting    = true;
    if (extra.commands.size() > 0) {
      MessageRecord rec;
      const char* err = nullptr;
      bool queued = false;
      Common::RnsLock::Guard rns_guard;
      LXMFGateway::send(it->iden, it->peer, "", "", nullptr, rec, &err, &queued,
                        /*use_seq=*/0, &extra);
    }
    ++it;
  }
}

}  // namespace TelemetryShare
}  // namespace LXMF
