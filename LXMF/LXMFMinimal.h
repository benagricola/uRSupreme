#pragma once

// LXMFMinimal — embedded LXMF parser/sender for microReticulum.
//
// Refactored from PR #17 on attermann/microReticulum_Firmware
// (varna9000's LXMF_Minimal.h, 641 lines). Key changes from the original:
//
//   - Multi-instance: dispatch via static std::map<destination_hash, LXMFMinimal*>
//     instead of a single static _instance. Each account hosts its own
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


  class LXMFMinimal {
  public:
    using DeliveryCallback = std::function<void(const MessageRecord&)>;

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

      _initialized = true;
      NOTICEF("LXMF: account %s delivery destination ready (%s)",
              display_name, _destination.hash().toHex().c_str());
      return true;
    }

    // Tear down the dispatch registration. Call from destructor / account delete.
    void shutdown() {
      if (!_initialized) return;
      registry().erase(_destination.hash());
      _initialized = false;
    }

    ~LXMFMinimal() { shutdown(); }

    void set_delivery_callback(DeliveryCallback cb) { _on_delivery = std::move(cb); }

    // LXMF address of this account (16-byte destination hash, hex-encoded).
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
                      MessageRecord& out_rec) {
      if (!_initialized) return false;
      if (dest_hash.size() != HASH_LEN) {
        ERROR("LXMF: send_message: destination hash must be 16 bytes");
        return false;
      }

      RNS::Identity remote_identity = RNS::Identity::recall(dest_hash);
      if (!remote_identity) {
        WARNINGF("LXMF: cannot send to %s — recipient identity unknown",
                 dest_hash.toHex().c_str());
        return false;
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
      if (n == 0) { ERROR("LXMF: send: timestamp pack failed"); return false; }
      mp_pos += n;

      n = RawMsgPack::pack_bin_str(&mp[mp_pos], sizeof(mp) - mp_pos, title);
      if (n == 0) { ERROR("LXMF: send: title pack failed"); return false; }
      mp_pos += n;

      n = RawMsgPack::pack_bin_str(&mp[mp_pos], sizeof(mp) - mp_pos, content);
      if (n == 0) { ERROR("LXMF: send: content pack failed"); return false; }
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
        return false;
      }
      if (signature.size() != SIG_LEN) {
        ERRORF("LXMF: send: unexpected signature length %u", (unsigned)signature.size());
        return false;
      }

      // Build the on-wire LXMF blob: src_hash || sig || payload
      size_t total = HASH_LEN + SIG_LEN + mp_pos;
      if (total > 400) {
        WARNING("LXMF: send: payload exceeds opportunistic packet size");
        return false;
      }
      uint8_t wire[400];
      size_t wp = 0;
      memcpy(&wire[wp], src_hash.data(), HASH_LEN); wp += HASH_LEN;
      memcpy(&wire[wp], signature.data(), SIG_LEN); wp += SIG_LEN;
      memcpy(&wire[wp], mp, mp_pos); wp += mp_pos;

      RNS::Bytes packet_data(wire, wp);
      RNS::Packet packet(remote_dest, packet_data);
      packet.send();

      out_rec.ts           = ts;
      out_rec.peer_hash    = dest_hash;
      out_rec.title        = title;
      out_rec.content      = content;
      out_rec.incoming     = false;
      out_rec.signature_ok = true;
      out_rec.status       = OutboxStatus::Sent;
      out_rec.packet_hash  = packet.get_hash();

      NOTICEF("LXMF: sent message to %s (%u bytes)",
              dest_hash.toHex().c_str(), (unsigned)wp);
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
