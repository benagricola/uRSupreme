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
// labels, 200-byte content (text + LXMF framing + an attached
// telemetry blob stays inside one opportunistic packet, ~295 B).

#pragma once

#include <Arduino.h>
#include "../Common/OledText.h"
#include "../Display/ScreenFramework.h"
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
#include "../Sensors/Position/Gnss.h"
#include "LXMFTypes.h"
#include "LXMFGateway.h"
#include "TelemetryShare.h"

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
// packet (~295 B); framing + timestamp + an attached telemetry blob
// leave roughly this much room for the text itself.
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
  // Telemetry attached to every send of this preset - the same options
  // as the web compose popover. All items off = nothing attached.
  // share_s > 0 makes the send live: the recipient is granted update
  // requests for that window and offered the rate.
  bool     tel_location    = false;
  bool     tel_environment = false;
  bool     tel_battery     = false;
  bool     tel_compass     = false;
  uint32_t tel_share_s     = 0;
  uint32_t tel_rate_s      = 60;

  bool complete() const { return dest_hex.size() == 32; }
  uint8_t telemetry_items() const {
    return (tel_location    ? TelemetryShare::ITEM_LOCATION    : 0)
         | (tel_environment ? TelemetryShare::ITEM_ENVIRONMENT : 0)
         | (tel_battery     ? TelemetryShare::ITEM_BATTERY     : 0)
         | (tel_compass     ? TelemetryShare::ITEM_COMPASS     : 0);
  }
};

enum class Page : uint8_t { Hidden, NoIdentity, NoPresets, List, Confirm, Result, Message };

namespace _detail {
  inline std::vector<Preset>& presets_ref() { static std::vector<Preset> v; return v; }
  inline std::string& store_path_ref() { static std::string p; return p; }
  inline Page&     page_ref()        { static Page p = Page::Hidden; return p; }
  // True while the showing Result page belongs to a send that granted
  // live telemetry updates - drives the chrome spinner.
  inline bool&     result_live_ref() { static bool v = false; return v; }
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
    JsonObject t = o["tel"].to<JsonObject>();
    t["location"]    = p.tel_location;
    t["environment"] = p.tel_environment;
    t["battery"]     = p.tel_battery;
    t["compass"]     = p.tel_compass;
    t["share_s"]     = p.tel_share_s;
    t["rate_s"]      = p.tel_rate_s;
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
  std::vector<Preset> v(3);
  v[0].label = "OK";          v[0].content = "All good here.";
  v[1].label = "On my way";   v[1].content = "On my way.";
  v[2].label = "Need pickup"; v[2].content = "Please come get me.";
  v[2].tel_location = true;
  return v;
}

// Close the messenger through the framework (its on_exit callback
// does the cleanup); safe no-op when another screen is active.
inline void exit_mode() {
  if (_detail::page_ref() != Page::Hidden) Display::Screens::exit_active();
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
          if (o["tel"].is<JsonObjectConst>()) {
            JsonObjectConst t = o["tel"];
            p.tel_location    = (bool)(t["location"]    | false);
            p.tel_environment = (bool)(t["environment"] | false);
            p.tel_battery     = (bool)(t["battery"]     | false);
            p.tel_compass     = (bool)(t["compass"]     | false);
            p.tel_share_s     = (uint32_t)(t["share_s"] | 0);
            p.tel_rate_s      = (uint32_t)(t["rate_s"]  | 60);
          } else {
            // Pre-telemetry schema: "gps" meant attach the position.
            p.tel_location = (bool)(o["gps"] | false);
          }
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
inline void send_selected();
inline const Display::Screens::ScreenPage& screen_page();

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
  Display::Screens::activate(&screen_page());
  notify_led(true);
}


// ---- ScreenFramework plugin handlers --------------------------------
// New gesture map (issue #3): POWER tap advances the cursor, POWER
// hold commits (pick on the list, send on the confirm page), BOOT
// hold pops one level (Confirm -> List) and exits at the root. The
// framework owns entry (BOOT tap rotation) and exit.

namespace _detail {
  // Framework entry: initialise the flow unless an out-of-band page
  // (incoming message) was set before activation.
  inline void plugin_enter() {
    if (page_ref() != Page::Hidden) return;
    cursor_ref() = 0;
    if (LXMFGateway::screen_identity() == nullptr) page_ref() = Page::NoIdentity;
    else if (usable_count() == 0)                  page_ref() = Page::NoPresets;
    else                                           page_ref() = Page::List;
  }
  inline void plugin_exit() {
    page_ref()   = Page::Hidden;
    cursor_ref() = 0;
    notify_led(false);
  }
  inline void plugin_next() {
    switch (page_ref()) {
      case Page::List: {
        const size_t n = usable_count();
        if (n == 0) page_ref() = Page::NoPresets;
        else        cursor_ref() = (cursor_ref() + 1) % n;
        break;
      }
      case Page::Confirm:
        break;   // commit is a hold; nothing to advance
      default:
        Display::Screens::exit_active();   // read-only pages: any input closes
        break;
    }
  }
  inline void plugin_select();
  inline bool plugin_back() {
    switch (page_ref()) {
      case Page::Confirm: page_ref() = Page::List; return true;
      default:            return false;   // at root: framework exits
    }
  }
}

inline void send_selected() {
  // Copy the selected preset out under the guard, then release it -
  // the send itself can take a while and must not hold the lock.
  Preset p;
  {
    _detail::Guard g;
    const auto idx = _detail::complete_indices_locked();
    if (_detail::cursor_ref() >= idx.size()) { Display::Screens::exit_active(); return; }
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

  // The preset's telemetry selection packs as a FIELD_TELEMETRY blob
  // (the Sideband convention - receivers render it natively), exactly
  // like the web compose popover. Empty when nothing it asked for is
  // available (e.g. position-only without a fix); the message still
  // sends without it. share_s > 0 also grants the recipient live
  // updates and carries the offer (window + rate).
  ExtraFields extra;
  extra.visible = true;   // user-composed via the device buttons
  const uint8_t tel_items = p.telemetry_items();
  if (tel_items != 0) {
    extra.telemetry = TelemetryShare::pack_items(a->id, tel_items);
    if (extra.telemetry.size() > 0 && p.tel_share_s > 0) {
      extra.custom_meta = TelemetryShare::pack_meta(
          p.tel_share_s, p.tel_rate_s, /*mark_message=*/false);
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
  const bool ok = LXMFGateway::send(a->id, dest, "", p.content, nullptr,
                                    rec, &err, &queued, /*use_seq=*/0,
                                    extra.empty() ? nullptr : &extra);
  // Same rule as the web send path: the live-share grant records only
  // once the send is accepted (sent or queued), and a one-shot
  // telemetry send supersedes any previous grant for this recipient.
  if ((ok || queued) && extra.telemetry.size() > 0) {
    TelemetryShare::record_grant(a->id, dest, tel_items, p.tel_share_s);
  }
  _detail::result_live_ref() = (ok || queued)
                               && extra.telemetry.size() > 0
                               && p.tel_share_s > 0;
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

// Text helpers shared with the sensors view live in Common/OledText.h.

inline void render_body(GFXcanvas1& area, int16_t y_top, int16_t y_bottom) {
  area.setFont(&Picopixel);
  area.setTextWrap(false);
  area.setTextColor(1);
  area.setTextSize(1);

  const Page pg = _detail::page_ref();
  if (pg == Page::Message) {
    const int body_rows = (y_bottom - y_top - 3) / 8;
    Common::OledText::wrap_classic(area, _detail::msg_text_ref(),
                                   (int16_t)(y_top + 3), body_rows);
    return;
  }
  if (pg == Page::NoIdentity) {
    Common::OledText::line(area, (int16_t)(y_top + 7),  "No screen identity");
    Common::OledText::line(area, (int16_t)(y_top + 14), "is set.");
    Common::OledText::line(area, (int16_t)(y_top + 25), "Enable one in the");
    Common::OledText::line(area, (int16_t)(y_top + 32), "web app.");
    return;
  }
  if (pg == Page::NoPresets) {
    Common::OledText::line(area, (int16_t)(y_top + 7),  "No messages are");
    Common::OledText::line(area, (int16_t)(y_top + 14), "set up.");
    Common::OledText::line(area, (int16_t)(y_top + 25), "Add some in the");
    Common::OledText::line(area, (int16_t)(y_top + 32), "web app.");
    return;
  }
  if (pg == Page::List) {
    _detail::Guard g;
    auto& v = _detail::presets_ref();
    const auto idx = _detail::complete_indices_locked();
    const size_t cur = _detail::cursor_ref();
    // Rows fill the body; the window scrolls to keep the cursor on
    // screen. The device must explain itself - whoever holds it has
    // no web UI in hand.
    const size_t rows  = (size_t)((y_bottom - y_top - 5) / 7);
    const size_t first = (cur >= rows) ? cur - rows + 1 : 0;
    int16_t y = (int16_t)(y_top + 7);
    for (size_t i = first; i < idx.size() && i < first + rows; ++i, y += 7) {
      if (i == cur) {
        area.drawBitmap(2, (int16_t)(y - 6), Display::Screens::GLYPH_CURSOR, 8, 8, 1);
      }
      area.setCursor(11, y);
      area.print(Common::OledText::fit(area, v[idx[i]].label, 51).c_str());
    }
    return;
  }
  if (pg == Page::Confirm) {
    _detail::Guard g;
    const auto idx = _detail::complete_indices_locked();
    if (idx.empty()) { _detail::page_ref() = Page::NoPresets; return; }
    const size_t cur = _detail::cursor_ref() < idx.size() ? _detail::cursor_ref() : 0;
    const Preset& p = _detail::presets_ref()[idx[cur]];
    Common::OledText::line(area, (int16_t)(y_top + 7),
                           Common::OledText::fit(area, p.label, 60).c_str());
    // The recipient's prefix with the person glyph, then the message
    // itself in the reading font, so the holder confirms what goes out.
    area.drawBitmap(2, (int16_t)(y_top + 10), Display::Screens::GLYPH_PERSON, 8, 8, 1);
    const std::string to = p.dest_hex.substr(0, 8);
    Common::OledText::line(area, (int16_t)(y_top + 17), ("  " + to).c_str());
    const int16_t body_y = (int16_t)(y_top + 22);
    const int body_rows = (y_bottom - body_y) / 8;
    if (body_rows > 0) Common::OledText::wrap_classic(area, p.content, body_y, body_rows);
    return;
  }
  if (pg == Page::Result) {
    // Status glyph left of the wrapped status text, keyed off the
    // same string on_outbox_status updates.
    const std::string& r = _detail::result_ref();
    const uint8_t* g = Display::Screens::GLYPH_CLOCK;
    if      (r == "Sent")      g = Display::Screens::GLYPH_CHECK;
    else if (r == "Delivered") g = Display::Screens::GLYPH_CHECK2;
    else if (r == "Failed")    g = Display::Screens::GLYPH_CROSS;
    area.drawBitmap(2, (int16_t)(y_top + 4), g, 8, 8, 1);
    const int body_rows = (y_bottom - y_top - 3) / 8;
    Common::OledText::wrap_classic(area, r, (int16_t)(y_top + 3),
                                   body_rows > 0 ? body_rows : 1);
    return;
  }
}

namespace _detail {
  inline void plugin_header(const uint8_t** glyph, const char** title) {
    switch (page_ref()) {
      case Page::Confirm:
        *glyph = Display::Screens::GLYPH_SEND;  *title = "SEND";   break;
      case Page::Result:
        *glyph = Display::Screens::GLYPH_SEND;  *title = "STATUS"; break;
      case Page::Message: {
        // Sender as the title, uppercased per the chrome rules.
        static char buf[14];
        const std::string& f = msg_from_ref();
        size_t n = 0;
        for (; n < sizeof(buf) - 1 && n < f.size(); ++n) buf[n] = (char)toupper((unsigned char)f[n]);
        buf[n] = 0;
        *glyph = Display::Screens::GLYPH_INBOX; *title = buf;       break;
      }
      default:
        *glyph = Display::Screens::GLYPH_ENVELOPE; *title = "MESSAGES"; break;
    }
  }
  inline size_t plugin_hints(const char** out, size_t max) {
    switch (page_ref()) {
      case Page::List:
        if (max < 3) return 0;
        out[0] = "Tap POWER: next";
        out[1] = "Hold POWER: pick";
        out[2] = "Hold BOOT: back";
        return 3;
      case Page::Confirm:
        if (max < 2) return 0;
        out[0] = "Hold POWER: send";
        out[1] = "Hold BOOT: back";
        return 2;
      default:
        if (max < 1) return 0;
        out[0] = "Tap ANY: close";
        return 1;
    }
  }
  // The result page after a live-granted send keeps updating: spinner.
  inline bool plugin_live() {
    return page_ref() == Page::Result && result_live_ref();
  }
  inline void plugin_select() {
    switch (page_ref()) {
      case Page::List:    page_ref() = Page::Confirm; break;
      case Page::Confirm: send_selected();            break;
      default:            Display::Screens::exit_active(); break;
    }
  }
}

// The registered framework page. Body + handlers above; chrome and
// navigation are the framework's.
inline const Display::Screens::ScreenPage MESSENGER_PAGE = {
  _detail::plugin_header, _detail::plugin_hints, render_body,
  _detail::plugin_enter, _detail::plugin_exit,
  _detail::plugin_next, _detail::plugin_select, _detail::plugin_back,
  _detail::plugin_live, /*ttl_ms=*/0,
};
inline const Display::Screens::ScreenPage& screen_page() { return MESSENGER_PAGE; }

}  // namespace Messenger
}  // namespace LXMF
