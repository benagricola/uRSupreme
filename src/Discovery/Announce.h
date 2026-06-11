// Discovery announce wire-format encoder.
//
// Builds the `app_data` payload that goes on an
// `rnstransport.discovery.interface` aspect announce - the upstream
// Reticulum InterfaceAnnouncer.get_interface_announce_data() in
// reticulum/RNS/Discovery.py is the reference implementation; the
// tag values + ordering + framing here match it byte-for-byte (so
// any standard listener - a downstream RNS listener, or another RNS node
// running InterfaceAnnounceHandler - can parse what we emit).
//
// Frame shape:
//
//   [flags : 1 byte]
//   [packed-msgpack-dict : variable]
//   [stamp : 32 bytes]
//
//   - flags bit 0 = FLAG_SIGNED, bit 1 = FLAG_ENCRYPTED. v1 emits
//     0x00: unsigned, unencrypted. Encryption is opt-in upstream too.
//   - The packed dict uses uint8 tag keys (`Tag` enum below) and
//     typed values (string / int / float / bool / bin) per the
//     upstream tag table.
//   - The stamp is an LXMF proof-of-work over the SHA-256 of the
//     packed-msgpack bytes. Computed asynchronously by
//     Discovery::Stamp; the Builder exposes serialize_unstamped() to
//     get the bytes that feed the PoW, and serialize_with_stamp() to
//     assemble the final wire frame once the 32-byte stamp is back.
//
// Serialisation primitives come from Common/MsgPack.h - same module
// LXMF uses, so the codebase has a single msgpack encoder.

#pragma once

#include <Arduino.h>
#include <Bytes.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include "../Common/MsgPack.h"

namespace Discovery {
namespace Announce {

// Upstream tag table - keep these constants in sync with
// reticulum/RNS/Discovery.py.
enum Tag : uint8_t {
  TAG_INTERFACE_TYPE  = 0x00,
  TAG_TRANSPORT       = 0x01,
  TAG_REACHABLE_ON    = 0x02,
  TAG_LATITUDE        = 0x03,
  TAG_LONGITUDE       = 0x04,
  TAG_HEIGHT          = 0x05,
  TAG_PORT            = 0x06,
  TAG_IFAC_NETNAME    = 0x07,
  TAG_IFAC_NETKEY     = 0x08,
  TAG_FREQUENCY       = 0x09,
  TAG_BANDWIDTH       = 0x0A,
  TAG_SPREADINGFACTOR = 0x0B,
  TAG_CODINGRATE      = 0x0C,
  TAG_MODULATION      = 0x0D,
  TAG_CHANNEL         = 0x0E,
  TAG_TRANSPORT_ID    = 0xFE,
  TAG_NAME            = 0xFF,
};

inline constexpr uint8_t FLAG_SIGNED    = 0x01;
inline constexpr uint8_t FLAG_ENCRYPTED = 0x02;
inline constexpr size_t  STAMP_SIZE     = 32;   // RNS::Identity::HASHLENGTH/8
inline constexpr size_t  MAX_WIRE_SIZE  = 1024; // generous upper bound; real
                                                 // announces top out near 200 B

// Field value-type tags. C++ doesn't get a tagged union for free;
// this is the simplest "one entry per field" representation.
struct Field {
  uint8_t tag = 0;
  enum class T : uint8_t { Str, Int, Float, Bool, Bin } type = T::Str;
  std::string s;       // Str
  int64_t i = 0;       // Int
  double f = 0.0;      // Float
  bool b = false;      // Bool
  RNS::Bytes bin;      // Bin
};

// Builder - accumulate fields in any order, then serialize() to
// the wire bytes. The internal representation is intentionally
// minimal - there are at most ~10-15 fields per announce, no need
// for a hash map.
class Builder {
public:
  Builder& set_str  (uint8_t tag, const std::string& s) { auto& f = slot(tag); f.type = Field::T::Str;   f.s = s; return *this; }
  Builder& set_int  (uint8_t tag, int64_t v)            { auto& f = slot(tag); f.type = Field::T::Int;   f.i = v; return *this; }
  Builder& set_float(uint8_t tag, double v)             { auto& f = slot(tag); f.type = Field::T::Float; f.f = v; return *this; }
  Builder& set_bool (uint8_t tag, bool v)               { auto& f = slot(tag); f.type = Field::T::Bool;  f.b = v; return *this; }
  Builder& set_bin  (uint8_t tag, const RNS::Bytes& b)  { auto& f = slot(tag); f.type = Field::T::Bin;   f.bin = b; return *this; }

  // Convenience wrappers using the canonical tag enum.
  Builder& name(const std::string& s)               { return set_str(TAG_NAME, s); }
  Builder& interface_type(const std::string& s)     { return set_str(TAG_INTERFACE_TYPE, s); }
  Builder& transport_enabled(bool v)                { return set_bool(TAG_TRANSPORT, v); }
  Builder& transport_id(const RNS::Bytes& hash)     { return set_bin(TAG_TRANSPORT_ID, hash); }
  Builder& lat(double v)                            { return set_float(TAG_LATITUDE, v); }
  Builder& lon(double v)                            { return set_float(TAG_LONGITUDE, v); }
  Builder& height(double v)                         { return set_float(TAG_HEIGHT, v); }
  Builder& frequency(int64_t hz)                    { return set_int(TAG_FREQUENCY, hz); }
  Builder& bandwidth(int64_t hz)                    { return set_int(TAG_BANDWIDTH, hz); }
  Builder& spreading_factor(int sf)                 { return set_int(TAG_SPREADINGFACTOR, sf); }
  Builder& coding_rate(int cr)                      { return set_int(TAG_CODINGRATE, cr); }
  Builder& port(int64_t p)                          { return set_int(TAG_PORT, p); }
  Builder& reachable_on(const std::string& s)       { return set_str(TAG_REACHABLE_ON, s); }
  Builder& modulation(const std::string& s)         { return set_str(TAG_MODULATION, s); }
  Builder& channel(int64_t v)                       { return set_int(TAG_CHANNEL, v); }

  // Serialise just the msgpack dict - the bytes that get hashed to
  // produce the LXStamper "material" input. Caller hands this to
  // Discovery::Stamp::submit() and assembles the final wire frame via
  // serialize_with_stamp() once the 32-byte stamp is back.
  // Returns empty Bytes on encoder overflow (shouldn't happen - the
  // MAX_WIRE_SIZE upper bound is generous).
  RNS::Bytes serialize_unstamped() const {
    uint8_t buf[MAX_WIRE_SIZE];
    size_t pos = 0;
    size_t n;
    n = Common::MsgPack::pack_map_header(&buf[pos], sizeof(buf) - pos, _fields.size());
    if (n == 0) return {};
    pos += n;
    for (const auto& f : _fields) {
      n = Common::MsgPack::pack_uint8(&buf[pos], sizeof(buf) - pos, f.tag);
      if (n == 0) return {};
      pos += n;
      switch (f.type) {
        case Field::T::Str:   n = Common::MsgPack::pack_str    (&buf[pos], sizeof(buf) - pos, f.s); break;
        case Field::T::Int:   n = Common::MsgPack::pack_int    (&buf[pos], sizeof(buf) - pos, f.i); break;
        case Field::T::Float: n = Common::MsgPack::pack_float64(&buf[pos], sizeof(buf) - pos, f.f); break;
        case Field::T::Bool:  n = Common::MsgPack::pack_bool   (&buf[pos], sizeof(buf) - pos, f.b); break;
        case Field::T::Bin:   n = Common::MsgPack::pack_bin    (&buf[pos], sizeof(buf) - pos, f.bin.data(), f.bin.size()); break;
      }
      if (n == 0) return {};
      pos += n;
    }
    return RNS::Bytes(buf, pos);
  }

  // Assemble the final wire frame: [flags=0x00] + packed-msgpack + stamp.
  // `stamp` must be exactly STAMP_SIZE bytes; pass a zero-filled buffer
  // to emit a cost=0 announce.
  static RNS::Bytes serialize_with_stamp(const RNS::Bytes& unstamped,
                                         const RNS::Bytes& stamp) {
    if (stamp.size() != STAMP_SIZE) return {};
    RNS::Bytes out;
    uint8_t flags = 0x00;
    out.append(&flags, 1);
    out.append(unstamped);
    out.append(stamp);
    return out;
  }

  // Direct read of the in-progress field list - useful for tests
  // that want to verify a builder's contents before serialization.
  const std::vector<Field>& fields() const { return _fields; }

private:
  Field& slot(uint8_t tag) {
    for (auto& f : _fields) { if (f.tag == tag) return f; }
    _fields.push_back({});
    _fields.back().tag = tag;
    return _fields.back();
  }
  std::vector<Field> _fields;
};

}  // namespace Announce
}  // namespace Discovery
