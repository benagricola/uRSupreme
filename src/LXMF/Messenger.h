// OLED messenger - send preset LXMF messages from the device itself,
// no web UI needed. inReach-style safety communicator, with the
// honest limits of LoRa: there is no backhaul guarantee, this is not
// an SOS service.
//
// The mode is a small page machine rendered into the 64x64 disp_area
// (Display.h calls render() while active() is true) and driven by two
// inputs (RNode_Firmware.ino routes them):
//   user button  short        next item
//   user button  long (<5 s)  select / send
//   power key    short        back / exit (also enters the mode)
// Presses longer than 5 s exit the mode and fall through to the
// global gestures (pairing, console), so those stay reachable.
//
// Messages send from the screen identity (LXMFGateway::screen_identity)
// through the normal gateway path: they land in the outbox and the
// conversation like any other send, and the delivery receipt drives
// the result page (Sent / Delivered / Failed) via on_outbox_status.
//
// Presets persist to /lxmf/messenger.json, managed from the web app.
// Bounds: MAX_PRESETS entries; content is capped so preset text plus
// the optional GPS suffix stays inside opportunistic-delivery size
// (no link or path round-trip needed when a route is known).

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Fonts/Org_01.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <string>
#include <vector>

#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"
#include "../Common/RnsLock.h"
#include "../Sensors/Position/L76K.h"
#include "LXMFTypes.h"
#include "LXMFGateway.h"

extern microStore::FileSystem filesystem;

namespace LXMF {
namespace Messenger {

inline constexpr const char* PRESETS_PATH   = "/lxmf/messenger.json";
inline constexpr size_t   MAX_PRESETS       = 8;
inline constexpr size_t   MAX_LABEL_LEN     = 24;
// Preset text cap. A full LXMF opportunistic payload must fit one
// packet (~295 B); framing + timestamp + the "@ lat,lon" suffix
// (~24 B) leave roughly this much room for the text itself.
inline constexpr size_t   MAX_CONTENT_LEN   = 200;
inline constexpr uint32_t RESULT_PAGE_TTL_MS = 30000;
// Long-press select threshold mirrors the global gesture boundary in
// button_event (700 ms separates click from hold).
inline constexpr unsigned long SELECT_PRESS_MS = 700;

struct Preset {
  std::string label;     // OLED list entry
  std::string dest_hex;  // 16-byte LXMF destination hash, hex
  std::string content;
  bool        gps = false;  // append "@ lat,lon" when a fix is valid
};

enum class Page : uint8_t { Hidden, NoIdentity, NoPresets, List, Confirm, Result };

namespace _detail {
  inline std::vector<Preset>& presets_ref() { static std::vector<Preset> v; return v; }
  inline Page&     page_ref()        { static Page p = Page::Hidden; return p; }
  inline size_t&   cursor_ref()      { static size_t c = 0; return c; }
  inline std::string& result_ref()   { static std::string s; return s; }
  inline uint32_t& result_at_ref()   { static uint32_t t = 0; return t; }
  inline RNS::Bytes& sent_hash_ref() { static RNS::Bytes b; return b; }
  // The preset list is written by the web task (POST replace) while
  // the main loop reads it (render, navigation, send). Everything
  // else - page, cursor, result - is main-loop-only. Guard exactly
  // the vector.
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
}

inline Page   page()       { return _detail::page_ref(); }
inline size_t cursor()     { return _detail::cursor_ref(); }
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
  if (_detail::cursor_ref() >= _detail::presets_ref().size()) {
    _detail::cursor_ref() = 0;
  }
}

inline size_t preset_count() {
  _detail::Guard g;
  return _detail::presets_ref().size();
}

inline void load(microStore::FileSystem& fs) {
  std::vector<Preset> next;
  if (fs.exists(PRESETS_PATH)) {
    std::vector<uint8_t> data;
    if (fs.readFile(PRESETS_PATH, data) > 0) {
      Common::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) == DeserializationError::Ok) {
        for (JsonObjectConst o : doc["presets"].as<JsonArrayConst>()) {
          if (next.size() >= MAX_PRESETS) break;
          Preset p;
          p.label    = (const char*)(o["label"]   | "");
          p.dest_hex = (const char*)(o["dest"]    | "");
          p.content  = (const char*)(o["content"] | "");
          p.gps      = (bool)(o["gps"] | false);
          if (p.label.size() > MAX_LABEL_LEN)     p.label.resize(MAX_LABEL_LEN);
          if (p.content.size() > MAX_CONTENT_LEN) p.content.resize(MAX_CONTENT_LEN);
          if (p.label.empty() || p.dest_hex.size() != 32 || p.content.empty()) continue;
          next.push_back(std::move(p));
        }
      }
    }
  }
  replace_presets(std::move(next));
}

inline void persist(microStore::FileSystem& fs) {
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
  fs.writeFile(PRESETS_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

inline void exit_mode() {
  _detail::page_ref()   = Page::Hidden;
  _detail::cursor_ref() = 0;
}

// Power-key short press: enter the mode, step back inside it.
inline void on_power_key() {
  switch (_detail::page_ref()) {
    case Page::Hidden:
      _detail::cursor_ref() = 0;
      if (LXMFGateway::screen_identity() == nullptr) {
        _detail::page_ref() = Page::NoIdentity;
      } else if (preset_count() == 0) {
        _detail::page_ref() = Page::NoPresets;
      } else {
        _detail::page_ref() = Page::List;
      }
      break;
    case Page::Confirm: _detail::page_ref() = Page::List; break;
    default:            exit_mode();                      break;
  }
}

inline void send_selected();

// User button while the mode is active. duration follows
// button_event's click semantics (ms between press and release).
inline void on_user_button(unsigned long duration) {
  const bool select = duration > SELECT_PRESS_MS;
  switch (_detail::page_ref()) {
    case Page::List: {
      const size_t n = preset_count();
      if (n == 0) {
        _detail::page_ref() = Page::NoPresets;
      } else if (select) {
        _detail::page_ref() = Page::Confirm;
      } else {
        _detail::cursor_ref() = (_detail::cursor_ref() + 1) % n;
      }
      break;
    }
    case Page::Confirm:
      if (select) send_selected();
      break;
    case Page::Result:
      exit_mode();
      break;
    default:
      exit_mode();
      break;
  }
}

inline void send_selected() {
  // Copy the preset out under the guard, then release it - the send
  // itself can take a while and must not hold the lock.
  Preset p;
  {
    _detail::Guard g;
    auto& v = _detail::presets_ref();
    if (_detail::cursor_ref() >= v.size()) { exit_mode(); return; }
    p = v[_detail::cursor_ref()];
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

// Auto-dismiss the result page so the device returns to its normal
// display when nobody is pressing buttons. Called each loop tick.
inline void tick() {
  if (_detail::page_ref() == Page::Result &&
      (millis() - _detail::result_at_ref()) > RESULT_PAGE_TTL_MS) {
    exit_mode();
  }
}

// ---- rendering ------------------------------------------------------
// Draws into the 64x64 disp_area canvas. Org_01 at size 1 is ~6 px
// per line; the layout is a title row plus up to 6 body rows.

inline void _line(GFXcanvas1& area, int16_t y, const char* text) {
  area.setCursor(2, y);
  area.print(text);
}

inline void render(GFXcanvas1& area) {
  area.fillRect(0, 0, area.width(), area.height(), 0 /*black*/);
  area.setFont(&Org_01);
  area.setTextWrap(false);
  area.setTextColor(1 /*white*/);
  area.setTextSize(1);

  const Page pg = _detail::page_ref();
  if (pg == Page::NoIdentity) {
    _line(area, 9,  "MESSAGES");
    _line(area, 27, "No screen");
    _line(area, 35, "identity set.");
    _line(area, 47, "Enable one in");
    _line(area, 55, "the web app.");
    return;
  }
  if (pg == Page::NoPresets) {
    _line(area, 9,  "MESSAGES");
    _line(area, 27, "No presets.");
    _line(area, 43, "Add some in");
    _line(area, 51, "the web app.");
    return;
  }
  if (pg == Page::List) {
    _line(area, 9, "SEND");
    _detail::Guard g;
    auto& v = _detail::presets_ref();
    const size_t cur = _detail::cursor_ref();
    // 4 visible rows so the navigation hints fit below; the window
    // scrolls to keep the cursor on screen. The device must explain
    // itself - whoever is holding it has no web UI in hand.
    const size_t rows  = 4;
    const size_t first = (cur >= rows) ? cur - rows + 1 : 0;
    int16_t y = 19;
    for (size_t i = first; i < v.size() && i < first + rows; ++i, y += 8) {
      area.setCursor(2, y);
      area.print(i == cur ? ">" : " ");
      area.setCursor(8, y);
      area.print(v[i].label.c_str());
    }
    _line(area, 54, "Btn: next");
    _line(area, 62, "Hold: choose");
    return;
  }
  if (pg == Page::Confirm) {
    _detail::Guard g;
    auto& v = _detail::presets_ref();
    if (v.empty()) { _detail::page_ref() = Page::NoPresets; return; }
    const Preset& p = v[_detail::cursor_ref() < v.size() ? _detail::cursor_ref() : 0];
    _line(area, 9, "SEND?");
    area.setCursor(2, 21);
    area.print(p.label.c_str());
    area.setCursor(2, 31);
    area.print(p.dest_hex.substr(0, 8).c_str());
    _line(area, 47, "Hold: send");
    _line(area, 55, "PWR: back");
    return;
  }
  if (pg == Page::Result) {
    _line(area, 9, "STATUS");
    area.setTextSize(1);
    area.setCursor(2, 31);
    area.print(_detail::result_ref().c_str());
    _line(area, 54, "Updates live.");
    _line(area, 62, "Any key: close");
    return;
  }
}

}  // namespace Messenger
}  // namespace LXMF
