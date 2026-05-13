#pragma once

#include <ArduinoJson.h>
#include <Log.h>
#include <Identity.h>
#include <microStore/FileSystem.h>

#include <memory>
#include <string>
#include <vector>
#include <stdint.h>

#include "LXMFTypes.h"
#include "LXMFMinimal.h"
#include "LXMFInbox.h"

extern microStore::FileSystem filesystem;

namespace LXMF {

  #ifndef LXMF_GATEWAY_MAX_ACCOUNTS
  #define LXMF_GATEWAY_MAX_ACCOUNTS 4
  #endif

  #ifndef LXMF_GATEWAY_ROOT
  #define LXMF_GATEWAY_ROOT "/lxmf"
  #endif

  #ifndef LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS
  #define LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS 300000  // 5 minutes
  #endif

  // One per-account record. Held in a static array so the per-instance
  // pointers in LXMFMinimal's dispatch registry stay stable for the
  // lifetime of the gateway.
  struct LXMFAccount {
    AccountId            id;                    // first 16 hex of identity.hash
    std::string          display_name;
    RNS::Identity        identity{RNS::Type::NONE};
    LXMFMinimal          lxmf;
    std::unique_ptr<LXMFInbox> inbox;
    std::unique_ptr<LXMFInbox> outbox;
    uint32_t             last_announce_ms = 0;
    uint32_t             announce_interval_ms = LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS;
    bool                 active = false;

    std::string dir() const { return std::string(LXMF_GATEWAY_ROOT "/accounts/") + id; }
    std::string identity_path() const { return dir() + "/identity.dat"; }
    std::string meta_path()     const { return dir() + "/meta.json"; }
    std::string address_hex()   const { return lxmf.address_hex(); }
  };

  class LXMFGateway {
  public:
    // Load existing accounts from disk and register them with the
    // Reticulum transport. Idempotent if already called.
    static void setup() {
      if (_setup_done) return;
      ensure_root();
      load_existing_accounts();
      _setup_done = true;
    }

    // Called from main loop. Drives periodic announces.
    static void loop() {
      if (!_setup_done) return;
      uint32_t now = millis();
      for (auto& a : accounts_storage()) {
        if (!a.active) continue;
        if (now - a.last_announce_ms >= a.announce_interval_ms) {
          a.lxmf.announce();
          a.last_announce_ms = now;
        }
      }
    }

    // Generate a fresh identity, persist it, instantiate the account.
    // Returns the new account_id, or empty string on failure / cap.
    static AccountId create_account(const std::string& display_name) {
      if (!_setup_done) {
        WARNING("LXMFGateway::create_account: setup() not called yet");
        return {};
      }
      LXMFAccount* slot = first_free_slot();
      if (!slot) {
        WARNING("LXMFGateway::create_account: account cap reached");
        return {};
      }
      RNS::Identity id;  // fresh keypair
      const std::string id_hex = id.hexhash();
      if (id_hex.empty()) {
        ERROR("LXMFGateway::create_account: identity hash empty");
        return {};
      }
      AccountId acc_id = id_hex.substr(0, 16);

      slot->id           = acc_id;
      slot->display_name = display_name;
      slot->identity     = id;

      ensure_account_dir(*slot);
      if (!id.to_file(slot->identity_path().c_str())) {
        ERRORF("LXMFGateway::create_account: failed to persist identity at %s",
               slot->identity_path().c_str());
        slot->active = false;
        return {};
      }
      write_meta(*slot);

      activate(*slot);
      NOTICEF("LXMFGateway: created account %s (%s) → %s",
              acc_id.c_str(), display_name.c_str(), slot->address_hex().c_str());
      return acc_id;
    }

    // Tear down and remove an account from disk.
    static bool delete_account(const AccountId& acc_id) {
      LXMFAccount* a = account_by_id_mut(acc_id);
      if (!a) return false;
      a->lxmf.shutdown();
      // Best-effort file removal.
      filesystem.remove(a->identity_path().c_str());
      filesystem.remove(a->meta_path().c_str());
      filesystem.remove((a->dir() + "/inbox.jsonl").c_str());
      filesystem.remove((a->dir() + "/outbox.jsonl").c_str());
      a->inbox.reset();
      a->outbox.reset();
      a->identity = RNS::Identity(RNS::Type::NONE);
      a->id.clear();
      a->display_name.clear();
      a->active = false;
      NOTICEF("LXMFGateway: deleted account %s", acc_id.c_str());
      return true;
    }

    static const LXMFAccount* account_by_id(const AccountId& acc_id) {
      return account_by_id_mut(acc_id);
    }

    // Force an immediate announce.
    static bool announce(const AccountId& acc_id) {
      LXMFAccount* a = account_by_id_mut(acc_id);
      if (!a) return false;
      a->lxmf.announce();
      a->last_announce_ms = millis();
      return true;
    }

    // Send an LXMF message from this account. Appends to outbox on success.
    static bool send(const AccountId& acc_id,
                     const RNS::Bytes& dest_hash,
                     const std::string& title,
                     const std::string& content,
                     MessageRecord& out_rec) {
      LXMFAccount* a = account_by_id_mut(acc_id);
      if (!a) return false;
      if (!a->lxmf.send_message(dest_hash, title, content, out_rec)) {
        return false;
      }
      if (a->outbox) a->outbox->append(out_rec);
      return true;
    }

    // Read-only access to accounts list.
    static const std::vector<LXMFAccount*>& active_accounts() {
      _active_view.clear();
      for (auto& a : accounts_storage()) if (a.active) _active_view.push_back(&a);
      return _active_view;
    }

    static size_t account_count() {
      size_t n = 0;
      for (auto& a : accounts_storage()) if (a.active) ++n;
      return n;
    }

  private:
    static std::array<LXMFAccount, LXMF_GATEWAY_MAX_ACCOUNTS>& accounts_storage() {
      static std::array<LXMFAccount, LXMF_GATEWAY_MAX_ACCOUNTS> s;
      return s;
    }

    static LXMFAccount* first_free_slot() {
      for (auto& a : accounts_storage()) if (!a.active) return &a;
      return nullptr;
    }

    static LXMFAccount* account_by_id_mut(const AccountId& acc_id) {
      if (acc_id.empty()) return nullptr;
      for (auto& a : accounts_storage()) if (a.active && a.id == acc_id) return &a;
      return nullptr;
    }

    static void ensure_root() {
      if (!filesystem.exists(LXMF_GATEWAY_ROOT)) filesystem.mkdir(LXMF_GATEWAY_ROOT);
      const char* accts = LXMF_GATEWAY_ROOT "/accounts";
      if (!filesystem.exists(accts)) filesystem.mkdir(accts);
    }

    static void ensure_account_dir(const LXMFAccount& a) {
      const std::string d = a.dir();
      if (!filesystem.exists(d.c_str())) filesystem.mkdir(d.c_str());
    }

    static void write_meta(const LXMFAccount& a) {
      JsonDocument doc;
      doc["display_name"]         = a.display_name;
      doc["created_ms"]           = (uint32_t)millis();
      doc["announce_interval_ms"] = a.announce_interval_ms;
      char buf[256];
      size_t n = serializeJson(doc, buf, sizeof(buf));
      if (n > 0 && n < sizeof(buf)) {
        filesystem.writeFile(a.meta_path().c_str(), reinterpret_cast<const uint8_t*>(buf), n);
      }
    }

    static void read_meta(LXMFAccount& a) {
      if (!filesystem.exists(a.meta_path().c_str())) return;
      std::vector<uint8_t> data;
      if (filesystem.readFile(a.meta_path().c_str(), data) == 0) return;
      JsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      a.display_name         = (const char*)(doc["display_name"] | "LXMF Account");
      a.announce_interval_ms = (uint32_t)(doc["announce_interval_ms"] | LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS);
    }

    static void activate(LXMFAccount& a) {
      a.active = true;
      a.inbox  = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "inbox.jsonl"));
      a.outbox = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "outbox.jsonl"));
      a.inbox->load();
      a.outbox->load();

      a.lxmf.init(a.identity, a.display_name.c_str());
      LXMFAccount* p = &a;
      a.lxmf.set_delivery_callback([p](const MessageRecord& rec) {
        if (!p->active || !p->inbox) return;
        MessageRecord local = rec;  // copy so we can mutate seq
        local.seq = 0;
        p->inbox->append(local);
      });
      a.last_announce_ms = 0;  // announce on first loop tick
    }

    static void load_existing_accounts() {
      const char* accts = LXMF_GATEWAY_ROOT "/accounts";
      if (!filesystem.exists(accts)) return;
      auto entries = filesystem.listDirectory(accts);
      size_t loaded = 0;
      for (const auto& entry : entries) {
        if (loaded >= LXMF_GATEWAY_MAX_ACCOUNTS) {
          WARNINGF("LXMFGateway: skipping %s (max accounts reached)", entry.c_str());
          continue;
        }
        std::string full_path = std::string(accts) + "/" + entry;
        if (!filesystem.isDirectory(full_path.c_str())) continue;

        LXMFAccount* slot = first_free_slot();
        if (!slot) break;

        std::string identity_path = full_path + "/identity.dat";
        if (!filesystem.exists(identity_path.c_str())) {
          WARNINGF("LXMFGateway: account %s missing identity.dat, skipping", entry.c_str());
          continue;
        }
        RNS::Identity id = RNS::Identity::from_file(identity_path.c_str());
        if (!id) {
          WARNINGF("LXMFGateway: failed to load identity for account %s", entry.c_str());
          continue;
        }
        slot->id           = entry;
        slot->identity     = id;
        slot->display_name = "LXMF Account";  // overridden by meta if present
        read_meta(*slot);
        activate(*slot);
        loaded++;
        NOTICEF("LXMFGateway: loaded account %s (%s) → %s",
                slot->id.c_str(), slot->display_name.c_str(), slot->address_hex().c_str());
      }
    }

  private:
    static inline bool _setup_done = false;
    static inline std::vector<LXMFAccount*> _active_view;
  };

} // namespace LXMF
