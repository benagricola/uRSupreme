#pragma once

// LXMFMinimal — embedded LXMF parser/sender for microReticulum.
//
// Refactored from PR #17 on attermann/microReticulum_Firmware
// (varna9000's LXMF_Minimal.h, 641 lines). Key changes from the original:
//
//   - Multi-instance: dispatch via static std::map<destination_hash, LXMFMinimal*>
//     instead of a single static _instance. Each identity hosts its own
//     LXMFMinimal and they share the underlying static callback trampoline.
//   - Announce is explicit (not auto-fired in init), so the gateway can
//     schedule announces centrally.
//   - send_message(dest_hash, title, content) replaces send_reply()'s
//     hard-coded empty-title shape.
//   - Incoming signature verification: each incoming packet's Ed25519
//     signature is verified against the sender's recalled identity.
//     Result reported via MessageRecord::signature_ok rather than silently
//     skipped.
//   - Delivery callback is std::function<void(const MessageRecord&)> —
//     a notification, not a reply generator (PR #17 returned a string from
//     the handler to auto-reply, which makes sense for GPIO control but
//     not for a generic messaging gateway).
//   - All Serial.print(..) trace lines are routed through the RNS log
//     facility so output is consistent with the rest of the firmware.

#include <Arduino.h>
#include <functional>
#include <map>
#include <string>

#include <Reticulum.h>
#include <Identity.h>
#include <Destination.h>
#include <Packet.h>
#include <Link.h>
#include <Resource.h>
#include <Transport.h>
#include <Bytes.h>
#include <Log.h>

#include "LXMFTypes.h"

namespace LXMF {

  // Raw MsgPack helpers (PR #17). Kept verbatim — no library dependency.
  namespace RawMsgPack {

    inline std::string read_bin_or_str(const uint8_t* data, size_t len, size_t& offset) {
      if (offset >= len) return "";
      uint8_t tag = data[offset++];
      size_t slen = 0;
      if ((tag & 0xE0) == 0xA0) {
        slen = tag & 0x1F;
      } else if (tag == 0xD9 && offset < len) {
        slen = data[offset++];
      } else if (tag == 0xDA && offset + 1 < len) {
        slen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
      } else if (tag == 0xDB && offset + 3 < len) {
        slen = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset+1] << 16) |
               ((uint32_t)data[offset+2] << 8) | data[offset+3];
        offset += 4;
      } else if (tag == 0xC4 && offset < len) {
        slen = data[offset++];
      } else if (tag == 0xC5 && offset + 1 < len) {
        slen = (data[offset] << 8) | data[offset + 1];
        offset += 2;
      } else if (tag == 0xC6 && offset + 3 < len) {
        slen = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset+1] << 16) |
               ((uint32_t)data[offset+2] << 8) | data[offset+3];
        offset += 4;
      } else {
        return "";
      }
      if (offset + slen > len) return "";
      std::string result((const char*)&data[offset], slen);
      offset += slen;
      return result;
    }

    inline bool skip_element(const uint8_t* data, size_t len, size_t& offset) {
      if (offset >= len) return false;
      uint8_t tag = data[offset++];
      if (tag == 0xC0 || tag == 0xC2 || tag == 0xC3) return true;
      if (tag <= 0x7F) return true;
      if (tag >= 0xE0) return true;
      if ((tag & 0xE0) == 0xA0) { size_t n = tag & 0x1F; offset += n; return offset <= len; }
      if ((tag & 0xF0) == 0x90) {
        size_t n = tag & 0x0F;
        for (size_t i = 0; i < n; i++) if (!skip_element(data, len, offset)) return false;
        return true;
      }
      if ((tag & 0xF0) == 0x80) {
        size_t n = tag & 0x0F;
        for (size_t i = 0; i < n * 2; i++) if (!skip_element(data, len, offset)) return false;
        return true;
      }
      if (tag == 0xCC || tag == 0xD0) { offset += 1; return offset <= len; }
      if (tag == 0xCD || tag == 0xD1) { offset += 2; return offset <= len; }
      if (tag == 0xCE || tag == 0xD2 || tag == 0xCA) { offset += 4; return offset <= len; }
      if (tag == 0xCF || tag == 0xD3 || tag == 0xCB) { offset += 8; return offset <= len; }
      if (tag == 0xD9 && offset < len) { size_t n = data[offset++]; offset += n; return offset <= len; }
      if (tag == 0xDA && offset + 1 < len) {
        size_t n = (data[offset] << 8) | data[offset+1]; offset += 2 + n; return offset <= len;
      }
      if (tag == 0xDB && offset + 3 < len) {
        size_t n = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset+1] << 16) |
                   ((uint32_t)data[offset+2] << 8) | data[offset+3];
        offset += 4 + n; return offset <= len;
      }
      if (tag == 0xC4 && offset < len) { size_t n = data[offset++]; offset += n; return offset <= len; }
      if (tag == 0xC5 && offset + 1 < len) {
        size_t n = (data[offset] << 8) | data[offset+1]; offset += 2 + n; return offset <= len;
      }
      if (tag == 0xC6 && offset + 3 < len) {
        size_t n = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset+1] << 16) |
                   ((uint32_t)data[offset+2] << 8) | data[offset+3];
        offset += 4 + n; return offset <= len;
      }
      if (tag == 0xDC && offset + 1 < len) {
        size_t n = (data[offset] << 8) | data[offset+1]; offset += 2;
        for (size_t i = 0; i < n; i++) if (!skip_element(data, len, offset)) return false;
        return true;
      }
      if (tag == 0xDE && offset + 1 < len) {
        size_t n = (data[offset] << 8) | data[offset+1]; offset += 2;
        for (size_t i = 0; i < n * 2; i++) if (!skip_element(data, len, offset)) return false;
        return true;
      }
      if (tag == 0xD4) { offset += 2; return offset <= len; }
      if (tag == 0xD5) { offset += 3; return offset <= len; }
      if (tag == 0xD6) { offset += 5; return offset <= len; }
      if (tag == 0xD7) { offset += 9; return offset <= len; }
      if (tag == 0xD8) { offset += 17; return offset <= len; }
      return false;
    }

    inline size_t pack_bin(uint8_t* buf, size_t buflen, const uint8_t* data, size_t dlen) {
      size_t pos = 0;
      if (dlen <= 255) {
        if (pos + 2 + dlen > buflen) return 0;
        buf[pos++] = 0xC4;
        buf[pos++] = (uint8_t)dlen;
      } else {
        if (pos + 3 + dlen > buflen) return 0;
        buf[pos++] = 0xC5;
        buf[pos++] = (dlen >> 8) & 0xFF;
        buf[pos++] = dlen & 0xFF;
      }
      memcpy(&buf[pos], data, dlen);
      pos += dlen;
      return pos;
    }

    inline size_t pack_bin_str(uint8_t* buf, size_t buflen, const std::string& str) {
      return pack_bin(buf, buflen, (const uint8_t*)str.c_str(), str.length());
    }

    inline size_t pack_float64(uint8_t* buf, size_t buflen, double val) {
      if (buflen < 9) return 0;
      buf[0] = 0xCB;
      uint64_t bits;
      memcpy(&bits, &val, 8);
      for (int i = 7; i >= 0; i--) {
        buf[8 - i] = (bits >> (i * 8)) & 0xFF;
      }
      return 9;
    }

  } // namespace RawMsgPack


  // LXMF wire-size thresholds.
  //
  // OPPORTUNISTIC_MAX = LXMF packet sent to a SINGLE destination, fits in
  // one encrypted radio packet. Empirically anything <= 295 bytes is safe
  // on our profile (Packet::ENCRYPTED_MDU ~ 399 minus header/IV margin).
  //
  // LINK_PACKET_MAX_CONTENT = LXMF packet sent inside an established Link
  // as a single DATA packet. Link::MDU is 431; LXMF_OVERHEAD is 80
  // (HASH_LEN + SIG_LEN), giving an effective 319-byte payload max with
  // some safety margin.
  //
  // Anything larger goes through a Resource transfer over the same Link.
  static constexpr size_t LXMF_OPPORTUNISTIC_MAX   = 295;
  static constexpr size_t LXMF_LINK_PACKET_MAX     = 319;

  class LXMFMinimal {
  public:
    using DeliveryCallback = std::function<void(const MessageRecord&)>;

    // Async state for an outbound DIRECT-mode send. We open a Link to the
    // peer in send_message and queue the wire bytes here keyed by the
    // link's hash; when the Link establishes, the static callback pops
    // the entry and dispatches the actual payload as either an in-link
    // Packet or a Resource. On completion (PROOF / Resource COMPLETE /
    // link closure / timeout) the entry is removed and the gateway is
    // notified via the delivery callback so the MessageRecord status
    // can advance.
    struct PendingLinkSend {
      RNS::Bytes   wire;
      RNS::Link    link;
      uint64_t     started_ms = 0;
      RNS::Bytes   dest_hash;
      RNS::Bytes   resource_hash;     // set after Resource is built (if >319 B)
      LXMFMinimal* owner = nullptr;
      MessageRecord rec;
    };

    LXMFMinimal()
      : _identity(RNS::Type::NONE),
        _destination(RNS::Type::NONE),
        _initialized(false),
        _time_offset(0),
        _time_calibrated(false),
        _display_name("LXMF Node"),
        _on_delivery(nullptr) {}

    // Register this instance under the given identity. Does NOT announce —
    // call announce() explicitly when ready.
    bool init(RNS::Identity& identity, const char* display_name = "LXMF Node") {
      _identity = identity;
      _display_name = display_name;

      _destination = RNS::Destination(
        _identity,
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "lxmf", "delivery"
      );

      // Register this instance for dispatch by destination hash. The static
      // trampoline below looks us up on every incoming packet.
      registry()[_destination.hash()] = this;
      _destination.set_packet_callback(_static_packet_callback);

      // Accept inbound links (for DIRECT-mode LXMF deliveries) and route
      // them to the static trampoline that will register per-link callbacks.
      _destination.accepts_links(true);
      _destination.set_link_established_callback(_static_inbound_link_established);

      _initialized = true;
      NOTICEF("LXMF: identity %s delivery destination ready (%s)",
              display_name, _destination.hash().toHex().c_str());
      return true;
    }

    // Tear down the dispatch registration. Call from destructor / identity delete.
    void shutdown() {
      if (!_initialized) return;
      registry().erase(_destination.hash());
      _initialized = false;
    }

    ~LXMFMinimal() { shutdown(); }

    void set_delivery_callback(DeliveryCallback cb) { _on_delivery = std::move(cb); }

    // LXMF address of this identity (16-byte destination hash, hex-encoded).
    std::string address_hex() const {
      if (!_initialized) return "";
      return _destination.hash().toHex();
    }

    const RNS::Bytes& address() const { return _destination.hash(); }

    // Manually emit the LXMF announce packet for this destination.
    void announce() {
      if (!_initialized) return;
      uint8_t buf[128];
      size_t pos = 0;
      buf[pos++] = 0x92;  // fixarray(2)
      size_t n = RawMsgPack::pack_bin(&buf[pos], sizeof(buf) - pos,
                                      (const uint8_t*)_display_name.c_str(),
                                      _display_name.length());
      if (n == 0) { ERROR("LXMF: announce pack failed"); return; }
      pos += n;
      buf[pos++] = 0xC0;  // stamp_cost: nil
      RNS::Bytes app_data(buf, pos);
      _destination.announce(app_data);
      NOTICEF("LXMF: announced %s", _destination.hash().toHex().c_str());
    }

    // Send an LXMF message to a remote destination hash. Returns false if
    // the recipient identity isn't yet known (no announce seen) or pack/sign
    // failed. Status reporting on delivery is left to the caller via the
    // returned MessageRecord — Phase 1 just sets status=Sent after send().
    bool send_message(const RNS::Bytes& dest_hash,
                      const std::string& title,
                      const std::string& content,
                      MessageRecord& out_rec,
                      const char** out_err = nullptr) {
      auto fail = [&](const char* msg) { if (out_err) *out_err = msg; return false; };
      if (!_initialized) return fail("LXMF gateway not initialised");
      if (dest_hash.size() != HASH_LEN) {
        ERROR("LXMF: send_message: destination hash must be 16 bytes");
        return fail("Destination address is the wrong length (expected 16 bytes).");
      }

      RNS::Identity remote_identity = RNS::Identity::recall(dest_hash);
      if (!remote_identity) {
        WARNINGF("LXMF: cannot send to %s — recipient identity unknown",
                 dest_hash.toHex().c_str());
        return fail("Recipient is unknown — wait for an announce from that address, or trigger one on the recipient. (We have no public key for that destination yet.)");
      }

      RNS::Destination remote_dest(
        remote_identity,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf", "delivery"
      );

      // Build msgpack payload: [timestamp:f64, title:bin, content:bin, fields:nil]
      uint8_t mp[512];
      size_t mp_pos = 0;
      mp[mp_pos++] = 0x94;  // fixarray(4)

      double ts = get_timestamp();
      size_t n = RawMsgPack::pack_float64(&mp[mp_pos], sizeof(mp) - mp_pos, ts);
      if (n == 0) { ERROR("LXMF: send: timestamp pack failed"); return fail("Internal error packing timestamp."); }
      mp_pos += n;

      n = RawMsgPack::pack_bin_str(&mp[mp_pos], sizeof(mp) - mp_pos, title);
      if (n == 0) { ERROR("LXMF: send: title pack failed"); return fail("Title is too long for the LXMF packet."); }
      mp_pos += n;

      n = RawMsgPack::pack_bin_str(&mp[mp_pos], sizeof(mp) - mp_pos, content);
      if (n == 0) { ERROR("LXMF: send: content pack failed"); return fail("Message body is too long to fit in a single LXMF packet (opportunistic mode caps around 200 chars)."); }
      mp_pos += n;

      mp[mp_pos++] = 0xC0;  // fields: nil

      // Build signed_part = dest_hash || src_hash || payload || sha256(...)
      const RNS::Bytes& src_hash = _destination.hash();
      RNS::Bytes hashed_part;
      hashed_part.append(dest_hash.data(), HASH_LEN);
      hashed_part.append(src_hash.data(), HASH_LEN);
      hashed_part.append(mp, mp_pos);
      RNS::Bytes message_hash = RNS::Identity::full_hash(hashed_part);
      RNS::Bytes signed_part;
      signed_part.append(hashed_part);
      signed_part.append(message_hash);

      RNS::Bytes signature;
      try {
        signature = _identity.sign(signed_part);
      } catch (const std::exception& e) {
        ERRORF("LXMF: send: signing failed: %s", e.what());
        return fail("Signing failed — identity may be misconfigured.");
      }
      if (signature.size() != SIG_LEN) {
        ERRORF("LXMF: send: unexpected signature length %u", (unsigned)signature.size());
        return fail("Internal error: signature was not the expected length.");
      }

      // Build the on-wire LXMF blob: src_hash || sig || payload
      size_t total = HASH_LEN + SIG_LEN + mp_pos;
      if (total > 1 * 1024 * 1024) {
        return fail("Message body is too large (> 1 MiB).");
      }
      RNS::Bytes wire;
      wire.append(src_hash.data(), HASH_LEN);
      wire.append(signature.data(), SIG_LEN);
      wire.append(mp, mp_pos);

      out_rec.ts           = ts;
      out_rec.peer_hash    = dest_hash;
      out_rec.title        = title;
      out_rec.content      = content;
      out_rec.incoming     = false;
      out_rec.signature_ok = true;

      // OPPORTUNISTIC: single packet to the SINGLE destination. Fast path
      // for short messages; status -> Sent immediately. This is what works
      // today and we keep the existing behaviour for compatibility.
      if (total <= LXMF_OPPORTUNISTIC_MAX) {
        RNS::Packet packet(remote_dest, wire);
        packet.send();
        out_rec.status      = OutboxStatus::Sent;
        out_rec.packet_hash = packet.get_hash();
        NOTICEF("LXMF: sent OPPORTUNISTIC to %s (%u bytes)",
                dest_hash.toHex().c_str(), (unsigned)total);
        return true;
      }

      // DIRECT-mode: open a Link to the peer; on link_established the
      // static callback will pop our queued send and dispatch as either
      // an in-link PACKET (<= 319 B) or a RESOURCE (> 319 B), then teardown.
      // The MessageRecord transitions Queued -> Sent (after PROOF/COMPLETE)
      // -> Delivered via the gateway's status update path.
      RNS::Link link(remote_dest,
                     _static_outbound_link_established,
                     _static_outbound_link_closed);
      RNS::Bytes link_hash = link.hash();
      PendingLinkSend ps;
      ps.wire        = wire;
      ps.link        = link;
      ps.started_ms  = (uint64_t)millis();
      ps.dest_hash   = dest_hash;
      ps.owner       = this;
      ps.rec         = out_rec;
      pending_link_sends()[link_hash] = std::move(ps);
      out_rec.status      = OutboxStatus::Queued;
      out_rec.packet_hash = link_hash;  // placeholder; real packet hash once sent
      NOTICEF("LXMF: opening Link to %s for %u-byte DIRECT send",
              dest_hash.toHex().c_str(), (unsigned)total);
      return true;
    }

    // LXMF timestamps are seconds-since-epoch as float64. The device has
    // no RTC in LoRa-only mode, so we use compile-time epoch + millis()
    // until we receive a calibration timestamp from a peer.
    double get_timestamp() {
      double up = (double)millis() / 1000.0;
      if (_time_calibrated) return _time_offset + up;
      return _compile_time_epoch() + up;
    }

    void calibrate_time(double remote_ts) {
      if (remote_ts > 1704067200.0 && !_time_calibrated) {
        double up = (double)millis() / 1000.0;
        _time_offset = remote_ts - up;
        _time_calibrated = true;
        NOTICEF("LXMF: time calibrated from peer, epoch offset %ld",
                (long)_time_offset);
      }
    }

  private:
    static std::map<RNS::Bytes, LXMFMinimal*>& registry() {
      static std::map<RNS::Bytes, LXMFMinimal*> r;
      return r;
    }

    // Outbound link state, keyed by link.hash(). Each pending send sits
    // here from the time send_message kicks off the link establishment
    // until the resource (or in-link packet) concludes / link closes.
    static std::map<RNS::Bytes, PendingLinkSend>& pending_link_sends() {
      static std::map<RNS::Bytes, PendingLinkSend> p;
      return p;
    }

    // Static trampoline: looks up the LXMFMinimal* by the packet's
    // destination hash and dispatches to that instance.
    static void _static_packet_callback(const RNS::Bytes& data, const RNS::Packet& packet) {
      auto& reg = registry();
      auto it = reg.find(packet.destination_hash());
      if (it == reg.end() || it->second == nullptr) {
        WARNINGF("LXMF: incoming packet for unknown destination %s",
                 packet.destination_hash().toHex().c_str());
        return;
      }
      it->second->_on_packet(data, packet);
    }

    // ------------------------------------------------------------------
    // DIRECT-mode (Link + optional Resource) wiring (plan step 10)
    //
    // OUTBOUND flow:
    //  send_message > 295 B
    //    -> open Link(dest, _static_outbound_link_established, ...)
    //    -> store PendingLinkSend keyed by link.hash()
    //    -> link establishes asynchronously
    //  _static_outbound_link_established(link)
    //    -> if wire <= 319: send as in-link Packet, status -> Sent,
    //       wait for Link teardown
    //    -> else: construct Resource(wire, link,
    //                                _static_outbound_resource_concluded)
    //       and record resource_hash in the pending entry
    //
    // INBOUND flow:
    //  peer opens Link to our delivery destination
    //    -> _static_inbound_link_established(link)
    //    -> link.set_resource_strategy(ACCEPT_ALL)
    //    -> link.set_packet_callback(_static_inbound_link_packet)
    //    -> link.set_resource_concluded_callback(_static_inbound_resource_concluded)
    //  peer sends short LXMF wire: in-link Packet path
    //    -> _static_inbound_link_packet  -> _deliver_lxmf_payload
    //  peer sends long LXMF wire: Resource path
    //    -> _static_inbound_resource_concluded -> _deliver_lxmf_payload
    // ------------------------------------------------------------------

    static void _static_outbound_link_established(RNS::Link& link) {
      auto& m = pending_link_sends();
      auto it = m.find(link.hash());
      if (it == m.end()) {
        WARNINGF("LXMF: outbound link established but no pending send for %s",
                 link.hash().toHex().c_str());
        return;
      }
      PendingLinkSend& ps = it->second;
      NOTICEF("LXMF: outbound link established to %s (%u-byte payload)",
              ps.dest_hash.toHex().c_str(), (unsigned)ps.wire.size());

      if (ps.wire.size() <= LXMF_LINK_PACKET_MAX) {
        // Single in-link DATA packet path.
        try {
          RNS::Packet pkt(link, ps.wire);
          pkt.send();
          ps.rec.status = OutboxStatus::Sent;
          ps.rec.packet_hash = pkt.get_hash();
        }
        catch (const std::exception& e) {
          ERRORF("LXMF: in-link packet send failed: %s", e.what());
          ps.rec.status = OutboxStatus::Failed;
        }
        if (ps.owner && ps.owner->_on_delivery) {
          try { ps.owner->_on_delivery(ps.rec); } catch (...) {}
        }
        link.teardown();
        m.erase(it);
      }
      else {
        // Resource transfer.
        RNS::Resource res(ps.wire, link,
                          /*advertise=*/true, /*auto_compress=*/false,
                          _static_outbound_resource_concluded,
                          /*progress=*/nullptr,
                          /*timeout=*/0.0);
        ps.resource_hash = res.hash();
        DEBUGF("LXMF: outbound resource advertised %s",
               ps.resource_hash.toHex().c_str());
      }
    }

    static void _static_outbound_link_closed(RNS::Link& link) {
      auto& m = pending_link_sends();
      auto it = m.find(link.hash());
      if (it == m.end()) return;
      PendingLinkSend& ps = it->second;
      // If the send already concluded successfully it'll have been
      // erased; getting here means an abnormal close.
      if (ps.rec.status != OutboxStatus::Sent &&
          ps.rec.status != OutboxStatus::Delivered) {
        ps.rec.status = OutboxStatus::Failed;
        WARNINGF("LXMF: link closed before send completed (peer %s)",
                 ps.dest_hash.toHex().c_str());
        if (ps.owner && ps.owner->_on_delivery) {
          try { ps.owner->_on_delivery(ps.rec); } catch (...) {}
        }
      }
      m.erase(it);
    }

    static void _static_outbound_resource_concluded(const RNS::Resource& res) {
      auto& m = pending_link_sends();
      // Find the pending send whose resource_hash matches.
      for (auto it = m.begin(); it != m.end(); ) {
        if (it->second.resource_hash == res.hash()) {
          PendingLinkSend& ps = it->second;
          const auto st = res.status();
          if (st == RNS::Type::Resource::COMPLETE) {
            ps.rec.status = OutboxStatus::Delivered;
            NOTICEF("LXMF: outbound resource COMPLETE for %s",
                    ps.dest_hash.toHex().c_str());
          }
          else {
            ps.rec.status = OutboxStatus::Failed;
            WARNINGF("LXMF: outbound resource FAILED for %s (status=%d)",
                     ps.dest_hash.toHex().c_str(), (int)st);
          }
          if (ps.owner && ps.owner->_on_delivery) {
            try { ps.owner->_on_delivery(ps.rec); } catch (...) {}
          }
          // Teardown the link; erase the entry.
          const_cast<RNS::Link&>(res.link()).teardown();
          it = m.erase(it);
        }
        else {
          ++it;
        }
      }
    }

    static void _static_inbound_link_established(RNS::Link& link) {
      // The destination this link landed on tells us which LXMFMinimal
      // instance owns it. (Multi-identity firmware can host several.)
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) {
        WARNINGF("LXMF: inbound link to unknown destination %s",
                 our_dest_hash.toHex().c_str());
        return;
      }
      NOTICEF("LXMF: inbound link established on %s",
              our_dest_hash.toHex().c_str());

      // Accept all Resources on this link; route in-link packets +
      // resource conclusions to the static trampolines that look up
      // the owning LXMFMinimal by the link's destination.
      link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
      link.set_packet_callback(_static_inbound_link_packet);
      link.set_resource_concluded_callback(_static_inbound_resource_concluded);
    }

    static void _static_inbound_link_packet(const RNS::Bytes& plaintext,
                                            const RNS::Packet& packet) {
      // Find the owning LXMFMinimal via the link's destination hash.
      const RNS::Link& link = packet.destination_link();
      if (!link) return;
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) return;
      NOTICEF("LXMF: inbound in-link PACKET on %s (%u bytes)",
              our_dest_hash.toHex().c_str(), (unsigned)plaintext.size());
      it->second->_deliver_lxmf_payload(plaintext, packet.get_hash());
    }

    static void _static_inbound_resource_concluded(const RNS::Resource& res) {
      if (res.status() != RNS::Type::Resource::COMPLETE) {
        WARNINGF("LXMF: inbound resource FAILED/CORRUPT (status=%d)",
                 (int)res.status());
        return;
      }
      const RNS::Link& link = res.link();
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) return;
      const RNS::Bytes& plaintext = res.plaintext();
      NOTICEF("LXMF: inbound RESOURCE COMPLETE on %s (%u bytes)",
              our_dest_hash.toHex().c_str(), (unsigned)plaintext.size());
      it->second->_deliver_lxmf_payload(plaintext, res.hash());
    }

    // Common parse + signature-verify + delivery path used by all three
    // inbound modes (OPPORTUNISTIC Packet, in-link Packet, Resource).
    void _deliver_lxmf_payload(const RNS::Bytes& wire,
                               const RNS::Bytes& packet_or_resource_hash) {
      if (wire.size() < HEADER_LEN + 5) {
        WARNING("LXMF: incoming payload too short for LXMF header");
        return;
      }
      const uint8_t* raw = wire.data();
      size_t raw_len = wire.size();

      RNS::Bytes source_hash(raw, HASH_LEN);
      RNS::Bytes signature(raw + HASH_LEN, SIG_LEN);
      const uint8_t* payload = raw + HEADER_LEN;
      size_t payload_len = raw_len - HEADER_LEN;

      double msg_ts = 0;
      std::string title;
      std::string content;
      if (!_parse_lxmf_payload(payload, payload_len, &msg_ts, &title, &content)) {
        WARNING("LXMF: malformed payload");
        return;
      }
      if (msg_ts > 0) calibrate_time(msg_ts);

      bool sig_ok = false;
      RNS::Identity sender = RNS::Identity::recall(source_hash);
      if (sender) {
        RNS::Bytes hashed_part;
        hashed_part.append(_destination.hash().data(), HASH_LEN);
        hashed_part.append(source_hash.data(), HASH_LEN);
        hashed_part.append(payload, payload_len);
        RNS::Bytes mh = RNS::Identity::full_hash(hashed_part);
        RNS::Bytes signed_part;
        signed_part.append(hashed_part);
        signed_part.append(mh);
        sig_ok = sender.validate(signature, signed_part);
      }
      else {
        WARNINGF("LXMF: sender %s identity not known — signature cannot be verified",
                 source_hash.toHex().c_str());
      }

      if (_on_delivery) {
        MessageRecord rec;
        rec.ts           = msg_ts;
        rec.peer_hash    = source_hash;
        rec.title        = title;
        rec.content      = content;
        rec.incoming     = true;
        rec.signature_ok = sig_ok;
        rec.status       = OutboxStatus::Delivered;
        rec.packet_hash  = packet_or_resource_hash;
        try { _on_delivery(rec); }
        catch (const std::bad_alloc&) {
          ERROR("LXMF: delivery callback bad_alloc");
        }
        catch (const std::exception& e) {
          ERRORF("LXMF: delivery callback exception: %s", e.what());
        }
      }
    }

    void _on_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
      // Acknowledge delivery to the sender (Reticulum proof).
      const_cast<RNS::Packet&>(packet).prove();

      if (data.size() < HEADER_LEN + 5) {
        WARNING("LXMF: incoming packet too short for LXMF header");
        return;
      }
      const uint8_t* raw = data.data();
      size_t raw_len = data.size();

      RNS::Bytes source_hash(raw, HASH_LEN);
      RNS::Bytes signature(raw + HASH_LEN, SIG_LEN);
      const uint8_t* payload = raw + HEADER_LEN;
      size_t payload_len = raw_len - HEADER_LEN;

      double msg_ts = 0;
      std::string title;
      std::string content;
      if (!_parse_lxmf_payload(payload, payload_len, &msg_ts, &title, &content)) {
        WARNING("LXMF: malformed payload");
        return;
      }
      if (msg_ts > 0) calibrate_time(msg_ts);

      // Reconstruct the signed payload and verify the Ed25519 signature.
      bool sig_ok = false;
      RNS::Identity sender = RNS::Identity::recall(source_hash);
      if (sender) {
        RNS::Bytes hashed_part;
        hashed_part.append(_destination.hash().data(), HASH_LEN);
        hashed_part.append(source_hash.data(), HASH_LEN);
        hashed_part.append(payload, payload_len);
        RNS::Bytes mh = RNS::Identity::full_hash(hashed_part);
        RNS::Bytes signed_part;
        signed_part.append(hashed_part);
        signed_part.append(mh);
        sig_ok = sender.validate(signature, signed_part);
      } else {
        WARNINGF("LXMF: sender %s identity not known — signature cannot be verified",
                 source_hash.toHex().c_str());
      }

      NOTICEF("LXMF: incoming from %s (%u bytes, sig %s)",
              source_hash.toHex().c_str(), (unsigned)data.size(),
              sig_ok ? "ok" : "BAD");

      if (_on_delivery) {
        MessageRecord rec;
        rec.ts           = msg_ts;
        rec.peer_hash    = source_hash;
        rec.title        = title;
        rec.content      = content;
        rec.incoming     = true;
        rec.signature_ok = sig_ok;
        rec.status       = OutboxStatus::Delivered;
        rec.packet_hash  = packet.get_hash();
        try {
          _on_delivery(rec);
        } catch (const std::bad_alloc&) {
          ERROR("LXMF: delivery callback bad_alloc");
        } catch (const std::exception& e) {
          ERRORF("LXMF: delivery callback exception: %s", e.what());
        }
      }
    }

    bool _parse_lxmf_payload(const uint8_t* data, size_t len,
                             double* out_ts, std::string* out_title, std::string* out_content) {
      if (len < 2) return false;
      size_t off = 0;
      uint8_t tag = data[off++];
      size_t arr_len = 0;
      if ((tag & 0xF0) == 0x90) {
        arr_len = tag & 0x0F;
      } else if (tag == 0xDC && off + 1 < len) {
        arr_len = (data[off] << 8) | data[off + 1];
        off += 2;
      } else {
        return false;
      }
      if (arr_len < 3) return false;

      // Element 0: timestamp (float64)
      if (out_ts && off + 9 <= len && data[off] == 0xCB) {
        union { double d; uint8_t b[8]; } u;
        for (int i = 0; i < 8; i++) u.b[7 - i] = data[off + 1 + i];
        *out_ts = u.d;
      }
      if (!RawMsgPack::skip_element(data, len, off)) return false;

      // Element 1: title
      std::string title = RawMsgPack::read_bin_or_str(data, len, off);
      if (out_title) *out_title = title;

      // Element 2: content
      std::string content = RawMsgPack::read_bin_or_str(data, len, off);
      if (content.empty()) return false;
      if (out_content) *out_content = content;
      return true;
    }

    static double _compile_time_epoch() {
      // 2025-01-01 00:00:00 UTC = 1735689600. Conservative baseline until
      // calibrated by a peer.
      return 1735689600.0;
    }

  private:
    RNS::Identity     _identity;
    RNS::Destination  _destination;
    bool              _initialized;
    double            _time_offset;
    bool              _time_calibrated;
    std::string       _display_name;
    DeliveryCallback  _on_delivery;
  };

} // namespace LXMF
