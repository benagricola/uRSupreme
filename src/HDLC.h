#pragma once

#include <Bytes.h>
#include <stdint.h>
#include <stddef.h>

namespace HDLC {

  static constexpr uint8_t FLAG     = 0x7E;
  static constexpr uint8_t ESC      = 0x7D;
  static constexpr uint8_t ESC_MASK = 0x20;

  inline void encode(const RNS::Bytes& in, RNS::Bytes& out) {
    out.append((uint8_t)FLAG);
    const uint8_t* p = in.data();
    size_t n = in.size();
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = p[i];
      if (b == FLAG || b == ESC) {
        out.append((uint8_t)ESC);
        out.append((uint8_t)(b ^ ESC_MASK));
      } else {
        out.append(b);
      }
    }
    out.append((uint8_t)FLAG);
  }

  class Decoder {
  public:
    Decoder() : _in_frame(false), _escape(false) {}

    template<typename FrameHandler>
    void feed(uint8_t b, FrameHandler on_frame) {
      if (b == FLAG) {
        if (_in_frame && _buf.size() > 0) on_frame(_buf);
        _buf.clear();
        _in_frame = true;
        _escape = false;
        return;
      }
      if (!_in_frame) return;
      if (_escape) {
        _buf.append((uint8_t)(b ^ ESC_MASK));
        _escape = false;
      } else if (b == ESC) {
        _escape = true;
      } else {
        _buf.append(b);
      }
    }

    void reset() { _buf.clear(); _in_frame = false; _escape = false; }
    size_t buffered() const { return _buf.size(); }

  private:
    RNS::Bytes _buf;
    bool _in_frame;
    bool _escape;
  };

}
