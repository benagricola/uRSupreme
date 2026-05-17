// Global inbox capacity + TTL config, persisted at /lxmf/inbox_config.json.
//
// One config applies to every identity's inbox + outbox. Loaded once
// at LXMFGateway::activate() time (so it lands before the inboxes
// replay their JSONL spools, ensuring on-boot prune of any over-cap
// or expired entries). The /api/inbox_config endpoint POSTs an
// updated config and immediately re-applies it across every active
// identity's inbox and outbox. (#129)

#pragma once

#include <stdint.h>
#include <ArduinoJson.h>
#include <microStore/FileSystem.h>

#include "LXMFInbox.h"

extern microStore::FileSystem filesystem;

namespace LXMF {
namespace InboxConfig {

inline constexpr const char* CONFIG_PATH = "/lxmf/inbox_config.json";

struct Config {
  size_t   ram_capacity = LXMFInbox::DEFAULT_RAM_CAPACITY;   // SIZE_MAX = unlimited
  uint32_t ttl_seconds  = 0;                                  // 0 = TTL off
};

namespace _detail {
  inline Config& current_ref() { static Config c; return c; }
}

inline const Config& current() { return _detail::current_ref(); }

inline void load(microStore::FileSystem& fs) {
  Config& c = _detail::current_ref();
  c = Config{};   // reset to defaults
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  // ram_capacity: a SIZE_MAX sentinel encodes as a large JSON number.
  // Treat anything ≥ 0xFFFFFFFE as "unlimited" so we never accidentally
  // truncate it on round-trip via SPA -> server -> JSON.
  if (doc["ram_capacity"].is<uint32_t>()) {
    uint32_t v = doc["ram_capacity"].as<uint32_t>();
    c.ram_capacity = (v >= 0xFFFFFFFEu) ? LXMFInbox::UNLIMITED_CAPACITY : (size_t)v;
  }
  if (doc["ttl_seconds"].is<uint32_t>()) {
    c.ttl_seconds = doc["ttl_seconds"].as<uint32_t>();
  }
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = _detail::current_ref();
  JsonDocument doc;
  doc["ram_capacity"] = (c.ram_capacity >= 0xFFFFFFFEu)
      ? (uint32_t)0xFFFFFFFFu : (uint32_t)c.ram_capacity;
  doc["ttl_seconds"]  = c.ttl_seconds;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// Apply incoming { ram_capacity, ttl_seconds } and persist. ram_capacity
// of 0 is treated as "unlimited" since "no inbox at all" isn't useful.
inline void set(microStore::FileSystem& fs, uint32_t ram_capacity, uint32_t ttl_seconds) {
  Config& c = _detail::current_ref();
  c.ram_capacity = (ram_capacity == 0 || ram_capacity >= 0xFFFFFFFEu)
      ? LXMFInbox::UNLIMITED_CAPACITY : (size_t)ram_capacity;
  c.ttl_seconds  = ttl_seconds;
  persist(fs);
}

}  // namespace InboxConfig
}  // namespace LXMF
