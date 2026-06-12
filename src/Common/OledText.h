// Shared OLED text helpers for full-screen views (messenger, sensors).
// Drawing conventions: Picopixel for dense rows (7 px steps, baseline
// cursors), the classic built-in 6x8 font for body text (glyph-top
// cursors, 10 columns across the 64 px panel).
#pragma once

#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <string>
#include <initializer_list>
#include <algorithm>

namespace Common {
namespace OledText {

inline void line(GFXcanvas1& area, int16_t y, const char* text) {
  area.setCursor(2, y);
  area.print(text);
}

// Truncate `text` until it measures inside `max_px` for the currently
// selected font. Pixel-true, so near-mono fonts cannot clip.
inline std::string fit(GFXcanvas1& area, const std::string& text, int16_t max_px) {
  std::string t = text;
  int16_t x1, y1;
  uint16_t w, hh;
  while (!t.empty()) {
    area.getTextBounds(t.c_str(), 0, 0, &x1, &y1, &w, &hh);
    if ((int16_t)w + x1 <= max_px) break;
    t.pop_back();
  }
  return t;
}

// Greedy word-wrap in the classic 6x8 font: exactly 10 columns, 8 px
// per row, cursor at glyph top. The final row gets a trailing '~'
// when text was left over.
inline void wrap_classic(GFXcanvas1& area, const std::string& text,
                         int16_t y_top, int max_rows) {
  constexpr size_t cols = 10;
  area.setFont(nullptr);   // classic built-in font
  size_t pos = 0;
  int row = 0;
  while (pos < text.size() && row < max_rows) {
    size_t take = std::min(cols, text.size() - pos);
    if (pos + take < text.size()) {
      const size_t brk = text.rfind(' ', pos + take);
      if (brk != std::string::npos && brk > pos) take = brk - pos;
    }
    std::string ln = text.substr(pos, take);
    // Newlines in content end the line early.
    const size_t nl = ln.find('\n');
    if (nl != std::string::npos) { ln.resize(nl); take = nl; }
    if (row == max_rows - 1 && pos + take < text.size()) {
      if (ln.size() >= cols) ln.resize(cols - 1);
      ln += "~";
    }
    area.setCursor(2, y_top + (int16_t)(row * 8));
    area.print(ln.c_str());
    pos += take;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) ++pos;
    ++row;
  }
  area.setFont(&Picopixel);
}

// Bottom-anchored hint block, Picopixel, 7 px steps. Button names
// match the board silkscreen: PWR, BOOT (RST is not software-
// readable).
inline void hints(GFXcanvas1& area, std::initializer_list<const char*> lines) {
  area.setFont(&Picopixel);
  int16_t y = (int16_t)(area.height() - 2 - 7 * (lines.size() - 1));
  area.drawFastHLine(0, y - 9, area.width(), 1);
  for (const char* l : lines) {
    line(area, y, l);
    y += 7;
  }
}

}  // namespace OledText
}  // namespace Common
