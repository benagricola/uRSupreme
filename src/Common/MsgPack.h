// MsgPack encoder + decoder helpers.
//
// Self-contained: no library dependency, no heap allocations. Each
// encoder writes into a caller-supplied buffer and returns bytes
// written (0 on overflow). Each decoder reads from a (data, len,
// &offset) triple, advancing offset as it goes.
//
// Why hand-rolled, not the hideakitai/MsgPack library we pull in
// via platformio.ini: Packer writes to a std::vector with the
// default allocator (internal heap). LXMF's encoder needs the
// buffer routed through PSRAM for large attachment sends - without
// that, attachment encoding lands in internal SRAM and reintroduces
// the WiFi/lwIP fragmentation we worked to clear. Until the library
// supports a custom allocator (or we fork+patch one), the
// hand-rolled encoder lets us pre-allocate the destination buffer
// in PSRAM and pack into it directly.
//
// Single source of truth for msgpack encoding across the project -
// used by LXMF wire-format builders and by Discovery announces.
//
// What's here:
//   reads  - read_bin_or_str, skip_element
//   writes - pack_uint8, pack_str, pack_bin, pack_bin_str,
//            pack_bin_header, pack_float64, pack_int, pack_bool,
//            pack_array_header, pack_map_header
//
// All writers return 0 if the destination is too small; callers
// should check and abort encoding if so.

#pragma once

#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <cstring>

namespace Common {
namespace MsgPack {

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

// Just the bin-format header (bin8 / bin16 / bin32). Returns the
// header length; caller writes the body bytes separately. Used for
// staging-backed payloads where the body is streamed chunk by chunk
// rather than memcpy'd from a vector.
inline size_t pack_bin_header(uint8_t* buf, size_t buflen, size_t dlen) {
  if (dlen <= 255) {
    if (buflen < 2) return 0;
    buf[0] = 0xC4;
    buf[1] = (uint8_t)dlen;
    return 2;
  }
  if (dlen <= 0xFFFF) {
    if (buflen < 3) return 0;
    buf[0] = 0xC5;
    buf[1] = (dlen >> 8) & 0xFF;
    buf[2] = dlen & 0xFF;
    return 3;
  }
  if (buflen < 5) return 0;
  buf[0] = 0xC6;
  buf[1] = (dlen >> 24) & 0xFF;
  buf[2] = (dlen >> 16) & 0xFF;
  buf[3] = (dlen >> 8) & 0xFF;
  buf[4] = dlen & 0xFF;
  return 5;
}

inline size_t pack_bin(uint8_t* buf, size_t buflen, const uint8_t* data, size_t dlen) {
  size_t pos = pack_bin_header(buf, buflen, dlen);
  if (pos == 0) return 0;
  if (pos + dlen > buflen) return 0;
  memcpy(&buf[pos], data, dlen);
  return pos + dlen;
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

// Positive integer ≤ 255. Picks fixint when possible, else uint8.
// Also serves as the "uint8 map key" helper.
inline size_t pack_uint8(uint8_t* buf, size_t buflen, uint8_t v) {
  if (buflen < 1) return 0;
  if (v <= 0x7F) { buf[0] = v; return 1; }
  if (buflen < 2) return 0;
  buf[0] = 0xCC; buf[1] = v; return 2;
}

// Full signed integer pack. Picks the narrowest msgpack encoding
// that fits (positive-fixint / negative-fixint / int8 / int16 / etc).
inline size_t pack_int(uint8_t* buf, size_t buflen, int64_t v) {
  if (v >= 0) {
    if (v <= 0x7F)              { if (buflen < 1) return 0; buf[0] = (uint8_t)v; return 1; }
    if (v <= 0xFF)              { if (buflen < 2) return 0; buf[0] = 0xCC; buf[1] = (uint8_t)v; return 2; }
    if (v <= 0xFFFF)            { if (buflen < 3) return 0; buf[0] = 0xCD; buf[1] = (v>>8)&0xFF; buf[2] = v&0xFF; return 3; }
    if (v <= 0xFFFFFFFFLL)      { if (buflen < 5) return 0; buf[0] = 0xCE;
                                  buf[1] = (v>>24)&0xFF; buf[2] = (v>>16)&0xFF;
                                  buf[3] = (v>>8)&0xFF;  buf[4] = v&0xFF; return 5; }
    if (buflen < 9) return 0;   buf[0] = 0xCF;
    for (int i = 7; i >= 0; --i) buf[8 - i] = (((uint64_t)v) >> (i * 8)) & 0xFF;
    return 9;
  } else {
    if (v >= -32)               { if (buflen < 1) return 0; buf[0] = (uint8_t)(0xE0 | (v & 0x1F)); return 1; }
    if (v >= -128)              { if (buflen < 2) return 0; buf[0] = 0xD0; buf[1] = (uint8_t)(int8_t)v; return 2; }
    if (v >= -32768)            { if (buflen < 3) return 0; buf[0] = 0xD1; buf[1] = (v>>8)&0xFF; buf[2] = v&0xFF; return 3; }
    if (v >= INT32_MIN)         { if (buflen < 5) return 0; buf[0] = 0xD2;
                                  buf[1] = (v>>24)&0xFF; buf[2] = (v>>16)&0xFF;
                                  buf[3] = (v>>8)&0xFF;  buf[4] = v&0xFF; return 5; }
    if (buflen < 9) return 0;   buf[0] = 0xD3;
    for (int i = 7; i >= 0; --i) buf[8 - i] = (((uint64_t)v) >> (i * 8)) & 0xFF;
    return 9;
  }
}

inline size_t pack_bool(uint8_t* buf, size_t buflen, bool v) {
  if (buflen < 1) return 0;
  buf[0] = v ? 0xC3 : 0xC2;
  return 1;
}

// Write a string (msgpack `str` type, distinct from `bin`). Sideband
// uses str for filenames and image extensions; using `bin` here
// would break interop.
inline size_t pack_str(uint8_t* buf, size_t buflen, const std::string& s) {
  const size_t slen = s.length();
  size_t pos = 0;
  if (slen <= 31) {
    if (pos + 1 + slen > buflen) return 0;
    buf[pos++] = 0xA0 | (uint8_t)slen;
  } else if (slen <= 255) {
    if (pos + 2 + slen > buflen) return 0;
    buf[pos++] = 0xD9;
    buf[pos++] = (uint8_t)slen;
  } else if (slen <= 0xFFFF) {
    if (pos + 3 + slen > buflen) return 0;
    buf[pos++] = 0xDA;
    buf[pos++] = (slen >> 8) & 0xFF; buf[pos++] = slen & 0xFF;
  } else {
    if (pos + 5 + slen > buflen) return 0;
    buf[pos++] = 0xDB;
    buf[pos++] = (slen >> 24) & 0xFF; buf[pos++] = (slen >> 16) & 0xFF;
    buf[pos++] = (slen >> 8)  & 0xFF; buf[pos++] =  slen        & 0xFF;
  }
  memcpy(&buf[pos], s.c_str(), slen);
  pos += slen;
  return pos;
}

// Write an array header. Caller follows with `n` packed elements.
inline size_t pack_array_header(uint8_t* buf, size_t buflen, size_t n) {
  if (n <= 15) {
    if (buflen < 1) return 0;
    buf[0] = 0x90 | (uint8_t)n;
    return 1;
  }
  if (n <= 0xFFFF) {
    if (buflen < 3) return 0;
    buf[0] = 0xDC;
    buf[1] = (n >> 8) & 0xFF; buf[2] = n & 0xFF;
    return 3;
  }
  if (buflen < 5) return 0;
  buf[0] = 0xDD;
  buf[1] = (n >> 24) & 0xFF; buf[2] = (n >> 16) & 0xFF;
  buf[3] = (n >> 8)  & 0xFF; buf[4] =  n        & 0xFF;
  return 5;
}

// Write a map header. Picks the narrowest msgpack encoding that
// fits the entry count. Caller follows with `n` key/value pairs.
inline size_t pack_map_header(uint8_t* buf, size_t buflen, size_t n) {
  if (n <= 15) {
    if (buflen < 1) return 0;
    buf[0] = 0x80 | (uint8_t)n;
    return 1;
  }
  if (n <= 0xFFFF) {
    if (buflen < 3) return 0;
    buf[0] = 0xDE;
    buf[1] = (n >> 8) & 0xFF; buf[2] = n & 0xFF;
    return 3;
  }
  if (buflen < 5) return 0;
  buf[0] = 0xDF;
  buf[1] = (n >> 24) & 0xFF; buf[2] = (n >> 16) & 0xFF;
  buf[3] = (n >> 8)  & 0xFF; buf[4] =  n        & 0xFF;
  return 5;
}

}  // namespace MsgPack
}  // namespace Common
