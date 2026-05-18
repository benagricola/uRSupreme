// Device-wide per-direction transfer caps, persisted at
// /lxmf/storage_config.json.
//
// Two user-facing settings replace seven overlapping internal caps:
//
//   user_max_send_bytes    largest outbound message the device will
//                          accept from the SPA for sending.
//   user_max_receive_bytes largest inbound Resource the device will
//                          accept from a peer.
//
// At enforcement time the user setting is clamped to whichever is
// smaller: the current backing-store free space (PSRAM / Flash / SD,
// whichever the staging or receive layer picks) and the raw RNS
// protocol ceiling. So the user's preference can never violate
// physical capacity by construction.
//
// The seven legacy constants in OutboundStaging.h + Type.h keep their
// roles as *inputs* to the resolver — they constrain which backend a
// transfer uses, not whether the transfer is allowed.
//
// SD removed mid-session: the saved user_max_* values are preserved
// (restored when SD is re-inserted); the effective_max_* values drop
// to the flash-safe cap until the card returns.

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <algorithm>
#include <ArduinoJson.h>
#include <microStore/FileSystem.h>

#include "OutboundStaging.h"
#include "SDCard.h"

extern microStore::FileSystem filesystem;

namespace Web {
namespace Storage {

inline constexpr const char* CONFIG_PATH = "/lxmf/storage_config.json";

// User-facing default is intentionally generous (256 MiB) so a fresh
// install gets the full available capacity without the user having
// to open Settings. The effective value clamps down to physical
// reality on each enforcement call.
inline constexpr size_t DEFAULT_USER_MAX = (size_t)256 * 1024 * 1024;

// Raw RNS protocol ceiling. Lifted from the old 1 MiB FIRMWARE_MAX
// to match the new SD-capable scope; the user setting is what
// actually constrains transfers in practice.
inline constexpr size_t PROTOCOL_CEILING = (size_t)256 * 1024 * 1024;

// Safety margin reserved on the receive backing store so a single
// large incoming transfer can't fill it completely (leaves room for
// inbox JSONL writes, attachment commits, etc).
inline constexpr size_t RECEIVE_SAFETY_MARGIN = (size_t)256 * 1024;

struct Config {
  size_t user_max_send_bytes    = DEFAULT_USER_MAX;
  size_t user_max_receive_bytes = DEFAULT_USER_MAX;
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
  JsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  if (doc["user_max_send_bytes"].is<uint32_t>()) {
    c.user_max_send_bytes = (size_t)doc["user_max_send_bytes"].as<uint32_t>();
  }
  if (doc["user_max_receive_bytes"].is<uint32_t>()) {
    c.user_max_receive_bytes = (size_t)doc["user_max_receive_bytes"].as<uint32_t>();
  }
  if (c.user_max_send_bytes    == 0) c.user_max_send_bytes    = DEFAULT_USER_MAX;
  if (c.user_max_receive_bytes == 0) c.user_max_receive_bytes = DEFAULT_USER_MAX;
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = _detail::current_ref();
  JsonDocument doc;
  doc["user_max_send_bytes"]    = (uint32_t)std::min<size_t>(c.user_max_send_bytes,    0xFFFFFFFFu);
  doc["user_max_receive_bytes"] = (uint32_t)std::min<size_t>(c.user_max_receive_bytes, 0xFFFFFFFFu);
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

inline void set(microStore::FileSystem& fs, size_t user_max_send, size_t user_max_receive) {
  Config& c = _detail::current_ref();
  c.user_max_send_bytes    = (user_max_send    == 0) ? DEFAULT_USER_MAX : user_max_send;
  c.user_max_receive_bytes = (user_max_receive == 0) ? DEFAULT_USER_MAX : user_max_receive;
  persist(fs);
}

// Current send-direction backing capacity, computed from OutboundStaging.
// This is the largest single outbound message the device can stage right
// now given SD presence, PSRAM free, and flash slack.
inline size_t backing_max_send() {
  return OutboundStaging::current_caps().max_bytes;
}

// Current receive-direction backing capacity. Receive payloads >
// RAM_BUFFER_THRESHOLD spill into a temp file on SD (if mounted) or
// Flash. We size the cap to the chosen backing store's free space
// minus a safety margin.
inline size_t backing_max_receive() {
  if (Web::SDCard::present()) {
    const uint64_t total = Web::SDCard::total_bytes();
    const uint64_t used  = Web::SDCard::used_bytes();
    const size_t free_b  = (total > used) ? (size_t)(total - used) : 0;
    return (free_b > RECEIVE_SAFETY_MARGIN) ? (free_b - RECEIVE_SAFETY_MARGIN) : 0;
  }
  const size_t flash_free = (size_t)filesystem.storageAvailable();
  return (flash_free > RECEIVE_SAFETY_MARGIN) ? (flash_free - RECEIVE_SAFETY_MARGIN) : 0;
}

inline size_t effective_max_send() {
  const Config& c = _detail::current_ref();
  return std::min({c.user_max_send_bytes, backing_max_send(), PROTOCOL_CEILING});
}

inline size_t effective_max_receive() {
  const Config& c = _detail::current_ref();
  return std::min({c.user_max_receive_bytes, backing_max_receive(), PROTOCOL_CEILING});
}

}  // namespace Storage
}  // namespace Web
