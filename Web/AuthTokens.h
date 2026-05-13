#pragma once

#include <ArduinoJson.h>
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

  // Bearer-token store for browser sessions. One token = one bound account.
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
    static constexpr size_t      MAX_TOKENS      = 16;
    static constexpr size_t      MAX_PER_ACCOUNT = 4;

    struct Token {
      std::string       hex;
      LXMF::AccountId   account_id;
      uint32_t          created_ms;
      uint32_t          last_seen_ms;
    };

    static void load() {
      _tokens().clear();
      if (!filesystem.exists(STORE_PATH)) return;
      std::vector<uint8_t> data;
      if (filesystem.readFile(STORE_PATH, data) == 0) return;
      JsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      if (!doc.is<JsonArray>()) return;
      for (JsonObject obj : doc.as<JsonArray>()) {
        Token t;
        t.hex          = (const char*)(obj["hex"]        | "");
        t.account_id   = (const char*)(obj["account_id"] | "");
        t.created_ms   = (uint32_t)(obj["created_ms"]    | 0);
        t.last_seen_ms = (uint32_t)(obj["last_seen_ms"]  | 0);
        if (t.hex.size() == TOKEN_HEX_LEN && !t.account_id.empty()) {
          _tokens().push_back(t);
        }
      }
      NOTICEF("AuthTokens: loaded %u tokens", (unsigned)_tokens().size());
    }

    static void save() {
      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      for (const auto& t : _tokens()) {
        JsonObject obj = arr.add<JsonObject>();
        obj["hex"]          = t.hex;
        obj["account_id"]   = t.account_id;
        obj["created_ms"]   = t.created_ms;
        obj["last_seen_ms"] = t.last_seen_ms;
      }
      // Use a growable String — at MAX_TOKENS=16 the JSON runs ~2 KB, well
      // past any reasonable stack-buffer choice.
      String body;
      if (serializeJson(doc, body) == 0) {
        WARNING("AuthTokens: save serialization failed");
        return;
      }
      filesystem.writeFile(STORE_PATH,
                           reinterpret_cast<const uint8_t*>(body.c_str()),
                           body.length());
    }

    // Issue a fresh token for an account. Evicts the oldest token for that
    // account if it already has MAX_PER_ACCOUNT. Returns the hex token.
    static std::string issue(const LXMF::AccountId& account_id) {
      if (account_id.empty()) return {};
      auto& store = _tokens();

      // Evict oldest for this account if at cap.
      size_t per_acct = 0;
      Token* oldest = nullptr;
      for (auto& t : store) {
        if (t.account_id == account_id) {
          per_acct++;
          if (!oldest || t.last_seen_ms < oldest->last_seen_ms) oldest = &t;
        }
      }
      if (per_acct >= MAX_PER_ACCOUNT && oldest) {
        revoke(oldest->hex);
      }
      // Cap total
      while (store.size() >= MAX_TOKENS) store.erase(store.begin());

      Token t;
      t.hex          = random_hex();
      t.account_id   = account_id;
      t.created_ms   = millis();
      t.last_seen_ms = t.created_ms;
      store.push_back(t);
      save();
      NOTICEF("AuthTokens: issued token for account %s", account_id.c_str());
      return t.hex;
    }

    // Look up the account bound to this token, refreshing last_seen.
    // Returns empty string on unknown / expired token.
    static LXMF::AccountId validate(const std::string& hex) {
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
          return t.account_id;
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

    static void revoke_for_account(const LXMF::AccountId& account_id) {
      auto& store = _tokens();
      bool removed = false;
      for (auto it = store.begin(); it != store.end(); ) {
        if (it->account_id == account_id) { it = store.erase(it); removed = true; }
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
