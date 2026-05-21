// Discovery announce wire-format encoder.
//
// Builds the `app_data` payload that goes on an
// `rnstransport.discovery.interface` aspect announce — the upstream
// Reticulum InterfaceAnnouncer.get_interface_announce_data() in
// reticulum/RNS/Discovery.py is the reference implementation; the
// tag values + ordering + framing here match it byte-for-byte (so
// any standard listener — rmap.world's, or another RNS node running
// InterfaceAnnounceHandler — can parse what we emit).
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
//     packed bytes. v1 emits 32 zero bytes — stamp PoW will be
//     implemented in a later commit. Listeners with required_value=0
//     accept this; the default upstream required_value is 14 so
//     stricter listeners will drop us until we land the PoW.
//
// Self-contained: inlines a small msgpack encoder rather than
// depending on LXMF/RawMsgPack so the Discovery folder doesn't
// pull LXMFMinimal.h transitively.

#pragma once

#include <Arduino.h>
#include <Bytes.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <cstring>

namespace Discovery {
namespace Announce {

// Upstream tag table — keep these constants in sync with
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

namespace _msgpack {

inline void put_u8(std::vector<uint8_t>& out, uint8_t v) {
  out.push_back(v);
}
inline void put_be(std::vector<uint8_t>& out, uint64_t v, int n) {
  for (int i = n - 1; i >= 0; --i) out.push_back((uint8_t)(v >> (i * 8)));
}

// uint8 tag for a map key (0x00 .. 0xFF). Per upstream, keys are
// emitted as fixint when ≤127, else uint8 (0xCC). The Discovery
// tag table includes 0xFE / 0xFF so we always take the uint8 path
// for safety + match — the Python umsgpack also uses uint8 for
// values > positive-fixint.
inline void put_map_key(std::vector<uint8_t>& out, uint8_t k) {
  if (k <= 0x7F) { out.push_back(k); }
  else           { out.push_back(0xCC); out.push_back(k); }
}

inline void put_map_header(std::vector<uint8_t>& out, size_t n) {
  if (n <= 0x0F) {
    out.push_back(0x80 | (uint8_t)n);
  } else if (n <= 0xFFFF) {
    out.push_back(0xDE);
    put_be(out, n, 2);
  } else {
    out.push_back(0xDF);
    put_be(out, n, 4);
  }
}

inline void put_str(std::vector<uint8_t>& out, const std::string& s) {
  const size_t n = s.size();
  if (n <= 0x1F) {
    out.push_back(0xA0 | (uint8_t)n);
  } else if (n <= 0xFF) {
    out.push_back(0xD9);
    out.push_back((uint8_t)n);
  } else if (n <= 0xFFFF) {
    out.push_back(0xDA);
    put_be(out, n, 2);
  } else {
    out.push_back(0xDB);
    put_be(out, n, 4);
  }
  for (char c : s) out.push_back((uint8_t)c);
}

inline void put_bin(std::vector<uint8_t>& out, const uint8_t* data, size_t n) {
  if (n <= 0xFF) {
    out.push_back(0xC4);
    out.push_back((uint8_t)n);
  } else if (n <= 0xFFFF) {
    out.push_back(0xC5);
    put_be(out, n, 2);
  } else {
    out.push_back(0xC6);
    put_be(out, n, 4);
  }
  for (size_t i = 0; i < n; ++i) out.push_back(data[i]);
}

inline void put_bool(std::vector<uint8_t>& out, bool v) {
  out.push_back(v ? 0xC3 : 0xC2);
}

inline void put_int(std::vector<uint8_t>& out, int64_t v) {
  if (v >= 0) {
    if (v <= 0x7F)             { out.push_back((uint8_t)v); }
    else if (v <= 0xFF)        { out.push_back(0xCC); out.push_back((uint8_t)v); }
    else if (v <= 0xFFFF)      { out.push_back(0xCD); put_be(out, v, 2); }
    else if (v <= 0xFFFFFFFFLL){ out.push_back(0xCE); put_be(out, v, 4); }
    else                       { out.push_back(0xCF); put_be(out, (uint64_t)v, 8); }
  } else {
    if (v >= -32)              { out.push_back((uint8_t)(0xE0 | (v & 0x1F))); }
    else if (v >= -128)        { out.push_back(0xD0); out.push_back((uint8_t)(int8_t)v); }
    else if (v >= -32768)      { out.push_back(0xD1); put_be(out, (uint16_t)v, 2); }
    else if (v >= INT32_MIN)   { out.push_back(0xD2); put_be(out, (uint32_t)v, 4); }
    else                       { out.push_back(0xD3); put_be(out, (uint64_t)v, 8); }
  }
}

inline void put_float64(std::vector<uint8_t>& out, double v) {
  uint64_t bits;
  static_assert(sizeof(bits) == sizeof(v), "float64 bit-width");
  std::memcpy(&bits, &v, sizeof(bits));
  out.push_back(0xCB);
  put_be(out, bits, 8);
}

}  // namespace _msgpack

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

// Builder — accumulate fields in any order, then serialize() to
// the wire bytes. The internal representation is intentionally
// minimal — there are at most ~10-15 fields per announce, no need
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

  // Serialize to wire form: [flags=0x00] + packed-msgpack + stamp(32 B).
  // For v1 we send unsigned + unencrypted, with a zero-filled stamp.
  // Returns the full app_data ready to hand to RNS::Destination::announce.
  RNS::Bytes serialize() const {
    std::vector<uint8_t> packed;
    packed.reserve(64 + _fields.size() * 16);
    _msgpack::put_map_header(packed, _fields.size());
    for (const auto& f : _fields) {
      _msgpack::put_map_key(packed, f.tag);
      switch (f.type) {
        case Field::T::Str:   _msgpack::put_str    (packed, f.s); break;
        case Field::T::Int:   _msgpack::put_int    (packed, f.i); break;
        case Field::T::Float: _msgpack::put_float64(packed, f.f); break;
        case Field::T::Bool:  _msgpack::put_bool   (packed, f.b); break;
        case Field::T::Bin:   _msgpack::put_bin    (packed, f.bin.data(), f.bin.size()); break;
      }
    }

    std::vector<uint8_t> wire;
    wire.reserve(1 + packed.size() + STAMP_SIZE);
    wire.push_back(0x00);                                              // flags: unsigned + unencrypted
    wire.insert(wire.end(), packed.begin(), packed.end());             // packed dict
    wire.insert(wire.end(), STAMP_SIZE, 0x00);                         // zero stamp (PoW placeholder)
    return RNS::Bytes(wire.data(), wire.size());
  }

  // Direct read of the in-progress field list — useful for tests
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
