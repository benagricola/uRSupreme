// OLED messenger - read incoming messages and send presets from the
// device itself, no web UI needed. inReach-style safety communicator,
// with the honest limits of LoRa: there is no backhaul guarantee,
// this is not an SOS service.
//
// Incoming messages for the screen identity take over the display as
// a full message page (sender + wrapped text) for MESSAGE_VIEW_TTL_MS,
// then the normal status display returns. The PMU's charge LED blinks
// while the page is up. A message that arrives mid-send-flow does not
// steal the page (the LED still blinks); it is in the inbox as normal.
//
// Navigation is two buttons, short presses only:
//   power key    forward: enter the mode, choose, send
//   user button  next item; hold (~1 s) steps back / exits
// Presses past 5 s leave the mode and fall through to the global
// gestures (pairing, console), so those stay reachable.
//
// Presets are PRIVATE TO THE SCREEN IDENTITY: they live in the
// identity's own directory and are deleted outright when that
// identity stops being the screen identity - they may carry personal
// information and must never surface under another identity. A fresh
// enable seeds template messages (no recipient yet); the web editor
// fills the recipients in, and the device list only offers presets
// whose recipient is set. Bounds: MAX_PRESETS entries, 24-char
// labels, 200-byte content (text + optional GPS suffix + LXMF framing
// stays inside one opportunistic packet, ~295 B).

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Fonts/Picopixel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <initializer_list>
#include <string>
#include <vector>

#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"
#include "../Common/RnsLock.h"
#include "../Sensors/Position/L76K.h"
#include "LXMFTypes.h"
#include "LXMFGateway.h"

extern microStore::FileSystem filesystem;
// PMU charge-LED override for the message notification (Power.h).
extern void notify_led(bool blinking);

namespace LXMF {
namespace Messenger {

// Pre-identity-scoping storage location; deleted at boot so presets
// from that era cannot outlive the privacy model that replaced it.
inline constexpr const char* LEGACY_PRESETS_PATH = "/lxmf/messenger.json";
inline constexpr const char* PRESETS_FILENAME    = "/messenger.json";

inline constexpr size_t   MAX_PRESETS        = 8;
inline constexpr size_t   MAX_LABEL_LEN      = 24;
// Preset text cap. A full LXMF opportunistic payload must fit one
// packet (~295 B); framing + timestamp + the "@ lat,lon" suffix
// (~24 B) leave roughly this much room for the text itself.
inline constexpr size_t   MAX_CONTENT_LEN    = 200;
inline constexpr uint32_t RESULT_PAGE_TTL_MS = 30000;
inline constexpr uint32_t MESSAGE_VIEW_TTL_MS = 60000;
// User-button hold threshold mirrors the global gesture boundary in
// button_event (700 ms separates click from hold).
inline constexpr unsigned long HOLD_PRESS_MS = 700;

struct Preset {
  std::string label;     // OLED list entry
  std::string dest_hex;  // 16-byte LXMF destination hash, hex; "" = template,
                         // shown in the web editor but not on the device
  std::string content;
  bool        gps = false;  // append "@ lat,lon" when a fix is valid

  bool complete() const { return dest_hex.size() == 32; }
};

enum class Page : uint8_t { Hidden, NoIdentity, NoPresets, List, Confirm, Result, Message };

namespace _detail {
  inline std::vector<Preset>& presets_ref() { static std::vector<Preset> v; return v; }
  inline std::string& store_path_ref() { static std::string p; return p; }
  inline Page&     page_ref()        { static Page p = Page::Hidden; return p; }
  inline size_t&   cursor_ref()      { static size_t c = 0; return c; }
  inline std::string& result_ref()   { static std::string s; return s; }
  inline uint32_t& result_at_ref()   { static uint32_t t = 0; return t; }
  inline RNS::Bytes& sent_hash_ref() { static RNS::Bytes b; return b; }
  inline std::string& msg_from_ref() { static std::string s; return s; }
  inline std::string& msg_text_ref() { static std::string s; return s; }
  inline uint32_t& msg_at_ref()      { static uint32_t t = 0; return t; }
  // The preset list + store path are written by the web task (replace,
  // lifecycle) while the main loop reads them (render, navigation,
  // send). The rest of the state - page, cursor, result, message - is
  // main-loop-only (show_incoming runs on the delivery callback, also
  // main loop). Guard exactly the vector + path.
  inline SemaphoreHandle_t& mtx_handle() {
    static SemaphoreHandle_t m = nullptr;
    return m;
  }
  struct Guard {
    Guard() {
      if (mtx_handle() == nullptr) mtx_handle() = xSemaphoreCreateMutex();
      xSemaphoreTake(mtx_handle(), portMAX_DELAY);
    }
    ~Guard() { xSemaphoreGive(mtx_handle()); }
  };

  // Indices of device-usable presets (recipient set), in list order.
  // Callers hold the Guard.
  inline std::vector<size_t> complete_indices_locked() {
    std::vector<size_t> idx;
    auto& v = presets_ref();
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].complete()) idx.push_back(i);
    }
    return idx;
  }
}

inline Page   page()       { return _detail::page_ref(); }
inline bool   active()     { return _detail::page_ref() != Page::Hidden; }
inline const std::string& result_text() { return _detail::result_ref(); }

// Stable page label for the display diag endpoint, so a test script
// can assert which page it is capturing.
inline const char* page_name() {
  switch (_detail::page_ref()) {
    case Page::Hidden:     return "hidden";
    case Page::NoIdentity: return "no_identity";
    case Page::NoPresets:  return "no_presets";
    case Page::List:       return "list";
    case Page::Confirm:    return "confirm";
    case Page::Result:     return "result";
    case Page::Message:    return "message";
  }
  return "unknown";
}

// Copy-out for the web GET handler (web task).
inline std::vector<Preset> presets_snapshot() {
  _detail::Guard g;
  return _detail::presets_ref();
}

// Whole-list replace from the web POST handler (web task). The list
// is tiny; replacing it whole keeps one writer path. The cursor is
// clamped so a shrink can't leave it dangling mid-navigation.
inline void replace_presets(std::vector<Preset>&& next) {
  _detail::Guard g;
  _detail::presets_ref() = std::move(next);
  const size_t n = _detail::complete_indices_locked().size();
  if (_detail::cursor_ref() >= n) _detail::cursor_ref() = 0;
}

// Count of device-usable presets (recipient set).
inline size_t usable_count() {
  _detail::Guard g;
  return _detail::complete_indices_locked().size();
}

inline bool has_store() {
  _detail::Guard g;
  return !_detail::store_path_ref().empty();
}

inline void persist(microStore::FileSystem& fs) {
  std::string path;
  {
    _detail::Guard g;
    path = _detail::store_path_ref();
  }
  if (path.empty()) return;
  const std::vector<Preset> snap = presets_snapshot();
  Common::PsramJsonDocument doc;
  JsonArray arr = doc["presets"].to<JsonArray>();
  for (const auto& p : snap) {
    JsonObject o = arr.add<JsonObject>();
    o["label"]   = p.label;
    o["dest"]    = p.dest_hex;
    o["content"] = p.content;
    o["gps"]     = p.gps;
  }
  String out;
  serializeJson(doc, out);
  fs.writeFile(path.c_str(),
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// Starter messages seeded on a fresh screen-enable. Recipients are
// deliberately empty: the web editor fills them in, and the device
// list only offers presets with a recipient set.
inline std::vector<Preset> default_templates() {
  return {
    { "OK",          "", "All good here.",      false },
    { "On my way",   "", "On my way.",          false },
    { "Need pickup", "", "Please come get me.", true  },
  };
}

inline void exit_mode() {
  _detail::page_ref()   = Page::Hidden;
  _detail::cursor_ref() = 0;
  notify_led(false);
}

// Screen-identity lifecycle, called from LXMFGateway::set_screen_identity
// (web task) and from identity load at boot.
//
// Enable: presets come from the identity's own directory; a fresh
// enable seeds the templates. Disable: the file and the in-RAM list
// are destroyed - presets are private to the identity that made them
// and must not survive into another identity's tenure.
inline void on_screen_identity_changed(bool enabled, const std::string& identity_dir) {
  if (!enabled) {
    std::string old_path;
    {
      _detail::Guard g;
      old_path = _detail::store_path_ref();
      _detail::store_path_ref().clear();
      _detail::presets_ref().clear();
      _detail::cursor_ref() = 0;
    }
    if (!old_path.empty() && filesystem.exists(old_path.c_str())) {
      filesystem.remove(old_path.c_str());
    }
    exit_mode();
    return;
  }

  const std::string path = identity_dir + PRESETS_FILENAME;
  std::vector<Preset> next;
  bool from_disk = false;
  if (filesystem.exists(path.c_str())) {
    std::vector<uint8_t> data;
    if (filesystem.readFile(path.c_str(), data) > 0) {
      Common::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) == DeserializationError::Ok) {
        from_disk = true;
        for (JsonObjectConst o : doc["presets"].as<JsonArrayConst>()) {
          if (next.size() >= MAX_PRESETS) break;
          Preset p;
          p.label    = (const char*)(o["label"]   | "");
          p.dest_hex = (const char*)(o["dest"]    | "");
          p.content  = (const char*)(o["content"] | "");
          p.gps      = (bool)(o["gps"] | false);
          if (p.label.size() > MAX_LABEL_LEN)     p.label.resize(MAX_LABEL_LEN);
          if (p.content.size() > MAX_CONTENT_LEN) p.content.resize(MAX_CONTENT_LEN);
          if (p.label.empty() || p.content.empty()) continue;
          if (!p.dest_hex.empty() && p.dest_hex.size() != 32) p.dest_hex.clear();
          next.push_back(std::move(p));
        }
      }
    }
  }
  if (!from_disk) next = default_templates();
  {
    _detail::Guard g;
    _detail::store_path_ref() = path;
    _detail::presets_ref()    = std::move(next);
    _detail::cursor_ref()     = 0;
  }
  if (!from_disk) persist(filesystem);
}

// Boot-time cleanup: the pre-identity-scoping device-wide presets
// file must not linger (it may hold a previous tenant's messages).
// The active holder's own store is loaded by the gateway's identity
// load path via on_screen_identity_changed.
inline void boot(microStore::FileSystem& fs) {
  if (fs.exists(LEGACY_PRESETS_PATH)) fs.remove(LEGACY_PRESETS_PATH);
}

// ---- incoming message view ------------------------------------------

// Full-screen incoming-message page. Called from the gateway delivery
// callback (main loop) for the screen identity only. If the holder is
// mid-send-flow, the page is not stolen - the LED still announces the
// arrival and the message waits in the inbox.
inline void show_incoming(const std::string& from_name, const std::string& content) {
  const Page pg = _detail::page_ref();
  if (pg != Page::Hidden && pg != Page::Message) {
    notify_led(true);
    return;
  }
  _detail::msg_from_ref() = from_name;
  _detail::msg_text_ref() = content.substr(0, MAX_CONTENT_LEN);
  _detail::msg_at_ref()   = millis();
  _detail::page_ref()     = Page::Message;
  notify_led(true);
}

inline void send_selected();

// Power key, short press: forward. Enters the mode, chooses, sends.
inline void on_power_key() {
  switch (_detail::page_ref()) {
    case Page::Hidden:
      _detail::cursor_ref() = 0;
      if (LXMFGateway::screen_identity() == nullptr) {
        _detail::page_ref() = Page::NoIdentity;
      } else if (usable_count() == 0) {
        _detail::page_ref() = Page::NoPresets;
      } else {
        _detail::page_ref() = Page::List;
      }
      break;
    case Page::List:    _detail::page_ref() = Page::Confirm; break;
    case Page::Confirm: send_selected();                     break;
    default:            exit_mode();                         break;
  }
}

// User button. Short press: next item (closes the read-only pages).
// Hold: back / exit. duration follows button_event's click semantics.
inline void on_user_button(unsigned long duration) {
  const bool hold = duration > HOLD_PRESS_MS;
  if (hold) {
    switch (_detail::page_ref()) {
      case Page::Confirm: _detail::page_ref() = Page::List; break;
      default:            exit_mode();                      break;
    }
    return;
  }
  switch (_detail::page_ref()) {
    case Page::List: {
      const size_t n = usable_count();
      if (n == 0) _detail::page_ref() = Page::NoPresets;
      else        _detail::cursor_ref() = (_detail::cursor_ref() + 1) % n;
      break;
    }
    case Page::Confirm:
      break;  // choose/send is the power key; back is a hold
    default:
      exit_mode();
      break;
  }
}

inline void send_selected() {
  // Copy the selected preset out under the guard, then release it -
  // the send itself can take a while and must not hold the lock.
  Preset p;
  {
    _detail::Guard g;
    const auto idx = _detail::complete_indices_locked();
    if (_detail::cursor_ref() >= idx.size()) { exit_mode(); return; }
    p = _detail::presets_ref()[idx[_detail::cursor_ref()]];
  }

  const LXMFIdentity* a = LXMFGateway::screen_identity();
  if (!a) { _detail::page_ref() = Page::NoIdentity; return; }

  RNS::Bytes dest;
  dest.assignHex(p.dest_hex.c_str());
  if (dest.size() != 16) {
    _detail::result_ref()    = "Bad address";
    _detail::result_at_ref() = millis();
    _detail::page_ref()      = Page::Result;
    return;
  }

  std::string content = p.content;
  if (p.gps) {
    const Sensors::L76K::Fix fix = Sensors::L76K::last_fix();
    if (fix.valid) {
      char buf[40];
      snprintf(buf, sizeof(buf), "\n@ %.5f,%.5f",
               fix.latitude_deg, fix.longitude_deg);
      content += buf;
    }
  }

  MessageRecord rec;
  const char* err = nullptr;
  bool queued = false;
  // The button path runs on the main loop OUTSIDE its rns_lock
  // section (input_read comes after the guarded RNS block), and the
  // gateway send touches RNS state the web task also uses - so take
  // the lock here, exactly like a web handler would.
  Common::RnsLock::Guard rns_guard;
  const bool ok = LXMFGateway::send(a->id, dest, "", content, nullptr,
                                    rec, &err, &queued);
  _detail::sent_hash_ref() = rec.packet_hash;
  if (ok)          _detail::result_ref() = "Sent";
  else if (queued) _detail::result_ref() = "Finding route";
  else             _detail::result_ref() = "Failed";
  _detail::result_at_ref() = millis();
  _detail::page_ref()      = Page::Result;
}

// Delivery receipts, forwarded from the gateway's outbox status
// callback. Updates the result page while it is showing.
inline void on_outbox_status(const RNS::Bytes& hash, OutboxStatus status) {
  if (_detail::sent_hash_ref().size() == 0) return;
  if (!(hash == _detail::sent_hash_ref())) return;
  switch (status) {
    case OutboxStatus::Delivered: _detail::result_ref() = "Delivered"; break;
    case OutboxStatus::Sent:      _detail::result_ref() = "Sent";      break;
    case OutboxStatus::Failed:    _detail::result_ref() = "Failed";    break;
    default: return;
  }
  if (_detail::page_ref() == Page::Result) {
    _detail::result_at_ref() = millis();   // keep the page up for the update
  }
}

// Auto-dismiss timers: the result page and the incoming-message page
// both return the display to normal when nobody presses anything.
inline void tick() {
  const Page pg = _detail::page_ref();
  if (pg == Page::Result &&
      (millis() - _detail::result_at_ref()) > RESULT_PAGE_TTL_MS) {
    exit_mode();
  } else if (pg == Page::Message &&
             (millis() - _detail::msg_at_ref()) > MESSAGE_VIEW_TTL_MS) {
    exit_mode();
  }
}

// ---- rendering ------------------------------------------------------
// Two fonts, both effectively fixed-pitch, picked over Org_01 (which
// is variable width and clipped at the panel edge):
//   * Picopixel for chrome - titles, list rows, hints. The same font
//     the identity-code page uses; ~4 px per glyph, 7 px line step.
//     User-supplied text (labels, sender names) is truncated by
//     MEASURED pixels, not character counts, since a few glyphs are
//     wider.
//   * The built-in classic GFX 6x8 font for message bodies and the
//     result status - slightly bigger for reading, and exactly fixed
//     width: 10 columns across the 64 px panel, so wrapping is
//     deterministic. (Classic-font cursors are glyph-top, not
//     baseline.)

inline void _line(GFXcanvas1& area, int16_t y, const char* text) {
  area.setCursor(2, y);
  area.print(text);
}

// Truncate `text` until it measures inside `max_px` for the currently
// selected font. Pixel-true, so near-mono fonts cannot clip.
inline std::string _fit(GFXcanvas1& area, const std::string& text, int16_t max_px) {
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
inline void _wrap_classic(GFXcanvas1& area, const std::string& text,
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
    std::string line = text.substr(pos, take);
    // Newlines in content end the line early.
    const size_t nl = line.find('\n');
    if (nl != std::string::npos) { line.resize(nl); take = nl; }
    if (row == max_rows - 1 && pos + take < text.size()) {
      if (line.size() >= cols) line.resize(cols - 1);
      line += "~";
    }
    area.setCursor(2, y_top + (int16_t)(row * 8));
    area.print(line.c_str());
    pos += take;
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\n')) ++pos;
    ++row;
  }
  area.setFont(&Picopixel);
}

// Bottom-anchored hint block, Picopixel, 7 px steps. Button names
// match the board silkscreen: PWR, BOOT (RST is not software-
// readable).
inline void _hints(GFXcanvas1& area, std::initializer_list<const char*> lines) {
  area.setFont(&Picopixel);
  int16_t y = (int16_t)(area.height() - 2 - 7 * (lines.size() - 1));
  area.drawFastHLine(0, y - 9, area.width(), 1);
  for (const char* l : lines) {
    _line(area, y, l);
    y += 7;
  }
}

inline void render(GFXcanvas1& area) {
  area.fillRect(0, 0, area.width(), area.height(), 0 /*black*/);
  area.setFont(&Picopixel);
  area.setTextWrap(false);
  area.setTextColor(1 /*white*/);
  area.setTextSize(1);
  const int16_t h = area.height();

  const Page pg = _detail::page_ref();
  if (pg == Page::Message) {
    // Sender on the title row, message in the bigger classic font
    // below, dismiss hint at the bottom.
    _line(area, 7, _fit(area, _detail::msg_from_ref(), 60).c_str());
    area.drawFastHLine(0, 10, area.width(), 1);
    const int body_rows = (h - 16 - 12) / 8;
    _wrap_classic(area, _detail::msg_text_ref(), 16, body_rows);
    _hints(area, {"Any button closes"});
    return;
  }
  if (pg == Page::NoIdentity) {
    _line(area, 7,  "MESSAGES");
    area.drawFastHLine(0, 10, area.width(), 1);
    _line(area, 20, "No screen identity");
    _line(area, 27, "is set.");
    _line(area, 38, "Enable one in the");
    _line(area, 45, "web app.");
    if (h > 64) _hints(area, {"Any button closes"});
    return;
  }
  if (pg == Page::NoPresets) {
    _line(area, 7,  "MESSAGES");
    area.drawFastHLine(0, 10, area.width(), 1);
    _line(area, 20, "No messages are");
    _line(area, 27, "set up.");
    _line(area, 38, "Add some in the");
    _line(area, 45, "web app.");
    if (h > 64) _hints(area, {"Any button closes"});
    return;
  }
  if (pg == Page::List) {
    _line(area, 7, "SEND");
    area.drawFastHLine(0, 10, area.width(), 1);
    _detail::Guard g;
    auto& v = _detail::presets_ref();
    const auto idx = _detail::complete_indices_locked();
    const size_t cur = _detail::cursor_ref();
    // Rows fill whatever height is left above the hint block; the
    // window scrolls to keep the cursor on screen. The device must
    // explain itself - whoever holds it has no web UI in hand.
    const size_t rows  = (size_t)((h - 18 - 32) / 7);
    const size_t first = (cur >= rows) ? cur - rows + 1 : 0;
    int16_t y = 18;
    for (size_t i = first; i < idx.size() && i < first + rows; ++i, y += 7) {
      area.setCursor(2, y);
      area.print(i == cur ? ">" : " ");
      area.setCursor(8, y);
      area.print(_fit(area, v[idx[i]].label, 54).c_str());
    }
    _hints(area, {"Tap PWR: pick", "Tap BOOT: next", "Hold BOOT: back"});
    return;
  }
  if (pg == Page::Confirm) {
    _detail::Guard g;
    const auto idx = _detail::complete_indices_locked();
    if (idx.empty()) { _detail::page_ref() = Page::NoPresets; return; }
    const size_t cur = _detail::cursor_ref() < idx.size() ? _detail::cursor_ref() : 0;
    const Preset& p = _detail::presets_ref()[idx[cur]];
    _line(area, 7, "SEND?");
    area.drawFastHLine(0, 10, area.width(), 1);
    _line(area, 20, _fit(area, p.label, 60).c_str());
    // The recipient's prefix, then the message itself in the reading
    // font, so the holder confirms what actually goes out.
    const std::string to = "To: " + p.dest_hex.substr(0, 8);
    _line(area, 27, to.c_str());
    const int body_rows = (h - 33 - 19) / 8;
    if (body_rows > 0) _wrap_classic(area, p.content, 33, body_rows);
    _hints(area, {"Tap PWR: send", "Hold BOOT: back"});
    return;
  }
  if (pg == Page::Result) {
    _line(area, 7, "STATUS");
    area.drawFastHLine(0, 10, area.width(), 1);
    area.setFont(nullptr);
    area.setCursor(2, 24);
    area.print(_detail::result_ref().c_str());
    area.setFont(&Picopixel);
    _hints(area, {"Updates live", "Any button closes"});
    return;
  }
}
}  // namespace Messenger
}  // namespace LXMF
