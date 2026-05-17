#pragma once

#include <ArduinoJson.h>
#include <Log.h>
#include <Identity.h>
#include <Utilities/OS.h>
#include <microStore/FileSystem.h>
#include "../Web/SDCard.h"
#include "../Web/OutboundStaging.h"
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
namespace Web {
  void publish_lxmf_progress(const LXMF::IdentityId& identity_id,
                             const RNS::Bytes& peer_hash,
                             const RNS::Bytes& link_hash,
                             bool incoming,
                             uint32_t bytes_done,
                             uint32_t bytes_total,
                             bool finished);
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
      // (#107) Drive outbox-retry: pending_link_sends() is a shared
      // static across all LXMFMinimal instances, so one call covers
      // every identity. Entries whose next_retry_at_ms has elapsed get
      // a fresh Link and another attempt.
      LXMFMinimal::tick_retries();
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
      if (!a->lxmf.send_message(dest_hash, title, content, attachments, out_rec, out_err)) {
        return false;
      }
      if (a->outbox) a->outbox->append(out_rec);
      return true;
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
    // inbox + outbox. Called by /api/inbox_config POST after the
    // new config is persisted. (#129)
    static void apply_inbox_config_to_all() {
      const auto& cfg = InboxConfig::current();
      for (auto& a : identities_storage()) {
        if (!a.active) continue;
        if (a.inbox) {
          a.inbox->set_capacity(cfg.ram_capacity);
          a.inbox->set_ttl_seconds(cfg.ttl_seconds);
        }
        if (a.outbox) {
          a.outbox->set_capacity(cfg.ram_capacity);
          a.outbox->set_ttl_seconds(cfg.ttl_seconds);
        }
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
      JsonDocument doc;
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
      JsonDocument doc;
      if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
      a.display_name         = (const char*)(doc["display_name"] | "LXMF Identity");
      a.announce_interval_ms = (uint32_t)(doc["announce_interval_ms"] | LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS);
      a.persist_outbound_attachments = (bool)(doc["persist_outbound_attachments"] | true);
      std::string ph = (const char*)(doc["password_hash"] | "");
      std::string ps = (const char*)(doc["password_salt"] | "");
      if (!ph.empty()) a.password_hash.assignHex(ph.c_str());
      if (!ps.empty()) a.password_salt.assignHex(ps.c_str());
    }

    static void activate(LXMFIdentity& a) {
      a.active = true;
      // Inbox + outbox capacity/TTL come from the global config
      // (#129). Default state when no config file exists keeps the
      // historical 200-entry FIFO behaviour.
      const auto& cfg = InboxConfig::current();
      a.inbox  = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "inbox.jsonl",
                                                         cfg.ram_capacity, cfg.ttl_seconds));
      a.outbox = std::unique_ptr<LXMFInbox>(new LXMFInbox(a.dir(), "outbox.jsonl",
                                                         cfg.ram_capacity, cfg.ttl_seconds));
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
      });
      // Outbound lifecycle: link-mode sends start as Queued in the outbox
      // (see LXMFMinimal::send_message), and transition to Sent / Delivered
      // / Failed as the Link or Resource completes. The lookup key is the
      // link hash that was stamped onto the outbox record's packet_hash.
      a.lxmf.set_outbox_status_callback(
          [p](const RNS::Bytes& link_hash, OutboxStatus status) {
            if (!p->active || !p->outbox) return;
            p->outbox->update_status(link_hash, status);
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
      a.lxmf.set_progress_callback(
          [p](const RNS::Bytes& peer_hash, const RNS::Bytes& link_hash,
              bool incoming, uint32_t bytes_done, uint32_t bytes_total) {
            if (!p->active) return;
            Web::publish_lxmf_progress(p->id, peer_hash, link_hash,
                                       incoming, bytes_done, bytes_total,
                                       /*finished=*/false);
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
            const bool use_sd = Web::SDCard::present();
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
                // Try SD first. On failure, fall back to LittleFS so the
                // user doesn't lose the attachment due to a flaky card.
                const size_t w = Web::SDCard::write_file(full.c_str(), f.raw, f.raw_len);
                if (w == f.raw_len) { wrote_ok = true; backend = "sd"; }
                else {
                  WARNINGF("LXMF: SD attachment write short (wrote %u/%u for %s) — falling back to flash",
                           (unsigned)w, (unsigned)f.raw_len, on_disk);
                }
              }
              if (!wrote_ok) {
                const size_t w = filesystem.writeFile(full.c_str(), f.raw, f.raw_len);
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
            const bool use_sd = Web::SDCard::present();
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
              if (filesystem.exists(full.c_str())) filesystem.remove(full.c_str());
              bool wrote_ok = false;
              std::string backend = "flash";
              std::vector<uint8_t> chunk;
              chunk.reserve(4096);
              size_t off = 0;
              auto next_chunk = [&](uint8_t* buf, size_t want) -> size_t {
                if (a.staging_id) {
                  return Web::OutboundStaging::read(a.staging_id, off, want, buf);
                }
                const size_t avail = a.data.size() > off ? a.data.size() - off : 0;
                const size_t take = std::min(want, avail);
                if (take) memcpy(buf, a.data.data() + off, take);
                return take;
              };
              if (use_sd) {
                File f = SD.open(full.c_str(), FILE_WRITE);
                if (f) {
                  size_t written = 0;
                  uint8_t buf[1024];
                  while (off < total) {
                    const size_t want = std::min((size_t)sizeof(buf), total - off);
                    const size_t got = next_chunk(buf, want);
                    if (got == 0) break;
                    const size_t w = f.write(buf, got);
                    if (w != got) break;
                    off += got;
                    written += w;
                    RNS::Utilities::OS::reset_watchdog();
                  }
                  f.close();
                  if (written == total) { wrote_ok = true; backend = "sd"; }
                  else WARNINGF("LXMF: SD outbound write short (%u/%u) for %s — fallback to flash",
                                (unsigned)written, (unsigned)total, on_disk);
                }
                if (!wrote_ok && SD.exists(full.c_str())) SD.remove(full.c_str());
              }
              if (!wrote_ok) {
                // Buffer the whole thing on the heap for the flash
                // writeFile API. Outbound persistence on flash is best-
                // effort: it'll fail silently for multi-MB images on
                // an 8 MB part, which is fine — that's why the user
                // bought an SD card.
                std::vector<uint8_t> all(total);
                size_t roff = 0;
                while (roff < total) {
                  const size_t want = std::min((size_t)4096, total - roff);
                  const size_t got = next_chunk(all.data() + roff, want);
                  if (got == 0) break;
                  roff += got;
                  off  += got;
                }
                if (roff == total) {
                  const size_t w = filesystem.writeFile(full.c_str(), all.data(), all.size());
                  if (w == total) wrote_ok = true;
                  else WARNINGF("LXMF: flash outbound write short (%u/%u) for %s",
                                (unsigned)w, (unsigned)total, on_disk);
                }
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
