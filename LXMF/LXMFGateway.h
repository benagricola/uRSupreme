#pragma once

#include <ArduinoJson.h>
#include "../Common/PsramAllocator.h"
#include <Log.h>
#include <Identity.h>
#include <Utilities/OS.h>
#include <microStore/FileSystem.h>
#include "../Storage/SDCard.h"
#include "../Storage/OutboundStaging.h"
#include "../Storage/Streaming.h"
#include "../Common/Status.h"   // OLED marquee for in-flight RX progress
#include <SD.h>

#include <memory>
#include <string>
#include <vector>
#include <stdint.h>

#include "LXMFTypes.h"
#include "InboxConfig.h"
#include "LXMFMinimal.h"
#include "RatchetStore.h"
#include "LXMFInbox.h"

extern microStore::FileSystem filesystem;

// Forward declaration of the WebUI's progress-publish hook. Defined inline
// in Web/WebUI.h (which #includes us, creating a header cycle the other
// direction — so we can't pull it in here). Linker resolves the inline
// definition once any TU that included WebUI.h has been seen.
namespace RNS { class Bytes; }
namespace LXMF { struct MessageRecord; }
namespace Web {
  void publish_lxmf_progress(const LXMF::IdentityId& identity_id,
                             const RNS::Bytes& peer_hash,
                             const RNS::Bytes& link_hash,
                             bool incoming,
                             uint32_t bytes_done,
                             uint32_t bytes_total,
                             bool finished);
  namespace WS {
    void publish_incoming(const LXMF::IdentityId& identity_id,
                          const LXMF::MessageRecord& m);
    void publish_outbound(const LXMF::IdentityId& identity_id,
                          const LXMF::MessageRecord& m);
    void publish_outbox_status(const LXMF::IdentityId& identity_id,
                               const RNS::Bytes& link_hash,
                               const char* status_name);
  }
}

namespace LXMF {

  #ifndef LXMF_GATEWAY_MAX_IDENTITIES
  #define LXMF_GATEWAY_MAX_IDENTITIES 4
  #endif

  #ifndef LXMF_GATEWAY_ROOT
  #define LXMF_GATEWAY_ROOT "/lxmf"
  #endif

  #ifndef LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS
  #define LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS 300000  // 5 minutes
  #endif

  // One per-identity record. Held in a static array so the per-instance
  // pointers in LXMFMinimal's dispatch registry stay stable for the
  // lifetime of the gateway.
  struct LXMFIdentity {
    IdentityId            id;                    // first 16 hex of identity.hash
    std::string          display_name;
    RNS::Identity        identity{RNS::Type::NONE};
    LXMFMinimal          lxmf;
    std::unique_ptr<LXMFInbox> inbox;
    std::unique_ptr<LXMFInbox> outbox;
    uint32_t             last_announce_ms = 0;
    uint32_t             announce_interval_ms = LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS;
    // When true, attachments sent from this identity get a copy
    // persisted on the device so the sender's chat history can
    // render the image / play the audio inline. When false, the
    // outbox shows a chip-only "attachment (12 KB)" placeholder.
    bool                 persist_outbound_attachments = true;
    bool                 active = false;
    // Password hash (PBKDF2-HMAC-SHA256) + per-identity salt. Set at
    // identity creation; required for login. Empty if identity is from an
    // older firmware build that didn't set passwords (in which case
    // login is blocked until the identity is recreated — there is no
    // password-recovery path, by design).
    RNS::Bytes           password_hash;
    RNS::Bytes           password_salt;
    // Per-identity X25519 ratchet ring. Public half of the newest entry
    // is advertised on every announce; private halves are kept for the
    // ring window so messages encrypted to a recent ratchet pubkey can
    // still be decrypted after rotation.  See LXMF/RatchetStore.h.
    RatchetStore         ratchets;

    std::string dir() const { return std::string(LXMF_GATEWAY_ROOT "/identities/") + id; }
    std::string identity_path() const { return dir() + "/identity.dat"; }
    std::string meta_path()     const { return dir() + "/meta.json"; }
    std::string ratchet_path()  const { return dir() + "/ratchet.dat"; }
    std::string conversation_config_path() const { return dir() + "/conversation_config.json"; }
    std::string address_hex()   const { return lxmf.address_hex(); }
  };

  class LXMFGateway {
  public:
    // Load existing identities from disk and register them with the
    // Reticulum transport. Idempotent if already called.
    static void setup() {
      if (_setup_done) return;
      ensure_root();
      load_existing_identities();
      _setup_done = true;
    }

    // Called from main loop. Drives periodic announces.
    // announce_interval_ms == 0 disables auto-announce for that identity.
    static void loop() {
      if (!_setup_done) return;
      uint32_t now = millis();
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.announce_interval_ms == 0) continue;  // auto-announce disabled
        if (now - a.last_announce_ms >= a.announce_interval_ms) {
          a.lxmf.announce();
          a.last_announce_ms = now;
        }
      }
      // Drive outbox-retry: pending_link_sends() is a shared
      // static across all LXMFMinimal instances, so one call covers
      // every identity. Entries whose next_retry_at_ms has elapsed get
      // a fresh Link and another attempt. The orphan sweep runs in
      // the same tick — catches any PendingLinkSend whose lifecycle
      // callback never fired (radio cut out mid-transfer, async event
      // loop dropped a frame, etc.) so the map can't grow unbounded.
      LXMFMinimal::tick_retries();
      LXMFMinimal::sweep_orphaned_pending();
      // Advance opportunistic (single-packet) sends: proof -> Delivered,
      // timeout -> Failed. Same shared-static cadence as the retry tick.
      LXMFMinimal::tick_opportunistic_receipts();
      // Auto-send messages whose recipient key has now arrived via the
      // path request issued at send time (AP-mode on-demand key learning).
      tick_pending_identity_sends();
    }

    // Update the auto-announce interval for a single identity and
    // persist to meta.json. interval_ms == 0 disables auto-announce
    // (manual announce via the API/UI still works).
    static bool set_announce_interval(const IdentityId& iden_id, uint32_t interval_ms) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->announce_interval_ms = interval_ms;
      a->last_announce_ms = millis();  // reset so a tiny new interval doesn't fire immediately
      write_meta(*a);
      return true;
    }

    static bool set_persist_outbound_attachments(const IdentityId& iden_id, bool on) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->persist_outbound_attachments = on;
      write_meta(*a);
      return true;
    }

    // Update the LXMF announcement label and persist. Pushes the new
    // name into the live LXMFMinimal so the next announce() carries it
    // — does NOT trigger an announce here (the caller fires one off
    // separately so peers re-learn the label).
    static bool set_display_name(const IdentityId& iden_id, const std::string& name) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->display_name = name;
      a->lxmf.set_display_name(name.c_str());
      write_meta(*a);
      return true;
    }

    // Generate a fresh identity + password hash, persist, instantiate.
    // Returns the new identity_id, or empty string on failure / cap /
    // password too short.
    static IdentityId create_identity(const std::string& display_name,
                                    const std::string& password,
                                    RNS::Bytes (*hash_fn)(const std::string&, const RNS::Bytes&),
                                    RNS::Bytes (*new_salt_fn)()) {
      if (!_setup_done) {
        WARNING("LXMFGateway::create_identity: setup() not called yet");
        return {};
      }
      LXMFIdentity* slot = first_free_slot();
      if (!slot) {
        WARNING("LXMFGateway::create_identity: identity cap reached");
        return {};
      }
      // Caller checks password length; we don't trust hash_fn to do so.
      RNS::Identity id;  // fresh keypair
      const std::string id_hex = id.hexhash();
      if (id_hex.empty()) {
        ERROR("LXMFGateway::create_identity: identity hash empty");
        return {};
      }
      IdentityId iden_id = id_hex.substr(0, 16);

      slot->id           = iden_id;
      slot->display_name = display_name;
      slot->identity     = id;
      slot->password_salt = new_salt_fn();
      slot->password_hash = hash_fn(password, slot->password_salt);

      ensure_identity_dir(*slot);
      if (!id.to_file(slot->identity_path().c_str())) {
        ERRORF("LXMFGateway::create_identity: failed to persist identity at %s",
               slot->identity_path().c_str());
        slot->active = false;
        return {};
      }
      write_meta(*slot);

      activate(*slot);
      NOTICEF("LXMFGateway: created identity %s (%s) → %s",
              iden_id.c_str(), display_name.c_str(), slot->address_hex().c_str());
      return iden_id;
    }

    // Test a candidate password against the stored hash for this identity.
    // verify_fn computes the PBKDF2 hash of the candidate with the
    // identity's salt and constant-time compares against the stored hash.
    static bool check_password(const IdentityId& iden_id,
                               const std::string& candidate,
                               bool (*verify_fn)(const std::string&, const RNS::Bytes&, const RNS::Bytes&)) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      if (a->password_hash.size() == 0 || a->password_salt.size() == 0) {
        // Pre-password identity from an older firmware — refuse login;
        // user must factory-reset to recover.
        return false;
      }
      return verify_fn(candidate, a->password_salt, a->password_hash);
    }

    // Tear down and remove an identity from disk.
    static bool delete_identity(const IdentityId& iden_id) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->lxmf.shutdown();
      // Drop the on-disk attachment files before the JSONL spool. Walk
      // every loaded record and unlink each AttachmentMeta's backing
      // file — without this, the spool goes away but
      // <dir>/attachments/<filename> stays forever.
      auto unlink_atts = [&](const LXMFInbox* box) {
        if (!box) return;
        // Iterate the deque directly — no vector copy. The old
        // recent(SIZE_MAX) call duplicated the entire ring just to
        // walk it once.
        for (const auto& rec : box->ring()) {
          for (const auto& att : rec.attachments) {
            if (att.filename.empty()) continue;
            const std::string full = a->dir() + "/attachments/" + att.filename;
            if (att.backend == "sd") {
              if (Storage::SDCard::present() && Storage::SDCard::exists(full.c_str())) {
                SD.remove(full.c_str());
              }
            } else {
              if (filesystem.exists(full.c_str())) filesystem.remove(full.c_str());
            }
          }
        }
      };
      unlink_atts(a->inbox.get());
      unlink_atts(a->outbox.get());
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
      NOTICEF("LXMFGateway: deleted identity %s", iden_id.c_str());
      return true;
    }

    static const LXMFIdentity* identity_by_id(const IdentityId& iden_id) {
      return identity_by_id_mut(iden_id);
    }

    // Force an immediate announce.
    static bool announce(const IdentityId& iden_id) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->lxmf.announce();
      a->last_announce_ms = millis();
      return true;
    }

    // Send an LXMF message from this identity. Appends to outbox on success.
    // On failure, *out_err (if non-null) is set to a string literal pointing
    // to a human-readable explanation suitable for surfacing in the UI.
    static bool send(const IdentityId& iden_id,
                     const RNS::Bytes& dest_hash,
                     const std::string& title,
                     const std::string& content,
                     MessageRecord& out_rec,
                     const char** out_err = nullptr) {
      return send(iden_id, dest_hash, title, content, nullptr, out_rec, out_err);
    }

    static bool send(const IdentityId& iden_id,
                     const RNS::Bytes& dest_hash,
                     const std::string& title,
                     const std::string& content,
                     const std::vector<LXMFMinimal::OutgoingAttachment>* attachments,
                     MessageRecord& out_rec,
                     const char** out_err = nullptr) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) {
        if (out_err) *out_err = "No such identity is logged in on this device.";
        return false;
      }
      // Auto-retry on an unknown recipient key. In AP mode the LoRa
      // interface no longer floods announces, so a recipient's public key
      // is learned on demand via a path request — the first message to a
      // new contact would otherwise fail until the user resends. Queue it
      // and resend automatically once the key arrives (tick_pending_
      // identity_sends). Attachment-free only: the staged attachment
      // buffers can't be safely held across the retry window.
      if ((attachments == nullptr || attachments->empty()) &&
          !RNS::Identity::recall(dest_hash)) {
        RNS::Transport::request_path(dest_hash);
        if (queue_pending_identity_send(iden_id, dest_hash, title, content)) {
          if (out_err) *out_err = "Fetching the recipient's key. The message will send automatically when it arrives.";
        } else if (out_err) {
          *out_err = "Recipient's key is not known yet and the auto-send queue is full. Please resend in a moment.";
        }
        return false;
      }
      if (!a->lxmf.send_message(dest_hash, title, content, attachments, out_rec, out_err)) {
        return false;
      }
      if (a->outbox) {
        a->outbox->append(out_rec);
        // Broadcast the new outbound message so every connected client
        // (not just the one that issued the send, and including API-only
        // senders) shows the bubble live instead of only after a refresh.
        Web::WS::publish_outbound(iden_id, out_rec);
      }
      return true;
    }

    // Auto-retry queue for sends whose recipient key wasn't known yet
    // (see send()). Each entry is re-attempted once the key arrives via
    // the path request, then dropped. Bounded + short-lived; the message
    // text/dest are copied (large bodies land in PSRAM via the heap
    // threshold). Attachment-free by construction.
    struct PendingIdentitySend {
      IdentityId  iden_id;
      RNS::Bytes  dest;
      std::string title;
      std::string content;
      uint8_t     attempts    = 0;
      uint64_t    next_at_ms  = 0;
    };
    static constexpr uint8_t  PENDING_ID_MAX          = 8;
    static constexpr uint8_t  PENDING_ID_MAX_ATTEMPTS = 6;
    static constexpr uint32_t PENDING_ID_BACKOFF_MS   = 4000;
    static std::vector<PendingIdentitySend>& pending_identity_sends() {
      static std::vector<PendingIdentitySend> v;
      return v;
    }

    static bool queue_pending_identity_send(const IdentityId& iden,
                                            const RNS::Bytes& dest,
                                            const std::string& title,
                                            const std::string& content) {
      auto& q = pending_identity_sends();
      // Coalesce an identical re-queue (user mashing send) — refresh its
      // schedule instead of stacking duplicates.
      for (auto& p : q) {
        if (p.iden_id == iden && p.dest == dest &&
            p.title == title && p.content == content) {
          p.attempts = 0;
          p.next_at_ms = (uint64_t)millis() + PENDING_ID_BACKOFF_MS;
          return true;
        }
      }
      if (q.size() >= PENDING_ID_MAX) return false;
      PendingIdentitySend p;
      p.iden_id = iden; p.dest = dest; p.title = title; p.content = content;
      p.next_at_ms = (uint64_t)millis() + PENDING_ID_BACKOFF_MS;
      q.push_back(std::move(p));
      return true;
    }

    // Called from loop(): when a queued send's recipient key has arrived,
    // send it for real (which appends the outbox record + broadcasts it,
    // so the bubble appears). Give up after PENDING_ID_MAX_ATTEMPTS.
    static void tick_pending_identity_sends() {
      auto& q = pending_identity_sends();
      const uint64_t now = (uint64_t)millis();
      for (auto it = q.begin(); it != q.end(); ) {
        if (now < it->next_at_ms) { ++it; continue; }
        if (RNS::Identity::recall(it->dest)) {
          MessageRecord rec;
          const char* err = nullptr;
          if (send(it->iden_id, it->dest, it->title, it->content, nullptr, rec, &err)) {
            NOTICEF("LXMF: auto-sent queued message to %s once its key arrived",
                    it->dest.toHex().c_str());
          } else {
            WARNINGF("LXMF: auto-send to %s failed after key arrived: %s",
                     it->dest.toHex().c_str(), err ? err : "unknown");
          }
          it = q.erase(it);
        } else if (++it->attempts >= PENDING_ID_MAX_ATTEMPTS) {
          WARNINGF("LXMF: giving up auto-send to %s — key never arrived after %u tries",
                   it->dest.toHex().c_str(), (unsigned)it->attempts);
          it = q.erase(it);
        } else {
          RNS::Transport::request_path(it->dest);  // nudge the path request again
          it->next_at_ms = now + (uint64_t)PENDING_ID_BACKOFF_MS * it->attempts;
          ++it;
        }
      }
    }

    // Read-only access to identities list.
    static const std::vector<LXMFIdentity*>& active_identities() {
      _active_view.clear();
      for (auto& a : identities_storage()) if (a.active) _active_view.push_back(&a);
      return _active_view;
    }

    static size_t identity_count() {
      size_t n = 0;
      for (auto& a : identities_storage()) if (a.active) ++n;
      return n;
    }

    // Apply the current InboxConfig to every active identity's
    // inbox + outbox. The default retention is propagated; per-peer
    // overrides ARE NOT touched here — by design, changing the
    // global default after a chat already exists doesn't retroactively
    // touch that chat.
    static void apply_inbox_config_to_all() {
      const auto& cfg = InboxConfig::current();
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.inbox)  a.inbox ->set_default_retention(cfg.default_retention);
        if (a.outbox) a.outbox->set_default_retention(cfg.default_retention);
      }
    }

    // Run prune_expired() on every active identity's inbox + outbox.
    // Called from the main loop at a low cadence so time-based
    // expirations fire even when no fresh messages are arriving.
    // Cheap on the no-op path (LXMFInbox::prune_expired returns
    // immediately when no retention policies are configured).
    static void prune_all() {
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.inbox)  a.inbox ->prune_expired();
        if (a.outbox) a.outbox->prune_expired();
      }
    }

    // Used by AnnounceLog to drop echoes of our own identities.
    static bool is_own_destination(const RNS::Bytes& dest) {
      for (auto& a : identities_storage()) {
        if (a.active && a.lxmf.address() == dest) return true;
      }
      return false;
    }

  private:
    static std::array<LXMFIdentity, LXMF_GATEWAY_MAX_IDENTITIES>& identities_storage() {
      static std::array<LXMFIdentity, LXMF_GATEWAY_MAX_IDENTITIES> s;
      return s;
    }

    static LXMFIdentity* first_free_slot() {
      for (auto& a : identities_storage()) if (!a.active) return &a;
      return nullptr;
    }

    static LXMFIdentity* identity_by_id_mut(const IdentityId& iden_id) {
      if (iden_id.empty()) return nullptr;
      for (auto& a : identities_storage()) if (a.active && a.id == iden_id) return &a;
      return nullptr;
    }

    static void ensure_root() {
      // Note: PosixFileSystem::exists() does open(O_RDONLY) which fails
      // for directories. Use isDirectory() for dir-existence checks.
      // mkdir() is internally idempotent so a redundant call is harmless,
      // but the explicit guard keeps log noise down.
      if (!filesystem.isDirectory(LXMF_GATEWAY_ROOT)) filesystem.mkdir(LXMF_GATEWAY_ROOT);
      const char* accts = LXMF_GATEWAY_ROOT "/identities";
      if (!filesystem.isDirectory(accts)) filesystem.mkdir(accts);
    }

    static void ensure_identity_dir(const LXMFIdentity& a) {
      const std::string d = a.dir();
      if (!filesystem.isDirectory(d.c_str())) filesystem.mkdir(d.c_str());
    }

    static void write_meta(const LXMFIdentity& a) {
      Common::PsramJsonDocument doc;
      doc["display_name"]         = a.display_name;
      doc["created_ms"]           = (uint32_t)millis();
      doc["announce_interval_ms"]         = a.announce_interval_ms;
      doc["persist_outbound_attachments"] = a.persist_outbound_attachments;
      if (a.password_hash.size() > 0) doc["password_hash"] = a.password_hash.toHex();
      if (a.password_salt.size() > 0) doc["password_salt"] = a.password_salt.toHex();
      String body;
      serializeJson(doc, body);
      filesystem.writeFile(a.meta_path().c_str(),
                           reinterpret_cast<const uint8_t*>(body.c_str()),
                           body.length());
    }

    static void read_meta(LXMFIdentity& a) {
      if (!filesystem.exists(a.meta_path().c_str())) return;
      std::vector<uint8_t> data;
      if (filesystem.readFile(a.meta_path().c_str(), data) == 0) return;
      Common::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      a.display_name         = (const char*)(doc["display_name"] | "LXMF Identity");
      a.announce_interval_ms = (uint32_t)(doc["announce_interval_ms"] | LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS);
      a.persist_outbound_attachments = (bool)(doc["persist_outbound_attachments"] | true);
      std::string ph = (const char*)(doc["password_hash"] | "");
      std::string ps = (const char*)(doc["password_salt"] | "");
      if (!ph.empty()) a.password_hash.assignHex(ph.c_str());
      if (!ps.empty()) a.password_salt.assignHex(ps.c_str());
    }

    // Per-conversation retention overrides persistence. Schema:
    //   { "peer_retention": {
    //       "<peer_hex>": { "kind": "time"|"count"|"none", "value": N },
    //       ...
    //   } }
    // The map is loaded into both inbox + outbox at activate() time
    // so the initial prune honours the user's saved choices.
    static void read_conversation_config(const LXMFIdentity& a,
                                          std::unordered_map<std::string, Retention>& out) {
      out.clear();
      const std::string path = a.conversation_config_path();
      if (!filesystem.exists(path.c_str())) return;
      std::vector<uint8_t> data;
      if (filesystem.readFile(path.c_str(), data) == 0) return;
      Common::PsramJsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      JsonObject pr = doc["peer_retention"].as<JsonObject>();
      if (pr.isNull()) return;
      for (JsonPair kv : pr) {
        JsonObjectConst entry = kv.value().as<JsonObjectConst>();
        if (entry.isNull()) continue;
        Retention r;
        r.kind  = retention_kind_from_str(entry["kind"] | "none");
        r.value = (uint32_t)(entry["value"] | 0);
        out[std::string(kv.key().c_str())] = r;
      }
    }
    static void write_conversation_config(const LXMFIdentity& a,
                                           const std::unordered_map<std::string, Retention>& peer_retention) {
      Common::PsramJsonDocument doc;
      JsonObject pr = doc["peer_retention"].to<JsonObject>();
      for (const auto& kv : peer_retention) {
        JsonObject o = pr[kv.first.c_str()].to<JsonObject>();
        o["kind"]  = retention_kind_name(kv.second.kind);
        o["value"] = kv.second.value;
      }
      String body;
      serializeJson(doc, body);
      filesystem.writeFile(a.conversation_config_path().c_str(),
                           reinterpret_cast<const uint8_t*>(body.c_str()),
                           body.length());
    }

    static void activate(LXMFIdentity& a) {
      a.active = true;
      const auto& cfg = InboxConfig::current();
      a.inbox  = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "inbox.jsonl",
                                                         LXMFInbox::DEFAULT_RAM_CAPACITY,
                                                         cfg.default_retention));
      a.outbox = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "outbox.jsonl",
                                                         LXMFInbox::DEFAULT_RAM_CAPACITY,
                                                         cfg.default_retention));
      // Load per-peer retention overrides from disk so the initial
      // prune respects existing user choices.
      {
        std::unordered_map<std::string, Retention> peer_retention;
        read_conversation_config(a, peer_retention);
        for (const auto& kv : peer_retention) {
          a.inbox->set_peer_retention(kv.first, kv.second);
          a.outbox->set_peer_retention(kv.first, kv.second);
        }
      }
      // Delete the on-disk attachment bytes when their owning record is
      // evicted from the inbox/outbox ring (TTL prune, capacity eviction,
      // or /api/.../conversations DELETE). Otherwise the JSONL line goes
      // away but the file under <dir>/attachments/<filename> stays
      // forever and flash fills up.
      const std::string adir = a.dir();
      // Body-spill machinery removed — bodies are always inline in
      // the JSONL ring (capped at LXMF_MAX_BODY_BYTES on the send
      // path). The earlier per-message spill files + lazy /body
      // endpoint were dead infrastructure: the SPA never called the
      // endpoint, so large bodies were silently inaccessible from
      // the UI even though the bytes were on disk.

      auto on_remove = [adir, p = &a](const MessageRecord& rec) {
        for (const auto& att : rec.attachments) {
          if (att.filename.empty()) continue;
          const std::string full = adir + "/attachments/" + att.filename;
          if (att.backend == "sd") {
            if (Storage::SDCard::present() && Storage::SDCard::exists(full.c_str())) {
              SD.remove(full.c_str());
            }
          } else {
            if (filesystem.exists(full.c_str())) filesystem.remove(full.c_str());
          }
        }
        // Unlink the per-message body spill file on whichever backend
        // it landed on. Pick the right mailbox (inbox vs outbox) from
        // the record's direction. SD-first, flash-fallback.
        const std::string mailbox = rec.incoming ? "inbox" : "outbox";
        const std::string body_path = adir + "/" + mailbox + "/" +
                                       std::to_string(rec.seq) + ".body";
        if (Storage::SDCard::present() && Storage::SDCard::exists(body_path.c_str())) {
          SD.remove(body_path.c_str());
        }
        if (filesystem.exists(body_path.c_str())) {
          filesystem.remove(body_path.c_str());
        }
      };
      a.inbox->set_on_remove(on_remove);
      a.outbox->set_on_remove(on_remove);
      a.inbox->load();
      a.outbox->load();
      a.inbox->prune_expired();
      a.outbox->prune_expired();

      a.lxmf.init(a.identity, a.display_name.c_str());
      LXMFIdentity* p = &a;
      a.lxmf.set_delivery_callback([p](const MessageRecord& rec) {
        if (!p->active || !p->inbox) return;
        MessageRecord local = rec;  // copy so we can mutate seq
        local.seq = 0;
        p->inbox->append(local);
        // Push to any WS client subscribed to this identity. SSE
        // pollers still pick up the same record via inbox->since().
        Web::WS::publish_incoming(p->id, local);
      });
      // Outbound lifecycle: link-mode sends start as Queued in the outbox
      // (see LXMFMinimal::send_message), and transition to Sent / Delivered
      // / Failed as the Link or Resource completes. The lookup key is the
      // link hash that was stamped onto the outbox record's packet_hash.
      a.lxmf.set_outbox_status_callback(
          [p](const RNS::Bytes& link_hash, OutboxStatus status) {
            if (!p->active || !p->outbox) return;
            p->outbox->update_status(link_hash, status);
            // Push a typed status frame to any WS subscriber — gives
            // the SPA's outbox row a direct trigger to flip the pill.
            Web::WS::publish_outbox_status(p->id, link_hash,
                                           outbox_status_name(status));
            // Terminal transitions (Sent / Delivered / Failed) should
            // also flush a final progress event so the SPA can flip the
            // in-flight bubble into its static form.
            if (status == OutboxStatus::Delivered ||
                status == OutboxStatus::Sent      ||
                status == OutboxStatus::Failed) {
              Web::publish_lxmf_progress(
                  p->id, /*peer=*/RNS::Bytes{}, link_hash, /*incoming=*/false,
                  /*bytes_done=*/0, /*bytes_total=*/0, /*finished=*/true);
            }
          });
      // Resource progress: fire SSE events so the SPA can render an
      // in-flight progress bar on the mid-transfer message bubble.
      // Also surfaces inbound progress on the OLED marquee so a user
      // looking at the device (not the SPA) knows a transfer is
      // landing and how far through it is.
      a.lxmf.set_progress_callback(
          [p](const RNS::Bytes& peer_hash, const RNS::Bytes& link_hash,
              bool incoming, uint32_t bytes_done, uint32_t bytes_total) {
            if (!p->active) return;
            Web::publish_lxmf_progress(p->id, peer_hash, link_hash,
                                       incoming, bytes_done, bytes_total,
                                       /*finished=*/false);
            // OLED marquee: incoming only. Outbound progress is shown
            // by the SPA on the bubble; the device-side user isn't
            // typically watching their own send. Use update() (not
            // say()) so the ring head slot is reused for each chunk
            // instead of stacking N messages per transfer. 5 s TTL
            // means the marquee auto-clears if the transfer aborts
            // mid-flight rather than getting stuck on the last %.
            if (incoming && bytes_total > 0) {
              const uint32_t pct = (uint32_t)(((uint64_t)bytes_done * 100)
                                              / bytes_total);
              char buf[Common::Status::MAX_MESSAGE_LEN];
              if (bytes_total < 1024) {
                snprintf(buf, sizeof(buf), "Recv %uB %u%%",
                         (unsigned)bytes_total, (unsigned)pct);
              } else {
                snprintf(buf, sizeof(buf), "Recv %uK %u%%",
                         (unsigned)(bytes_total / 1024), (unsigned)pct);
              }
              // Sticky (ttl=0): a Resource can stall between parts for longer
              // than any fixed TTL on an airtime-limited link, and the marquee
              // must not lapse back to the signal bars mid-transfer. The
              // receive-complete terminal below supersedes it; the Resource
              // always concludes (complete/fail/timeout), so it never sticks.
              Common::Status::update(buf, 0);
            }
          });
      // Inbound receive-complete: symmetric counterpart to the outbox
      // Sent/Delivered terminal in set_outbox_status_callback above.
      // Fires when the receiving Resource finishes streaming bytes,
      // BEFORE the decrypt step — so even decrypt-fail / malformed-
      // payload cases get a terminal message_complete on the wire and
      // the SPA's synthetic "Incoming attachment …" row + topbar strip
      // clear correctly. peer_hash is empty here (the LXMF source hash
      // is only known post-decrypt); the SPA keys the clear off the
      // link hash that earlier message_progress events also carried.
      a.lxmf.set_receive_complete_callback(
          [p](const RNS::Bytes& link_hash, uint32_t bytes_total, bool ok) {
            if (!p->active) return;
            Web::publish_lxmf_progress(
                p->id, /*peer=*/RNS::Bytes{}, link_hash, /*incoming=*/true,
                /*bytes_done=*/bytes_total, /*bytes_total=*/bytes_total,
                /*finished=*/true);
            // Supersede the sticky "Recv N%" marquee with a brief terminal so
            // the OLED strip reverts to the signal bars promptly instead of
            // holding the last percentage forever.
            Common::Status::update(ok ? "Recv complete" : "Recv failed", 2500);
          });
      // Attachment persistence — when an incoming LXMF message carries
      // FIELD_FILE_ATTACHMENTS / FIELD_IMAGE / FIELD_AUDIO blobs, write
      // each one to <identity_dir>/attachments/ (#122 routing: SD if a
      // card is mounted, else LittleFS). On-disk filenames are always
      // "<msg_hash_hex>_<tag>_<idx>.bin". The sender-supplied filename
      // (Sideband convention, populated by the FieldBlob) is propagated
      // onto AttachmentMeta.display_name for the SPA to use as the
      // download-prompt label.
      a.lxmf.set_attachment_persist_callback(
          [p](const RNS::Bytes& msg_hash,
              const std::vector<LXMFMinimal::FieldBlob>& fields) -> std::vector<AttachmentMeta> {
            std::vector<AttachmentMeta> out;
            if (!p->active) return out;
            const bool use_sd = Storage::SDCard::present();
            const std::string att_dir = p->dir() + "/attachments";
            // LittleFS-side directory still gets prepared even when SD
            // is mounted — small attachments (or fallback) land here.
            if (!filesystem.isDirectory(att_dir.c_str())) {
              filesystem.mkdir(att_dir.c_str());
            }
            for (size_t i = 0; i < fields.size(); ++i) {
              const auto& f = fields[i];
              char on_disk[96];
              snprintf(on_disk, sizeof(on_disk), "%s_%02x_%u.bin",
                       msg_hash.toHex().c_str(), (unsigned)f.tag, (unsigned)i);
              const std::string full = att_dir + "/" + on_disk;
              bool wrote_ok = false;
              std::string backend = "flash";
              RNS::Utilities::OS::reset_watchdog();
              if (use_sd) {
                const size_t w = Storage::Streaming::write_from_buffer(
                    full.c_str(), /*use_sd=*/true, f.raw, f.raw_len);
                if (w == f.raw_len) { wrote_ok = true; backend = "sd"; }
                else {
                  WARNINGF("LXMF: SD attachment write short (wrote %u/%u for %s) — falling back to flash",
                           (unsigned)w, (unsigned)f.raw_len, on_disk);
                }
              }
              if (!wrote_ok) {
                const size_t w = Storage::Streaming::write_from_buffer(
                    full.c_str(), /*use_sd=*/false, f.raw, f.raw_len);
                if (w == f.raw_len) wrote_ok = true;
                else WARNINGF("LXMF: flash attachment write short (wrote %u/%u) for %s",
                              (unsigned)w, (unsigned)f.raw_len, on_disk);
              }
              RNS::Utilities::OS::reset_watchdog();
              if (!wrote_ok) continue;
              AttachmentMeta meta;
              meta.tag      = f.tag;
              meta.size     = (uint32_t)f.raw_len;
              meta.filename = on_disk;
              meta.backend  = backend;
              // Sideband's FIELD_IMAGE only carries the extension
              // (e.g. "png", "webp"). Construct a usable display name
              // from it. For FILE_ATTACHMENTS, f.filename is already
              // the full sender-supplied name. AUDIO has no name.
              if (f.tag == LXMF::FIELD_IMAGE && !f.filename.empty()) {
                meta.display_name = "image." + f.filename;
              } else if (f.tag == LXMF::FIELD_FILE_ATTACHMENTS) {
                meta.display_name = f.filename;
              }
              // Trust-boundary truncation: both display_name and the
              // sender-supplied filename feed into the same field
              // here. Cap at the configured limit so a malicious peer
              // can't grow the inbox JSONL line with megabyte names.
              if (meta.display_name.size() > LXMF::LXMF_MAX_ATTACHMENT_NAME) {
                meta.display_name.resize(LXMF::LXMF_MAX_ATTACHMENT_NAME);
              }
              out.push_back(meta);
              NOTICEF("LXMF: persisted attachment %s (%u B, backend=%s, display='%s')",
                      on_disk, (unsigned)f.raw_len,
                      backend.c_str(), meta.display_name.c_str());
            }
            return out;
          });
      // Outbound attachment persistence — copy the staging bytes to
      // the sender's identity dir so their own chat bubble can show
      // an inline preview of what they sent. Same filename layout as
      // the inbound path; the SPA renders both from the same endpoint.
      a.lxmf.set_outbound_persist_callback(
          [p](const RNS::Bytes& msg_hash,
              const std::vector<LXMFMinimal::OutgoingAttachment>& outgoing)
              -> std::vector<AttachmentMeta> {
            std::vector<AttachmentMeta> out;
            if (!p->active || !p->persist_outbound_attachments) return out;
            const bool use_sd = Storage::SDCard::present();
            const std::string att_dir = p->dir() + "/attachments";
            if (!filesystem.isDirectory(att_dir.c_str())) {
              filesystem.mkdir(att_dir.c_str());
            }
            out.resize(outgoing.size());
            for (size_t i = 0; i < outgoing.size(); ++i) {
              const auto& a = outgoing[i];
              if (a.staging_id == 0 && a.data.empty()) continue;
              char on_disk[96];
              snprintf(on_disk, sizeof(on_disk), "%s_%02x_%u.bin",
                       msg_hash.toHex().c_str(), (unsigned)a.tag, (unsigned)i);
              const std::string full = att_dir + "/" + on_disk;
              const size_t total = a.staging_id ? a.staging_total_bytes : a.data.size();
              // Stream the bytes in 4 KiB chunks so a multi-MB
              // attachment doesn't need to materialise twice in RAM.
              // The destination file is built up chunk by chunk
              // (truncate first, then append-write).
              bool wrote_ok = false;
              std::string backend = "flash";
              // Reader pulls bytes from either OutboundStaging (PSRAM /
              // SD-staging backed) or the inline a.data vector. Both
              // are random-access by offset, so we can be called more
              // than once across SD-then-flash fallback without
              // re-priming any cursor state.
              auto next_chunk = [&a](uint8_t* dst, size_t off, size_t want) -> size_t {
                if (a.staging_id) {
                  return Storage::OutboundStaging::read(a.staging_id, off, want, dst);
                }
                const size_t avail = a.data.size() > off ? a.data.size() - off : 0;
                const size_t take = std::min(want, avail);
                if (take) memcpy(dst, a.data.data() + off, take);
                return take;
              };
              if (use_sd) {
                const size_t w = Storage::Streaming::write_streamed(
                    full.c_str(), /*use_sd=*/true, total, next_chunk);
                if (w == total) { wrote_ok = true; backend = "sd"; }
                else WARNINGF("LXMF: SD outbound write short (%u/%u) for %s — fallback to flash",
                              (unsigned)w, (unsigned)total, on_disk);
                if (!wrote_ok && SD.exists(full.c_str())) SD.remove(full.c_str());
              }
              if (!wrote_ok) {
                const size_t w = Storage::Streaming::write_streamed(
                    full.c_str(), /*use_sd=*/false, total, next_chunk);
                if (w == total) wrote_ok = true;
                else WARNINGF("LXMF: flash outbound write short (%u/%u) for %s",
                              (unsigned)w, (unsigned)total, on_disk);
              }
              if (!wrote_ok) continue;
              out[i].tag      = a.tag;
              out[i].size     = (uint32_t)total;
              out[i].filename = on_disk;
              out[i].backend  = backend;
              NOTICEF("LXMF: persisted outbound attachment %s (%u B, backend=%s)",
                      on_disk, (unsigned)total, backend.c_str());
            }
            return out;
          });
      a.last_announce_ms = 0;  // announce on first loop tick
    }

    static void load_existing_identities() {
      const char* accts = LXMF_GATEWAY_ROOT "/identities";
      // PosixFileSystem::exists() returns false for directories (it does
      // open(O_RDONLY) which fails on dirs). Use isDirectory() here so we
      // don't silently skip identity loading. Same applies in ensure_root()
      // and ensure_identity_dir().
      if (!filesystem.isDirectory(accts)) {
        NOTICEF("LXMFGateway: %s is not a directory — no identities to load", accts);
        return;
      }
      auto entries = filesystem.listDirectory(accts);
      size_t loaded = 0;
      for (const auto& entry : entries) {
        if (loaded >= LXMF_GATEWAY_MAX_IDENTITIES) {
          WARNINGF("LXMFGateway: skipping %s (max identities reached)", entry.c_str());
          continue;
        }
        std::string full_path = std::string(accts) + "/" + entry;
        if (!filesystem.isDirectory(full_path.c_str())) continue;

        LXMFIdentity* slot = first_free_slot();
        if (!slot) break;

        std::string identity_path = full_path + "/identity.dat";
        if (!filesystem.exists(identity_path.c_str())) {
          WARNINGF("LXMFGateway: identity %s missing identity.dat, skipping", entry.c_str());
          continue;
        }
        RNS::Identity id = RNS::Identity::from_file(identity_path.c_str());
        if (!id) {
          WARNINGF("LXMFGateway: failed to load identity for identity %s", entry.c_str());
          continue;
        }
        slot->id           = entry;
        slot->identity     = id;
        slot->display_name = "LXMF Identity";  // overridden by meta if present
        read_meta(*slot);
        // Load persisted ratchet ring (if any). Identities created on
        // pre-ratchet builds won't have a file — that's fine, the ring
        // stays empty and will populate on first announce.
        slot->ratchets.load(slot->ratchet_path(), filesystem);
        activate(*slot);
        loaded++;
        NOTICEF("LXMFGateway: loaded identity %s (%s) → %s (ratchets=%u)",
                slot->id.c_str(), slot->display_name.c_str(),
                slot->address_hex().c_str(), (unsigned)slot->ratchets.size());
      }
    }

  public:
    // Persist per-conversation retention overrides for `a`. Called from
    // the WebUI handler after it has applied a change via
    // LXMFInbox::set/clear_peer_retention(). Delegates to the
    // private write_conversation_config so the persistence layer
    // stays in one place but the call site can sit in WebUI.h.
    static void persist_conversation_config(const LXMFIdentity& a,
                                             const std::unordered_map<std::string, Retention>& peer_retention) {
      write_conversation_config(a, peer_retention);
    }

    // Called from the Destination::announce patch via the C bridge.
    // Generates a fresh ratchet keypair for the identity whose delivery
    // destination hash matches `dest_hash`, persists the ring, and writes
    // the new pubkey into `out_pubkey` (must be 32 bytes wide).
    // Returns true on success. False means "no LXMF identity matches this
    // destination" — the announce should not include a ratchet.
    static bool rotate_outbound_ratchet(const uint8_t* dest_hash, uint8_t* out_pubkey) {
      if (!dest_hash || !out_pubkey) return false;
      RNS::Bytes dh(dest_hash, 16);
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.lxmf.address() != dh) continue;
        const auto& entry = a.ratchets.rotate((uint64_t)millis());
        a.ratchets.save(a.ratchet_path(), filesystem);
        if (entry.pubkey.size() != RatchetStore::RATCHET_BYTES) return false;
        memcpy(out_pubkey, entry.pubkey.data(), RatchetStore::RATCHET_BYTES);
        return true;
      }
      return false;
    }

    // Called from the Identity::decrypt patch via the C bridge.
    // Returns the `index`-th ratchet privkey (newest-first) for the
    // identity whose identity hash matches `identity_hash`.
    // Returns false when index >= ring size or no matching identity.
    static bool inbound_ratchet_privkey(const uint8_t* identity_hash, size_t index, uint8_t* out_privkey) {
      if (!identity_hash || !out_privkey) return false;
      RNS::Bytes ih(identity_hash, 16);
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.identity.hash() != ih) continue;
        const auto* e = a.ratchets.at_newest_first(index);
        if (!e) return false;
        if (e->privkey.size() != RatchetStore::RATCHET_BYTES) return false;
        memcpy(out_privkey, e->privkey.data(), RatchetStore::RATCHET_BYTES);
        return true;
      }
      return false;
    }

  private:
    static inline bool _setup_done = false;
    static inline std::vector<LXMFIdentity*> _active_view;
  };

  // Implementation of the AnnounceLog shim declared in AnnounceLog.h.
  inline bool announce_log_is_own_identity(const RNS::Bytes& destination_hash) {
    return LXMFGateway::is_own_destination(destination_hash);
  }

} // namespace LXMF
