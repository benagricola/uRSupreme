#pragma once

#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <Log.h>
#include <microStore/FileSystem.h>

#include "../LXMF/LXMFTypes.h"

#include <Cryptography/Random.h>
#include <Bytes.h>

#include <string>
#include <vector>
#include <stdint.h>

extern microStore::FileSystem filesystem;

namespace Web {

  // Bearer-token store for browser sessions. One token = one bound identity.
  // Tokens are 16 random bytes hex-encoded (32 chars). Persisted to
  // /lxmf/auth_tokens.json so logins survive a reboot.
  //
  // Issuance happens after a successful button-press unlock (see Web/WebUI
  // and the button handler in RNode_Firmware.ino). Validation is a linear
  // scan of the in-RAM cache (capped tight, so the cost is bounded).
  class AuthTokens {
  public:
    static constexpr const char* STORE_PATH      = "/lxmf/auth_tokens.json";
    static constexpr size_t      TOKEN_BYTES     = 16;
    static constexpr size_t      TOKEN_HEX_LEN   = 32;
    static constexpr uint32_t    DEFAULT_TTL_S   = 30 * 24 * 60 * 60;  // 30 days inactivity
    // Total active tokens cap. Bumped from 16 to 64 — bench testing
    // with the WS migration uncovered the 4-tokens-per-identity cap
    // evicting parallel test sessions. The headline cap defends
    // against unbounded growth from a malicious / buggy client; the
    // per-identity cap is gone since legitimate use (multiple browser
    // tabs across phones, laptops, etc.) routinely needs more than 4.
    static constexpr size_t      MAX_TOKENS      = 64;

    struct Token {
      std::string       hex;
      LXMF::IdentityId   identity_id;
      uint32_t          created_ms;
      uint32_t          last_seen_ms;
    };

    static void load() {
      _tokens().clear();
      if (!filesystem.exists(STORE_PATH)) return;
      std::vector<uint8_t> data;
      if (filesystem.readFile(STORE_PATH, data) == 0) return;
      Common::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      if (!doc.is<JsonArray>()) return;
      for (JsonObject obj : doc.as<JsonArray>()) {
        Token t;
        t.hex          = (const char*)(obj["hex"]        | "");
        t.identity_id   = (const char*)(obj["identity_id"] | "");
        t.created_ms   = (uint32_t)(obj["created_ms"]    | 0);
        t.last_seen_ms = (uint32_t)(obj["last_seen_ms"]  | 0);
        if (t.hex.size() == TOKEN_HEX_LEN && !t.identity_id.empty()) {
          _tokens().push_back(t);
        }
      }
      NOTICEF("AuthTokens: loaded %u tokens", (unsigned)_tokens().size());
    }

    static void save() {
      Common::PsramJsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      for (const auto& t : _tokens()) {
        JsonObject obj = arr.add<JsonObject>();
        obj["hex"]          = t.hex;
        obj["identity_id"]   = t.identity_id;
        obj["created_ms"]   = t.created_ms;
        obj["last_seen_ms"] = t.last_seen_ms;
      }
      // Serialise directly into an exact-sized heap buffer rather than a
      // growable Arduino String. Save() runs on every successful login
      // and every token-cleanup pass; at MAX_TOKENS=16 the JSON is ~2 KB,
      // and the previous String-grow-by-doubling pattern allocated up
      // to 4 KiB transient (final 2 KiB + intermediate 2 KiB during the
      // last realloc) on the default heap — exactly the size range the
      // ESP-IDF WiFi driver needs for its esf_buf TX envelopes (#173).
      // measureJson + one-shot alloc removes the realloc churn.
      const size_t n = measureJson(doc);
      if (n == 0) {
        WARNING("AuthTokens: save measure returned 0");
        return;
      }
      auto buf = std::make_unique<uint8_t[]>(n);
      if (!buf) {
        WARNING("AuthTokens: save buffer alloc failed");
        return;
      }
      const size_t written = serializeJson(doc, buf.get(), n);
      if (written == 0) {
        WARNING("AuthTokens: save serialization failed");
        return;
      }
      filesystem.writeFile(STORE_PATH, buf.get(), written);
    }

    // Issue a fresh token for an identity. Returns the hex token. The
    // total-tokens cap evicts the globally-oldest entry, regardless of
    // identity — fair share is enforced implicitly by recency, not by
    // a per-identity quota. (The old 4-per-identity quota was breaking
    // multi-tab / multi-device workflows.)
    static std::string issue(const LXMF::IdentityId& identity_id) {
      if (identity_id.empty()) return {};
      auto& store = _tokens();

      // Cap total — evict the oldest entry by last_seen_ms.
      while (store.size() >= MAX_TOKENS) {
        auto oldest = store.begin();
        for (auto it = store.begin(); it != store.end(); ++it) {
          if (it->last_seen_ms < oldest->last_seen_ms) oldest = it;
        }
        store.erase(oldest);
      }

      Token t;
      t.hex          = random_hex();
      t.identity_id   = identity_id;
      t.created_ms   = millis();
      t.last_seen_ms = t.created_ms;
      store.push_back(t);
      save();
      NOTICEF("AuthTokens: issued token for identity %s", identity_id.c_str());
      return t.hex;
    }

    // Look up the identity bound to this token, refreshing last_seen.
    // Returns empty string on unknown / expired token.
    static LXMF::IdentityId validate(const std::string& hex) {
      if (hex.size() != TOKEN_HEX_LEN) return {};
      uint32_t now = millis();
      for (auto& t : _tokens()) {
        if (t.hex == hex) {
          // Inactivity expiry (note: millis() wraps at ~49 days; we only
          // care about a coarse "weeks ago" check, so do it in seconds and
          // tolerate wrap by treating wrap as "recently active").
          uint32_t inactive_ms = now - t.last_seen_ms;
          if (inactive_ms > DEFAULT_TTL_S * 1000UL && now > t.last_seen_ms) {
            revoke(t.hex);
            return {};
          }
          t.last_seen_ms = now;
          return t.identity_id;
        }
      }
      return {};
    }

    static bool revoke(const std::string& hex) {
      auto& store = _tokens();
      for (auto it = store.begin(); it != store.end(); ++it) {
        if (it->hex == hex) {
          store.erase(it);
          save();
          return true;
        }
      }
      return false;
    }

    static void revoke_for_identity(const LXMF::IdentityId& identity_id) {
      auto& store = _tokens();
      bool removed = false;
      for (auto it = store.begin(); it != store.end(); ) {
        if (it->identity_id == identity_id) { it = store.erase(it); removed = true; }
        else ++it;
      }
      if (removed) save();
    }

    // Drop any token whose last_seen_ms is older than DEFAULT_TTL_S.
    // Called periodically from web_ui_loop.
    static void sweep_expired() {
      uint32_t now = millis();
      auto& store = _tokens();
      bool removed = false;
      for (auto it = store.begin(); it != store.end(); ) {
        uint32_t inactive_ms = now - it->last_seen_ms;
        if (inactive_ms > DEFAULT_TTL_S * 1000UL && now > it->last_seen_ms) {
          it = store.erase(it); removed = true;
        } else ++it;
      }
      if (removed) save();
    }

  private:
    static std::vector<Token>& _tokens() {
      static std::vector<Token> v;
      return v;
    }

    static std::string random_hex() {
      RNS::Bytes b = RNS::Cryptography::random(TOKEN_BYTES);
      return b.toHex();
    }
  };

}
