// Global default retention applied to NEW chats. Persisted at
// /lxmf/inbox_config.json.
//
// The default is snapshotted onto each peer the first time we see
// a message from / to them (LXMFInbox::ensure_peer_retention).
// Changing the default later DOES NOT touch existing chats — that's
// the whole point of "default for new chats" semantics. Users edit
// individual chats via the per-conversation retention modal.

#pragma once

#include <stdint.h>
#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <microStore/FileSystem.h>

#include "LXMFInbox.h"
#include "LXMFTypes.h"

extern microStore::FileSystem filesystem;

namespace LXMF {
namespace InboxConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/inbox_config.json";

struct Config {
  Retention default_retention;   // Kind::None by default = keep forever
};

namespace _detail {
  inline Config& current_ref() { static Config c; return c; }
}

inline const Config& current() { return _detail::current_ref(); }

inline void load(microStore::FileSystem& fs) {
  Config& c = _detail::current_ref();
  c = Config{};
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  JsonObjectConst r = doc["default_retention"].as<JsonObjectConst>();
  if (!r.isNull()) {
    c.default_retention.kind  = retention_kind_from_str(r["kind"] | "none");
    c.default_retention.value = (uint32_t)(r["value"] | 0);
  }
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = _detail::current_ref();
  Common::PsramJsonDocument doc;
  JsonObject r = doc["default_retention"].to<JsonObject>();
  r["kind"]  = retention_kind_name(c.default_retention.kind);
  r["value"] = c.default_retention.value;
  std::string out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

inline void set_default_retention(microStore::FileSystem& fs, Retention r) {
  _detail::current_ref().default_retention = r;
  persist(fs);
}

}  // namespace InboxConfig
}  // namespace LXMF
