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
#include "GpxTrack.h"
#include <SD.h>

#include <deque>
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
// direction - so we can't pull it in here). Linker resolves the inline
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
    // Status update keyed by outbox seq rather than packet hash - used for
    // queued sends that have no packet hash yet (e.g. the "finding route"
    // give-up, or a deferred-stamp send). The SPA matches the bubble by
    // seq; link_hash (may be empty) lets it adopt the packet hash a
    // stamped send acquired at dispatch so later hash-keyed status
    // frames still find the bubble.
    void publish_outbox_status_seq(const LXMF::IdentityId& identity_id,
                                   uint32_t seq,
                                   const char* status_name,
                                   const RNS::Bytes& link_hash);
  }
}

// Forward declaration of the telemetry sender's delivery-state hook.
// Defined inline in LXMF/TelemetrySender.h, which #includes us - the
// same cycle-breaking arrangement as the Web hooks above. Telemetry
// sends skip the outbox, so this hook is the only consumer of their
// delivery receipts.
namespace LXMF {
  namespace TelemetrySender {
    void on_outbox_status(const RNS::Bytes& hash, OutboxStatus status);
  }
}

// Forward declaration of the screen-identity OLED notifier. Defined
// inline in LXMF/ScreenNotify.h (which needs AnnounceLog, a header
// this file cannot include - see ScreenNotify.h). The firmware TU
// includes the definition.
namespace LXMF {
  struct MessageRecord;
  void screen_notify_incoming(const MessageRecord& rec);
  // OLED messenger hooks - same arrangement, defined in
  // LXMF/Messenger.h. on_outbox_status drives the Sent / Delivered /
  // Failed result page for sends made from the device buttons;
  // on_screen_identity_changed loads (or seeds) the new holder's
  // private preset store on enable and destroys it on disable.
  namespace Messenger {
    void on_outbox_status(const RNS::Bytes& hash, OutboxStatus status);
    void on_screen_identity_changed(bool enabled, const std::string& identity_dir);
  }
  // Telemetry sharing hooks (LXMF/TelemetryShare.h, same arrangement):
  // inbound FIELD_COMMANDS reach it so granted peers can request fresh
  // readings during a live share window; FIELD_CUSTOM_META carries the
  // sender's live-update offer (window + rate) that starts our request
  // loop; inbound telemetry refreshes an active feed's latest readings.
  namespace TelemetryShare {
    void on_commands(const IdentityId& iden, const RNS::Bytes& peer,
                     const RNS::Bytes& raw);
    void on_live_offer(const IdentityId& iden, const RNS::Bytes& peer,
                       const RNS::Bytes& raw);
    void on_telemetry(const IdentityId& iden, const RNS::Bytes& peer,
                      const RNS::Bytes& blob);
    bool meta_marks_message(const RNS::Bytes& raw);
    bool has_feed(const IdentityId& iden, const RNS::Bytes& peer);
    bool location_from_blob(const RNS::Bytes& blob, double& lat, double& lon);
    void set_feed_gpx(const IdentityId& iden, const RNS::Bytes& peer,
                      const std::string& path, bool use_sd, double lat, double lon);
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
    // Inbound stamp policy (anti-spam proof-of-work). stamp_cost 0 =
    // no stamp required; 1-254 = the cost announced to senders.
    // enforce_stamps mirrors upstream LXMRouter's default-off
    // enforce_stamps(): off = validate-and-record but deliver anyway.
    uint8_t              stamp_cost = 0;
    bool                 enforce_stamps = false;
    // Compose-popover defaults for attaching telemetry to a message:
    // which items are pre-selected and the default live-share window
    // (0 = one-shot). The popover can override per message.
    bool     telemetry_location    = true;
    bool     telemetry_environment = false;
    bool     telemetry_battery     = false;
    bool     telemetry_compass     = false;
    uint32_t telemetry_share_s     = 0;
    uint32_t telemetry_rate_s      = 60;
    // The "screen" identity: the one identity whose messages the OLED
    // surfaces. Opt-in, at most one holder at a time (see
    // set_screen_identity). Holding the device means reading this
    // identity's messages, by design - enabling therefore requires the
    // physical-presence identity code (enforced at the web handler).
    bool                 screen = false;
    // Flash the charge LED on a new message. Only the screen identity
    // surfaces messages on the OLED, so this gates that LED alert.
    bool                 msg_flash = true;
    bool                 active = false;
    // Password hash (PBKDF2-HMAC-SHA256) + per-identity salt. Set at
    // identity creation; required for login. Empty if identity is from an
    // older firmware build that didn't set passwords (in which case
    // login is blocked until the identity is recreated - there is no
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
      // the same tick - catches any PendingLinkSend whose lifecycle
      // callback never fired (radio cut out mid-transfer, async event
      // loop dropped a frame, etc.) so the map can't grow unbounded.
      LXMFMinimal::tick_retries();
      LXMFMinimal::sweep_orphaned_pending();
      // Inactivity-expire + evict idle reuse Links (#90 Phase 2), bounding the
      // direct_links cache. Same shared-static cadence as the retry tick.
      LXMFMinimal::tick_clean_links();
      // Advance opportunistic (single-packet) sends: proof -> Delivered,
      // timeout -> Failed. Same shared-static cadence as the retry tick.
      LXMFMinimal::tick_opportunistic_receipts();
      // Apply announce-triggered accelerations BEFORE the retry/recheck
      // ticks below so a peer that just announced gets its queued work
      // dispatched in this very pass.
      drain_peer_announcements();
      // Auto-send messages whose recipient key has now arrived via the
      // path request issued at send time (AP-mode on-demand key learning).
      tick_pending_identity_sends();
      // Advance deferred delivery-stamp work: inbound messages parked
      // for validation, and outbound sends waiting on the background
      // proof-of-work worker. Both queues are FIFO and share the single
      // Stamp worker task.
      LXMFMinimal::tick_stamp_validations();
      tick_pending_stamp_sends();
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

    // Inbound stamp policy. cost 0 disables (announce carries nil);
    // 1-254 is announced to senders on the next announce (mirrors
    // upstream LXMRouter.set_inbound_stamp_cost's accepted range).
    // The caller fires an announce separately if it wants peers to
    // learn the new cost immediately.
    static bool set_stamp_cost(const IdentityId& iden_id, uint8_t cost) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->stamp_cost = (cost >= 1 && cost <= 254) ? cost : 0;
      a->lxmf.set_stamp_cost(a->stamp_cost);
      write_meta(*a);
      return true;
    }

    static bool set_enforce_stamps(const IdentityId& iden_id, bool on) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->enforce_stamps = on;
      a->lxmf.set_enforce_stamps(on);
      write_meta(*a);
      return true;
    }

    static bool set_msg_flash(const IdentityId& iden_id, bool on) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->msg_flash = on;
      write_meta(*a);
      return true;
    }

    // Default item set + share window for the compose telemetry
    // popover. Only seeds the popover's initial state; each send still
    // carries its own explicit selection.
    static bool set_telemetry_defaults(const IdentityId& iden_id,
                                       bool location, bool environment,
                                       bool battery, bool compass,
                                       uint32_t share_s, uint32_t rate_s) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->telemetry_location    = location;
      a->telemetry_environment = environment;
      a->telemetry_battery     = battery;
      a->telemetry_compass     = compass;
      a->telemetry_share_s     = share_s;
      a->telemetry_rate_s      = rate_s;
      write_meta(*a);
      return true;
    }

    // Update the LXMF announcement label and persist. Pushes the new
    // name into the live LXMFMinimal so the next announce() carries it
    // - does NOT trigger an announce here (the caller fires one off
    // separately so peers re-learn the label).
    static bool set_display_name(const IdentityId& iden_id, const std::string& name) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->display_name = name;
      a->lxmf.set_display_name(name.c_str());
      write_meta(*a);
      return true;
    }

    // The identity whose messages the OLED surfaces, or nullptr when
    // none is assigned. At most one active identity holds the flag.
    static const LXMFIdentity* screen_identity() {
      for (auto& a : identities_storage()) {
        if (a.active && a.screen) return &a;
      }
      return nullptr;
    }

    // Assign or clear the screen flag. Enabling fails while another
    // identity holds it - the holder must disable it first, explicitly,
    // so the screen never silently changes hands. Physical-presence
    // proof for enabling is the web handler's responsibility.
    static bool set_screen_identity(const IdentityId& iden_id, bool enable,
                                    const char** out_err = nullptr) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) {
        if (out_err) *out_err = "No such identity is logged in on this device.";
        return false;
      }
      if (enable) {
        const LXMFIdentity* holder = screen_identity();
        if (holder && holder->id != a->id) {
          if (out_err) *out_err = "Another identity already shows on the screen. Turn it off there first.";
          return false;
        }
        a->screen = true;
      } else {
        a->screen = false;
      }
      write_meta(*a);
      // Preset lifecycle: load-or-seed the holder's private store on
      // enable, destroy it on disable (Messenger.h).
      Messenger::on_screen_identity_changed(enable, a->dir());
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
    // Copy out an identity's stored password salt + hash. The caller runs the
    // slow PBKDF2 verify on these copies WITHOUT holding rns_lock, so a login
    // can't stall the main loop (rns_lock is shared with reticulum.loop()).
    // Returns false if no such identity; empty salt/hash means a pre-password
    // identity from older firmware (the caller refuses login).
    static bool read_password_material(const IdentityId& iden_id,
                                       RNS::Bytes& salt_out, RNS::Bytes& hash_out) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      salt_out = a->password_salt;
      hash_out = a->password_hash;
      return true;
    }

    // Tear down and remove an identity from disk.
    static bool delete_identity(const IdentityId& iden_id) {
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) return false;
      a->lxmf.shutdown();
      // Drop the on-disk attachment files before the JSONL spool. Walk
      // every loaded record and unlink each AttachmentMeta's backing
      // file - without this, the spool goes away but
      // <dir>/attachments/<filename> stays forever.
      auto unlink_atts = [&](const LXMFInbox* box) {
        if (!box) return;
        // Iterate the deque directly - no vector copy. The old
        // recent(SIZE_MAX) call duplicated the entire ring just to
        // walk it once.
        for (const auto& rec : box->ring()) {
          for (const auto& att : rec.attachments) {
            if (att.filename.empty()) continue;
            const std::string full = a->dir() + "/attachments/" + att.filename.c_str();
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
      // Drop any stamped sends still pending for this identity -
      // aborts in-flight proof-of-work and removes payload sidecars.
      {
        auto& q = pending_stamp_sends();
        for (auto it = q.begin(); it != q.end(); ) {
          if (it->iden_id == iden_id) {
            Discovery::Stamp::cancel(it->message_id);
            if (it->sidecar_handle) Storage::SdWriter::release(it->sidecar_handle);
            remove_pending_stamp_sidecar(it->sidecar);
            it = q.erase(it);
          } else {
            ++it;
          }
        }
      }
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
                     const char** out_err = nullptr,
                     // Set true when the send was accepted into the auto-send
                     // queue instead of sent now (no route yet). The caller
                     // (handle_send) reports that as an accepted/queued
                     // response, not a failure, and out_rec carries an
                     // optimistic "finding route" record keyed by the reserved
                     // seq. The auto-send tick passes use_seq so the real
                     // record reuses that seq and the SPA bubble merges.
                     bool* out_queued = nullptr,
                     uint32_t use_seq = 0,
                     // Optional non-attachment fields (telemetry blob,
                     // pre-packed commands, custom meta - see
                     // LXMF::ExtraFields). A send that carries ONLY
                     // these (no title, content or attachments) is
                     // machinery: it stays out of the outbox and the
                     // conversation - Sideband keeps its collector
                     // updates out of the conversation too, and one
                     // record per interval would crowd the bounded
                     // outbox. Delivery state for such sends flows
                     // through TelemetrySender::on_outbox_status.
                     const ExtraFields* extra = nullptr) {
      if (out_queued) *out_queued = false;
      const bool machinery_send = extra != nullptr && !extra->empty()
                                  && !extra->visible
                                  && title.empty() && content.empty()
                                  && (attachments == nullptr || attachments->empty());
      LXMFIdentity* a = identity_by_id_mut(iden_id);
      if (!a) {
        if (out_err) *out_err = "No such identity is logged in on this device.";
        return false;
      }
      // Auto-retry when there's no route to the recipient yet - either no
      // transport PATH to them, or their public key isn't cached. Upstream
      // LXMF requests the path whenever has_path() is false
      // (LXMRouter.py:1675), not just on an identity-cache miss: under load
      // the path table evicts the active contact while the key stays cached,
      // so gating on the key alone silently built a pathless packet that
      // Transport then dropped. A path request covers both - its response
      // announce re-populates the key too. Queue + auto-resend once a route
      // exists (tick_pending_identity_sends).
      //
      // Attachment sends queue too, but on a bounded window: the queue takes
      // ownership of the staged attachment buffers (we return here, before
      // send_message's StagingReleaser would free them) and releases them the
      // instant the window expires. The window is exactly PATH_REQUEST_TIMEOUT
      // - the path request fires here, once, on this first attempt, so its
      // timeout and the staging deadline start together and expire together.
      if (!RNS::Transport::has_path(dest_hash) || !RNS::Identity::recall(dest_hash)) {
        RNS::Transport::request_path(dest_hash);
        const uint32_t seq = queue_pending_identity_send(a, iden_id, dest_hash, title, content, attachments, extra);
        if (seq != 0) {
          // Accepted, not yet sent. Hand back an optimistic record - the
          // reserved seq, a "finding route" status, and the attachment
          // metadata - so the caller shows a live bubble that the eventual
          // auto-send (same seq, via use_seq) or the give-up event resolves.
          out_rec = MessageRecord{};
          out_rec.seq          = seq;
          out_rec.peer_hash    = dest_hash;
          out_rec.title        = title;
          out_rec.content      = content;
          out_rec.body_size    = (uint32_t)content.size();
          out_rec.incoming     = false;
          out_rec.signature_ok = true;
          out_rec.status       = OutboxStatus::FindingRoute;
          out_rec.boot_epoch   = Web::BootCounter::current();
          out_rec.received_ms  = (uint32_t)millis();
          // LXMF wall-clock ts, same source a dispatched send uses. Without it
          // the optimistic record carries ts=0, which sorts it to the very top
          // of the thread (msgCmp orders by ts first) - out of view below the
          // auto-scrolled bottom, so the bubble looks like it never appeared.
          out_rec.ts           = a->lxmf.get_timestamp();
          if (attachments) {
            for (const auto& at : *attachments) {
              AttachmentMeta m;
              m.tag      = at.tag;
              m.size     = (uint32_t)at.byte_count();
              m.filename = at.filename;
              m.mime     = at.mime;
              out_rec.attachments.push_back(std::move(m));
            }
          }
          if (out_queued) *out_queued = true;
          if (out_err) *out_err = "Finding a route to the recipient. The message will send automatically once it's known.";
          return false;
        }
        // Queue full. Attachment-free sends can't recover, so report it;
        // attachment sends fall through to send_message, which frees the
        // staging on its own no-route failure rather than leaking it.
        if (attachments == nullptr || attachments->empty()) {
          if (out_err) *out_err = "No route to the recipient yet and the auto-send queue is full. Please resend in a moment.";
          return false;
        }
      }
      // Delivery stamps: when the recipient's latest announce carries a
      // stamp cost, the message is prepared (payload + message id frozen)
      // and parked while the background worker searches for a stamp -
      // mirrors upstream LXMRouter.handle_outbound auto-applying the
      // cached cost and deferring to pending_deferred_stamps. The outbox
      // record is appended NOW with GeneratingStamp status so it shows in
      // the UI and survives a reboot; dispatch later mutates it in place.
      const uint8_t required_cost = LXMFMinimal::peer_stamp_cost(dest_hash);
      if (required_cost >= 1) {
        // Telemetry-only sends DO get an outbox record on this path:
        // the deferred-stamp machinery resumes and fails by outbox seq,
        // so skipping the record would orphan those flows. The cost is
        // a visible telemetry message per interval in the conversation
        // with a stamp-costed collector - rare, and visible rather
        // than silently wrong.
        return send_with_stamp(a, iden_id, dest_hash, title, content,
                               attachments, required_cost, out_rec,
                               out_err, use_seq, extra);
      }
      if (!a->lxmf.send_message(dest_hash, title, content, attachments, out_rec, out_err, extra)) {
        return false;
      }
      if (a->outbox && !machinery_send) {
        // When this is the queue's auto-send, reuse the seq the optimistic
        // bubble already has so the SPA merges rather than duplicating.
        if (use_seq != 0) out_rec.seq = use_seq;
        a->outbox->append(out_rec);
        // Broadcast the new outbound message so every connected client
        // (not just the one that issued the send, and including API-only
        // senders) shows the bubble live instead of only after a refresh.
        Web::WS::publish_outbound(iden_id, out_rec);
      }
      return true;
    }

    // Auto-retry queue for sends with no route to the recipient yet (see
    // send()). Each entry is re-attempted once a route arrives via the path
    // request, then dropped. Bounded + short-lived. Text bodies and (for
    // attachment sends) the staged-buffer references are copied in; the entry
    // carries the reserved outbox seq so the optimistic bubble and the real
    // record share an identity.
    struct PendingIdentitySend {
      IdentityId  iden_id;
      RNS::Bytes  dest;
      std::string title;
      std::string content;
      // Owned staged attachments (staging_id references). Empty for text
      // sends. When set, the queue is responsible for releasing the staging
      // buffers - either send_message frees them on a successful auto-send,
      // or the give-up path frees them when the window expires.
      std::vector<LXMFMinimal::OutgoingAttachment> attachments;
      // Optional non-attachment fields (telemetry blob, commands,
      // custom meta); all empty for normal sends. Bounded: a few
      // hundred bytes x PENDING_ID_MAX entries. A delayed send keeps
      // its original telemetry sample - the blob timestamps itself,
      // so a late delivery reports its real sample time.
      ExtraFields extra;
      uint32_t    outbox_seq  = 0;   // reserved seq the eventual record reuses
      uint8_t     attempts    = 0;
      uint64_t    next_at_ms  = 0;
      uint64_t    next_request_ms = 0;  // next path-request nudge (attachment entries)
    };
    static constexpr uint8_t  PENDING_ID_MAX          = 8;
    static constexpr uint8_t  PENDING_ID_MAX_ATTEMPTS = 6;
    static constexpr uint32_t PENDING_ID_BACKOFF_MS   = 4000;
    // Attachment entries re-attempt route resolution like upstream LXMF's
    // DIRECT path rather than giving up after a single PATH_REQUEST_TIMEOUT:
    // a multi-hop LoRa cold resolution routinely needs more than one 15s
    // window (the path request → response → identity recall round-trips over a
    // slow, duty-cycled link). Re-request the path at PATH_REQUEST_WAIT cadence
    // (7s, upstream's constant). The cheap has_path/recall poll runs every
    // RECHECK_MS; the path-request nudge is throttled to REQUEST_MS. Staging is
    // flash-spilled (not SRAM-pinned) and is released on the final give-up.
    // (Was a single request + hard 15s deadline with no re-issue, which dropped
    // any resolution slower than 15s.)
    //
    // MAX_ATTEMPTS deliberately exceeds upstream's MAX_DELIVERY_ATTEMPTS (5):
    // uR collapses upstream's separate pathless + delivery retry phases into
    // one has_path-gated send(), so it needs more path-request cycles to cover
    // the same wall-clock. On-rig, cold forward resolution while the bridge
    // services the rmap firehose was measured at 5–37s (residual under-load
    // LoRa packet loss, #88); 5 attempts (~35s) only just covered the worst
    // case. 8 (~56s) gives ~1.5x headroom over that without holding staging or
    // the "finding route" UI too long on a genuinely unreachable destination.
    static constexpr uint8_t  PENDING_ATTACH_MAX_ATTEMPTS = 8;     // ~56s window (> upstream's 5; see above)
    static constexpr uint32_t PENDING_ATTACH_REQUEST_MS   = 7000;  // upstream PATH_REQUEST_WAIT (7s)
    static constexpr uint32_t PENDING_ATTACH_RECHECK_MS   = 2000;
    static std::vector<PendingIdentitySend>& pending_identity_sends() {
      static std::vector<PendingIdentitySend> v;
      return v;
    }

    // --- Announce-triggered send acceleration --------------------------
    // A fresh lxmf.delivery announce from a peer we hold queued work for
    // should fire that work on the next tick instead of waiting out its
    // retry backoff / recheck interval - upstream resets
    // next_delivery_attempt the moment the announce handler sees a
    // pending destination (Handlers.py:23-32). AnnounceLog subscribers
    // may run off the main loop, so the callback only copies the raw
    // 16-byte hash into this fixed ring under a critical section (no
    // heap work, no map access); loop() drains it where every other
    // pending-map mutation already happens. Overflow drops silently -
    // acceleration is an optimisation, the regular cadence still
    // delivers.
    struct AnnouncedPeerRing {
      static constexpr size_t SLOTS = 8;
      uint8_t hash[SLOTS][16];
      uint8_t count = 0;
    };
    static AnnouncedPeerRing& announced_peers() {
      static AnnouncedPeerRing r;
      return r;
    }
    static portMUX_TYPE& announce_accel_mux() {
      static portMUX_TYPE m = portMUX_INITIALIZER_UNLOCKED;
      return m;
    }

    static void drain_peer_announcements() {
      uint8_t local[AnnouncedPeerRing::SLOTS][16];
      uint8_t n = 0;
      {
        auto& r = announced_peers();
        portENTER_CRITICAL(&announce_accel_mux());
        n = r.count;
        if (n) memcpy(local, r.hash, (size_t)n * 16);
        r.count = 0;
        portEXIT_CRITICAL(&announce_accel_mux());
      }
      if (n == 0) return;
      const uint64_t now = (uint64_t)millis();
      for (uint8_t i = 0; i < n; ++i) {
        RNS::Bytes dest(local[i], 16);
        LXMFMinimal::accelerate_pending_for(dest);
        // Same announce-driven re-fire for opportunistic sends - ports the
        // OPPORTUNISTIC arm of Handlers.py received_announce so an SX->LR
        // send recovers the instant the recipient announces.
        LXMFMinimal::accelerate_opp_for(dest);
        for (auto& e : pending_identity_sends()) {
          if (e.dest == dest && e.next_at_ms > now) e.next_at_ms = now;
        }
      }
    }

    // Called from the AnnounceLog subscriber for every fresh
    // lxmf.delivery announce (wired in the firmware setup). Safe from
    // any task; see drain_peer_announcements.
    static void note_peer_announced(const RNS::Bytes& dest) {
      if (dest.size() != 16) return;
      auto& r = announced_peers();
      portENTER_CRITICAL(&announce_accel_mux());
      bool present = false;
      for (uint8_t i = 0; i < r.count; ++i) {
        if (memcmp(r.hash[i], dest.data(), 16) == 0) { present = true; break; }
      }
      if (!present && r.count < AnnouncedPeerRing::SLOTS) {
        memcpy(r.hash[r.count], dest.data(), 16);
        r.count++;
      }
      portEXIT_CRITICAL(&announce_accel_mux());
    }

    // Returns the outbox seq reserved for this queued send, or 0 if it could
    // not be queued (queue full, or the identity has no outbox to reserve a
    // seq from). The seq lets the optimistic "finding route" bubble and the
    // eventual real record share an identity (the SPA dedups on seq).
    static uint32_t queue_pending_identity_send(
        LXMFIdentity* a,
        const IdentityId& iden,
        const RNS::Bytes& dest,
        const std::string& title,
        const std::string& content,
        const std::vector<LXMFMinimal::OutgoingAttachment>* attachments = nullptr,
        const ExtraFields* extra = nullptr) {
      auto& q = pending_identity_sends();
      const bool has_atts = (attachments != nullptr && !attachments->empty());
      const uint64_t now = (uint64_t)millis();
      // Coalesce an identical re-queue (user mashing send) - but only for
      // attachment-free entries. Two attachment sends carry distinct staging
      // buffers even with identical text, so they must stay separate; merging
      // them would orphan one set of staging ids. Reuse the coalesced entry's
      // seq so the caller's bubble still matches. A coalesced telemetry
      // entry adopts the newest sample - the older one is superseded.
      if (!has_atts) {
        for (auto& p : q) {
          if (p.attachments.empty() && p.iden_id == iden && p.dest == dest &&
              p.title == title && p.content == content) {
            p.attempts = 0;
            p.next_at_ms = now + PENDING_ID_BACKOFF_MS;
            if (extra) p.extra = *extra;
            return p.outbox_seq;
          }
        }
      }
      if (q.size() >= PENDING_ID_MAX) return 0;
      if (!a || !a->outbox) return 0;
      PendingIdentitySend p;
      p.iden_id = iden; p.dest = dest; p.title = title; p.content = content;
      if (extra) p.extra = *extra;
      p.outbox_seq = a->outbox->reserve_seq();
      if (has_atts) {
        p.attachments     = *attachments;    // take ownership of the staging ids
        p.next_at_ms      = now + PENDING_ATTACH_RECHECK_MS;
        p.next_request_ms = now + PENDING_ATTACH_REQUEST_MS;  // first nudge after the initial request
      } else {
        p.next_at_ms  = now + PENDING_ID_BACKOFF_MS;
      }
      const uint32_t seq = p.outbox_seq;
      q.push_back(std::move(p));
      return seq;
    }

    // Called from loop(): when a route to the recipient exists (a transport
    // path AND the recipient's key), send it for real (which appends the
    // outbox record + broadcasts it, so the bubble appears). Give up after
    // PENDING_ID_MAX_ATTEMPTS.
    static void tick_pending_identity_sends() {
      auto& q = pending_identity_sends();
      const uint64_t now = (uint64_t)millis();
      for (auto it = q.begin(); it != q.end(); ) {
        if (!it->attachments.empty()) {
          // Attachment entry. Re-request the path up to
          // PENDING_ATTACH_MAX_ATTEMPTS at PATH_REQUEST_WAIT cadence (mirrors
          // upstream LXMF DIRECT retry) instead of a single 15s give-up - a
          // multi-hop LoRa cold resolution often needs more than one window.
          // The cheap has_path/recall poll runs every recheck; the
          // path-request nudge is throttled to REQUEST_MS. Staging
          // (flash-spilled) is freed only on the final give-up.
          if (now < it->next_at_ms) { ++it; continue; }
          if (RNS::Transport::has_path(it->dest) && RNS::Identity::recall(it->dest)) {
            MessageRecord rec;
            const char* err = nullptr;
            // Route is up. send_message consumes and frees the staging buffers
            // (StagingReleaser), handing ownership back. The route is present
            // and the loop is cooperative (no yield before send()'s own
            // check), so send() can't re-enter the no-route gate and re-queue.
            if (send(it->iden_id, it->dest, it->title, it->content, &it->attachments,
                     rec, &err, nullptr, it->outbox_seq,
                     it->extra.empty() ? nullptr : &it->extra)) {
              NOTICEF("LXMF: auto-sent queued attachment message to %s once a route arrived",
                      it->dest.toHex().c_str());
            } else {
              WARNINGF("LXMF: auto-send of attachment message to %s failed after a route arrived: %s",
                       it->dest.toHex().c_str(), err ? err : "unknown");
              Web::WS::publish_outbox_status_seq(it->iden_id, it->outbox_seq, "failed", RNS::Bytes());
            }
            it = q.erase(it);
            continue;
          }
          if (now >= it->next_request_ms) {
            if (++it->attempts >= PENDING_ATTACH_MAX_ATTEMPTS) {
              for (auto& a : it->attachments)
                if (a.staging_id) Storage::OutboundStaging::release(a.staging_id);
              // Tell the SPA the "finding route" bubble has given up, keyed by
              // its seq (the message was never sent, so there's no packet hash).
              Web::WS::publish_outbox_status_seq(it->iden_id, it->outbox_seq, "failed", RNS::Bytes());
              WARNINGF("LXMF: route never resolved for attachment message to %s after %u path requests - freed staging",
                       it->dest.toHex().c_str(), (unsigned)it->attempts);
              it = q.erase(it);
              continue;
            }
            RNS::Transport::request_path(it->dest);  // nudge the path request again
            it->next_request_ms = now + PENDING_ATTACH_REQUEST_MS;
          }
          it->next_at_ms = now + PENDING_ATTACH_RECHECK_MS;
          ++it;
          continue;
        }

        // Text entry - lenient attempt-count retry with a path-request nudge.
        if (now < it->next_at_ms) { ++it; continue; }
        if (RNS::Transport::has_path(it->dest) && RNS::Identity::recall(it->dest)) {
          MessageRecord rec;
          const char* err = nullptr;
          if (send(it->iden_id, it->dest, it->title, it->content, nullptr,
                   rec, &err, nullptr, it->outbox_seq,
                   it->extra.empty() ? nullptr : &it->extra)) {
            NOTICEF("LXMF: auto-sent queued message to %s once its key arrived",
                    it->dest.toHex().c_str());
          } else {
            WARNINGF("LXMF: auto-send to %s failed after key arrived: %s",
                     it->dest.toHex().c_str(), err ? err : "unknown");
            Web::WS::publish_outbox_status_seq(it->iden_id, it->outbox_seq, "failed", RNS::Bytes());
          }
          it = q.erase(it);
        } else if (++it->attempts >= PENDING_ID_MAX_ATTEMPTS) {
          WARNINGF("LXMF: giving up auto-send to %s - key never arrived after %u tries",
                   it->dest.toHex().c_str(), (unsigned)it->attempts);
          Web::WS::publish_outbox_status_seq(it->iden_id, it->outbox_seq, "failed", RNS::Bytes());
          it = q.erase(it);
        } else {
          RNS::Transport::request_path(it->dest);  // nudge the path request again
          it->next_at_ms = now + (uint64_t)PENDING_ID_BACKOFF_MS * it->attempts;
          ++it;
        }
      }
    }

    // ---------------- deferred delivery-stamp sends ----------------
    // A send whose recipient requires a delivery stamp. The outbox
    // record (status GeneratingStamp) was appended at send() time; the
    // frozen payload lives in an on-disk sidecar (SD first, flash
    // fallback - the same policy as attachment persistence) so the
    // multi-second proof-of-work holds no payload bytes in RAM and the
    // send survives a reboot. `payload` is only populated as a RAM
    // fallback when the sidecar write failed. Mirrors upstream
    // LXMRouter.pending_deferred_stamps + process_deferred_stamps: one
    // generation in flight, FIFO, dispatch once the stamp lands.
    struct PendingStampSend {
      IdentityId  iden_id;
      RNS::Bytes  dest;
      RNS::Bytes  message_id;        // 32-byte stamp material (== message hash)
      uint32_t    outbox_seq = 0;
      uint8_t     cost = 0;
      std::string sidecar;           // on-disk payload copy ("" = RAM fallback)
      Storage::SdWriter::Handle sidecar_handle = 0;  // in-flight off-task SD stream write (0 = resolved/none)
      RNS::Bytes  payload;           // held until the sidecar write confirms; kept as fallback if it failed
      bool        submitted = false; // generation handed to the PoW worker
      RNS::Bytes  stamp;             // non-empty once generated → dispatch phase
      uint32_t    stamp_value = 0;
      uint8_t     dispatch_attempts = 0;
      uint64_t    next_dispatch_ms  = 0;
    };
    static constexpr size_t   PENDING_STAMP_MAX           = 8;
    static constexpr uint8_t  STAMP_DISPATCH_MAX_ATTEMPTS = 6;
    static constexpr uint32_t STAMP_DISPATCH_BACKOFF_MS   = 4000;
    static std::deque<PendingStampSend, PsAlloc<PendingStampSend>>& pending_stamp_sends() {
      static std::deque<PendingStampSend, PsAlloc<PendingStampSend>> q;
      return q;
    }

    // Sidecar layout: "LXSP" | version(1) | dest(16) | cost(1) |
    // message_id(32) | payload. The message id is stored so a boot-time
    // resume doesn't have to re-read the whole payload just to derive
    // the stamp material; send_prepared() re-verifies payload-vs-id
    // before anything goes on the wire.
    static constexpr size_t STAMP_SIDECAR_HDR = 4 + 1 + 16 + 1 + 32;

    static std::string pending_stamp_sidecar_path(const LXMFIdentity& a, uint32_t seq) {
      return a.dir() + "/pending_stamp_" + std::to_string(seq) + ".bin";
    }

    // Fill the fixed sidecar header in `hdr` (STAMP_SIDECAR_HDR bytes): "LXSP" |
    // version(1) | dest(16) | cost(1) | message_id(32). Returns false on a wrong
    // dest/message_id size. The SD-stream write, the inline-fallback write and the
    // read path all share this one layout, so it lives in a single place.
    static bool fill_stamp_header(uint8_t* hdr, const RNS::Bytes& dest,
                                  uint8_t cost, const RNS::Bytes& message_id) {
      if (dest.size() != 16 || message_id.size() != 32) return false;
      memcpy(hdr, "LXSP", 4);
      hdr[4] = 1;
      memcpy(hdr + 5, dest.data(), 16);
      hdr[21] = cost;
      memcpy(hdr + 22, message_id.data(), 32);
      return true;
    }

    static bool write_pending_stamp_sidecar(const std::string& path,
                                            const RNS::Bytes& dest,
                                            uint8_t cost,
                                            const RNS::Bytes& message_id,
                                            const RNS::Bytes& payload) {
      if (payload.size() == 0) return false;
      uint8_t hdr[STAMP_SIDECAR_HDR];
      if (!fill_stamp_header(hdr, dest, cost, message_id)) return false;
      const size_t total = sizeof(hdr) + payload.size();
      // Chunked writer: header bytes first, then payload straight from
      // the (PSRAM-backed) Bytes buffer - no contiguous header+payload
      // copy is ever materialised.
      auto reader = [&](uint8_t* dst, size_t off, size_t want) -> size_t {
        size_t filled = 0;
        while (filled < want && off + filled < total) {
          const size_t pos = off + filled;
          if (pos < sizeof(hdr)) {
            dst[filled++] = hdr[pos];
          } else {
            const size_t poff = pos - sizeof(hdr);
            const size_t take = std::min(want - filled, payload.size() - poff);
            memcpy(dst + filled, payload.data() + poff, take);
            filled += take;
          }
        }
        return filled;
      };
      if (Storage::SDCard::present()) {
        if (Storage::Streaming::write_streamed(path.c_str(), true, total, reader) == total) {
          return true;
        }
        if (SD.exists(path.c_str())) SD.remove(path.c_str());
        WARNINGF("LXMF: SD sidecar write failed for %s - falling back to flash", path.c_str());
      }
      if (Storage::Streaming::write_streamed(path.c_str(), false, total, reader) == total) {
        return true;
      }
      filesystem.remove(path.c_str());
      return false;
    }

    // Start an off-task SD stream write of the sidecar (header + payload, the
    // payload fed straight from the PSRAM Bytes). The shared writer drains it on
    // its own ring (MAX_STREAMS=2, so it coexists with an upload) off the send
    // task. Returns the job handle to poll for completion, or 0 if SD is absent
    // or no ring is free, so the caller falls back to the inline write.
    static Storage::SdWriter::Handle begin_sd_stamp_sidecar(
        const std::string& path, const RNS::Bytes& dest, uint8_t cost,
        const RNS::Bytes& message_id, const RNS::Bytes& payload) {
      if (!Storage::SDCard::present()) return 0;
      if (payload.size() == 0) return 0;
      uint8_t hdr[STAMP_SIDECAR_HDR];
      if (!fill_stamp_header(hdr, dest, cost, message_id)) return 0;
      Storage::SdWriter::Handle h = Storage::SdWriter::open(
          path.c_str(), Storage::SdWriter::Op::Truncate, 0,
          Storage::SdWriter::Kind::Sidecar, Storage::SdWriter::PRIO_NORMAL,
          /*want_sha=*/false, /*keep=*/true);
      if (!h) return 0;
      if (!Storage::SdWriter::feed(h, hdr, sizeof(hdr)) ||
          !Storage::SdWriter::feed(h, payload.data(), payload.size())) {
        Storage::SdWriter::release(h);
        return 0;
      }
      Storage::SdWriter::finish(h);
      return h;
    }

    // Read a sidecar back. out_payload may be null for a header-only
    // read (boot resume). The payload lands in a PSRAM-backed Bytes
    // buffer, filled in 4 KiB chunks.
    static bool read_pending_stamp_sidecar(const std::string& path,
                                           RNS::Bytes* out_dest,
                                           uint8_t*    out_cost,
                                           RNS::Bytes* out_message_id,
                                           RNS::Bytes* out_payload) {
      const bool on_sd = Storage::SDCard::present() && Storage::SDCard::exists(path.c_str());
      File sd_f;
      microStore::File fl_f;
      size_t fsize = 0;
      if (on_sd) {
        sd_f = SD.open(path.c_str(), FILE_READ);
        if (!sd_f) return false;
        fsize = sd_f.size();
      } else {
        if (!filesystem.exists(path.c_str())) return false;
        fl_f = filesystem.open(path.c_str(), microStore::File::ModeRead);
        if (!fl_f) return false;
        fsize = filesystem.size(path.c_str());
      }
      auto close_all = [&]() { if (on_sd) sd_f.close(); else fl_f.close(); };
      auto read_some = [&](uint8_t* dst, size_t want) -> size_t {
        return on_sd ? (size_t)sd_f.read(dst, want) : (size_t)fl_f.read(dst, want);
      };
      uint8_t hdr[STAMP_SIDECAR_HDR];
      if (fsize <= sizeof(hdr) || read_some(hdr, sizeof(hdr)) != sizeof(hdr)
          || memcmp(hdr, "LXSP", 4) != 0 || hdr[4] != 1) {
        close_all();
        return false;
      }
      if (out_dest)       *out_dest       = RNS::Bytes(hdr + 5, 16);
      if (out_cost)       *out_cost       = hdr[21];
      if (out_message_id) *out_message_id = RNS::Bytes(hdr + 22, 32);
      bool ok = true;
      if (out_payload) {
        const size_t plen = fsize - sizeof(hdr);
        uint8_t* dst = out_payload->writable(plen);
        if (dst == nullptr) {
          ok = false;
        } else {
          size_t off = 0;
          while (off < plen) {
            const size_t want = std::min((size_t)4096, plen - off);
            const size_t got = read_some(dst + off, want);
            if (got == 0) break;
            off += got;
            RNS::Utilities::OS::reset_watchdog();
          }
          ok = (off == plen);
        }
      }
      close_all();
      return ok;
    }

    static void remove_pending_stamp_sidecar(const std::string& path) {
      if (path.empty()) return;
      if (Storage::SDCard::present() && Storage::SDCard::exists(path.c_str())) {
        SD.remove(path.c_str());
      }
      if (filesystem.exists(path.c_str())) filesystem.remove(path.c_str());
    }

    // Terminal failure of a pending stamped send: flip the outbox record
    // to Failed, tell the SPA (seq-keyed - the record never had a packet
    // hash), and drop the payload sidecar.
    static void fail_pending_stamp(LXMFIdentity& a, const PendingStampSend& f, const char* why) {
      WARNINGF("LXMF: stamped send to %s (seq %lu) failed: %s",
               f.dest.toHex().c_str(), (unsigned long)f.outbox_seq,
               why ? why : "unknown");
      if (a.outbox) {
        a.outbox->mutate_by_seq(f.outbox_seq, [](MessageRecord& m) {
          m.status = OutboxStatus::Failed;
        });
      }
      Web::WS::publish_outbox_status_seq(f.iden_id, f.outbox_seq, "failed", RNS::Bytes());
      if (f.sidecar_handle) Storage::SdWriter::release(f.sidecar_handle);
      remove_pending_stamp_sidecar(f.sidecar);
    }

    // Clean cancel for a pending stamped send whose outbox record is
    // being removed (conversation clear, retention eviction). Aborts
    // the worker job if it's this entry's, and drops the sidecar. The
    // record itself is already on its way out, so no status mutation
    // or WS frame is needed.
    static void cancel_pending_stamp(const IdentityId& iden_id, uint32_t seq) {
      auto& q = pending_stamp_sends();
      for (auto it = q.begin(); it != q.end(); ++it) {
        if (it->iden_id != iden_id || it->outbox_seq != seq) continue;
        Discovery::Stamp::cancel(it->message_id);
        if (it->sidecar_handle) Storage::SdWriter::release(it->sidecar_handle);
        remove_pending_stamp_sidecar(it->sidecar);
        NOTICEF("LXMF: cancelled stamp generation for outbox seq %lu (record removed)",
                (unsigned long)seq);
        q.erase(it);
        return;
      }
    }

    // The stamped variant of send(): freeze the payload, append the
    // outbox record in GeneratingStamp state, persist the payload
    // sidecar, and queue the proof-of-work. Dispatch happens from
    // tick_pending_stamp_sends() once the stamp is found.
    static bool send_with_stamp(LXMFIdentity* a,
                                const IdentityId& iden_id,
                                const RNS::Bytes& dest_hash,
                                const std::string& title,
                                const std::string& content,
                                const std::vector<LXMFMinimal::OutgoingAttachment>* attachments,
                                uint8_t cost,
                                MessageRecord& out_rec,
                                const char** out_err,
                                uint32_t use_seq,
                                const ExtraFields* extra = nullptr) {
      // Ownership of staged attachment buffers passes to this call (the
      // same contract as send_message): prepare_message releases them on
      // every one of its exit paths, so only the two early-outs BEFORE
      // prepare_message have to release explicitly.
      auto release_staging = [&]() {
        if (!attachments) return;
        for (const auto& at : *attachments) {
          if (at.staging_id) Storage::OutboundStaging::release(at.staging_id);
        }
      };
      auto& q = pending_stamp_sends();
      if (q.size() >= PENDING_STAMP_MAX) {
        release_staging();
        if (out_err) *out_err = "Too many messages are already waiting on stamp generation. Please resend in a moment.";
        return false;
      }
      if (!a->outbox) {
        release_staging();
        if (out_err) *out_err = "This identity has no outbox spool.";
        return false;
      }
      LXMFMinimal::PreparedMessage pm;
      if (!a->lxmf.prepare_message(dest_hash, title, content, attachments, pm,
                                   out_rec, out_err, extra)) {
        return false;
      }
      out_rec.status = OutboxStatus::GeneratingStamp;
      if (use_seq != 0) out_rec.seq = use_seq;
      a->outbox->append(out_rec);  // assigns a seq if 0; persists the state
      PendingStampSend p;
      p.iden_id    = iden_id;
      p.dest       = dest_hash;
      p.message_id = pm.message_id;
      p.outbox_seq = out_rec.seq;
      p.cost       = cost;
      const std::string path = pending_stamp_sidecar_path(*a, out_rec.seq);
      Storage::SdWriter::Handle sc_h =
          begin_sd_stamp_sidecar(path, dest_hash, cost, pm.message_id, pm.payload);
      if (sc_h) {
        // Off-task SD stream write in flight. Hold the payload in PSRAM until
        // tick confirms it landed (then freed - the same "no payload in RAM
        // during PoW" outcome, resolved one tick later), or kept as the RAM
        // fallback if it failed.
        p.sidecar        = path;
        p.sidecar_handle = sc_h;
        p.payload        = pm.payload;
      } else if (write_pending_stamp_sidecar(path, dest_hash, cost, pm.message_id, pm.payload)) {
        // No SD (synchronous flash) or no ring free (inline SD fallback): the
        // payload is on disk, so the RAM is freed now.
        p.sidecar = path;
      } else {
        // Disk full / no backend: hold the payload in PSRAM. The send still
        // completes, it just will not survive a reboot mid-search.
        WARNING("LXMF: pending-stamp sidecar write failed - holding payload in PSRAM");
        p.payload = pm.payload;
      }
      q.push_back(std::move(p));
      NOTICEF("LXMF: send to %s deferred for stamp generation (cost %u, seq %lu)",
              dest_hash.toHex().c_str(), (unsigned)cost, (unsigned long)out_rec.seq);
      Web::WS::publish_outbound(iden_id, out_rec);
      return true;
    }

    // Called from loop(): advance the front pending stamped send.
    // Generation phase: hand the job to the shared PoW worker (which
    // may be busy with a discovery announce or an inbound validation -
    // bounce and retry next tick) and poll for its result. Dispatch
    // phase: reload the payload from the sidecar and send it with the
    // stamp as the 5th payload element. Only the front entry is ever
    // touched, so the queue is strictly FIFO with one generation in
    // flight, matching upstream's stamp_gen_lock serialisation.
    static void tick_pending_stamp_sends() {
      auto& q = pending_stamp_sends();
      if (q.empty()) return;
      auto& f = q.front();
      LXMFIdentity* a = identity_by_id_mut(f.iden_id);
      if (!a || !a->outbox) {
        // Identity deleted while queued. Abort + drop.
        Discovery::Stamp::cancel(f.message_id);
        if (f.sidecar_handle) Storage::SdWriter::release(f.sidecar_handle);
        remove_pending_stamp_sidecar(f.sidecar);
        q.pop_front();
        return;
      }
      const uint64_t now = (uint64_t)millis();
      // Resolve the off-task sidecar write before anything else: once it is
      // durable, free the held payload (frees RAM for the multi-second PoW); if
      // it failed, keep the payload as the RAM fallback. While it is still in
      // flight (ms) wait, so dispatch never races the write and the ring handle
      // is always released here (never leaked).
      if (f.sidecar_handle) {
        const int w = Storage::SdWriter::poll(f.sidecar_handle);
        if (w < 0) return;                       // still writing; resolve next tick
        Storage::SdWriter::release(f.sidecar_handle);
        f.sidecar_handle = 0;
        if (w == 1) f.payload = RNS::Bytes();     // durable on disk; free the RAM
        else        f.sidecar.clear();            // write failed; f.payload is the fallback
      }
      if (f.stamp.size() == 0) {
        if (!f.submitted) {
          f.submitted = Discovery::Stamp::submit_lxmf_generate(f.message_id, f.cost);
          return;
        }
        Discovery::Stamp::JobResult r;
        if (!Discovery::Stamp::take_result_if(f.message_id, r)) return;  // still searching
        if (!r.ok || r.stamp.size() == 0) {
          fail_pending_stamp(*a, f, "stamp generation was cancelled or could not allocate its workblock");
          q.pop_front();
          return;
        }
        f.stamp       = r.stamp;
        f.stamp_value = r.value;
        // Fall through to dispatch immediately.
      }
      if (now < f.next_dispatch_ms) return;
      LXMFMinimal::PreparedMessage pm;
      pm.message_id = f.message_id;
      pm.payload    = f.payload;  // RAM fallback (usually empty)
      if (pm.payload.size() == 0 && !f.sidecar.empty()) {
        RNS::Bytes side_mid;
        if (!read_pending_stamp_sidecar(f.sidecar, nullptr, nullptr, &side_mid, &pm.payload)
            || !(side_mid == f.message_id)) {
          fail_pending_stamp(*a, f, "the stored message payload is missing or corrupt");
          q.pop_front();
          return;
        }
      }
      if (pm.payload.size() == 0) {
        fail_pending_stamp(*a, f, "the prepared payload is no longer available");
        q.pop_front();
        return;
      }
      MessageRecord rec;
      const char* err = nullptr;
      if (!a->lxmf.send_prepared(f.dest, pm, f.stamp, rec, &err)) {
        // Usually a route that lapsed during the multi-second search -
        // send_prepared has already issued a fresh path request. Back
        // off and retry; the generated stamp is kept (it's bound to the
        // message id, not the route).
        if (++f.dispatch_attempts >= STAMP_DISPATCH_MAX_ATTEMPTS) {
          fail_pending_stamp(*a, f, err ? err : "dispatch failed");
          q.pop_front();
        } else {
          f.next_dispatch_ms = now + (uint64_t)STAMP_DISPATCH_BACKOFF_MS * f.dispatch_attempts;
        }
        return;
      }
      // Dispatched. Flip the GeneratingStamp record into its sent form
      // in place (the record predates the send, so dispatch must update,
      // not append). stamp_value is recorded on the sender side too -
      // the SPA shows the work that was attached to the message.
      const int16_t value = f.stamp_value > 0x7FFF ? (int16_t)0x7FFF : (int16_t)f.stamp_value;
      a->outbox->mutate_by_seq(f.outbox_seq, [&](MessageRecord& m) {
        m.status        = rec.status;
        m.packet_hash   = rec.packet_hash;
        m.stamp_checked = true;
        m.stamp_valid   = true;
        m.stamp_value   = value;
      });
      NOTICEF("LXMF: stamped send to %s dispatched (seq %lu, stamp value %u)",
              f.dest.toHex().c_str(), (unsigned long)f.outbox_seq,
              (unsigned)f.stamp_value);
      Web::WS::publish_outbox_status_seq(f.iden_id, f.outbox_seq,
                                         outbox_status_name(rec.status),
                                         rec.packet_hash);
      remove_pending_stamp_sidecar(f.sidecar);
      q.pop_front();
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
    // overrides ARE NOT touched here - by design, changing the
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
      doc["stamp_cost"]                   = a.stamp_cost;
      doc["enforce_stamps"]               = a.enforce_stamps;
      doc["screen"]                       = a.screen;
      doc["msg_flash"]                    = a.msg_flash;
      doc["tel_location"]                 = a.telemetry_location;
      doc["tel_environment"]              = a.telemetry_environment;
      doc["tel_battery"]                  = a.telemetry_battery;
      doc["tel_compass"]                  = a.telemetry_compass;
      doc["tel_share_s"]                  = a.telemetry_share_s;
      doc["tel_rate_s"]                   = a.telemetry_rate_s;
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
      const uint32_t sc = (uint32_t)(doc["stamp_cost"] | 0);
      a.stamp_cost     = (sc >= 1 && sc <= 254) ? (uint8_t)sc : 0;
      a.enforce_stamps = (bool)(doc["enforce_stamps"] | false);
      a.screen         = (bool)(doc["screen"] | false);
      a.msg_flash      = (bool)(doc["msg_flash"] | true);
      a.telemetry_location    = (bool)(doc["tel_location"]    | true);
      a.telemetry_environment = (bool)(doc["tel_environment"] | false);
      a.telemetry_battery     = (bool)(doc["tel_battery"]     | false);
      a.telemetry_compass     = (bool)(doc["tel_compass"]     | false);
      a.telemetry_share_s     = (uint32_t)(doc["tel_share_s"] | 0);
      a.telemetry_rate_s       = (uint32_t)(doc["tel_rate_s"]  | 60);
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
      // Body-spill machinery removed - bodies are always inline in
      // the JSONL ring (capped at LXMF_MAX_BODY_BYTES on the send
      // path). The earlier per-message spill files + lazy /body
      // endpoint were dead infrastructure: the SPA never called the
      // endpoint, so large bodies were silently inaccessible from
      // the UI even though the bytes were on disk.

      auto on_remove = [adir, p = &a](const MessageRecord& rec) {
        // An outbox record evicted while still waiting on stamp
        // generation (conversation clear, retention): abort the
        // proof-of-work job cleanly and drop its payload sidecar.
        if (!rec.incoming && rec.status == OutboxStatus::GeneratingStamp) {
          cancel_pending_stamp(p->id, rec.seq);
          remove_pending_stamp_sidecar(
              adir + "/pending_stamp_" + std::to_string(rec.seq) + ".bin");
        }
        for (const auto& att : rec.attachments) {
          if (att.filename.empty()) continue;
          const std::string full = adir + "/attachments/" + att.filename.c_str();
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
      // Inbound stamp policy from meta.json - must land before the
      // first announce so peers see the cost in app_data element [1].
      a.lxmf.set_stamp_cost(a.stamp_cost);
      a.lxmf.set_enforce_stamps(a.enforce_stamps);
      LXMFIdentity* p = &a;
      a.lxmf.set_delivery_callback([p](const MessageRecord& rec) {
        if (!p->active || !p->inbox) return;
        // Sideband-style command messages (e.g. telemetry requests)
        // act, they don't display.
        if (rec.commands.size() > 0) {
          TelemetryShare::on_commands(p->id, rec.peer_hash, rec.commands);
        }
        // Live-update offer riding in FIELD_CUSTOM_META: the sender
        // granted us a window + rate; start (or stop) the request loop.
        if (rec.custom_meta.size() > 0) {
          TelemetryShare::on_live_offer(p->id, rec.peer_hash, rec.custom_meta);
        }
        // Any telemetry from a peer we hold a live feed for refreshes
        // that feed's latest readings (and the SPA strip).
        if (rec.telemetry.size() > 0) {
          TelemetryShare::on_telemetry(p->id, rec.peer_hash, rec.telemetry);
        }
        // Messages that are ONLY machinery - commands, or a bare
        // telemetry update with no text and no files - stay out of
        // the inbox, like Sideband keeps them out of its
        // conversation. Their payloads were already consumed above /
        // by the telemetry store. A deliberately composed
        // telemetry-only message carries the urtn_msg marker in its
        // custom meta and lands in the inbox like an image-only one.
        const bool machinery_only =
            rec.content.size() == 0 && rec.attachments.empty()
            && (rec.commands.size() > 0 || rec.has_telemetry)
            && !TelemetryShare::meta_marks_message(rec.custom_meta);
        if (machinery_only) return;
        MessageRecord local = rec;  // copy so we can mutate seq
        local.seq = 0;
        local.commands    = RNS::Bytes{};   // transient; not for storage
        local.custom_meta = RNS::Bytes{};
        // Live position share: if this message started a live feed (it carried
        // an urtn_live offer, so we now hold a feed) and has a position, begin
        // a GPX track - create the file with this first point and attach it to
        // the record, then grow it one <trkpt> per later sample
        // (TelemetryShare::on_telemetry). It persists + renders like any .gpx.
        if (rec.has_telemetry && TelemetryShare::has_feed(p->id, rec.peer_hash)) {
          double lat, lon;
          if (TelemetryShare::location_from_blob(rec.telemetry, lat, lon)) {
            const uint32_t seq = p->inbox->reserve_seq();
            local.seq = seq;
            const bool use_sd = Storage::SDCard::present();
            const std::string att_dir = p->dir() + "/attachments";
            if (!filesystem.isDirectory(att_dir.c_str())) filesystem.mkdir(att_dir.c_str());
            char name[80];
            snprintf(name, sizeof(name), "%s_%08x.bin",
                     rec.peer_hash.toHex().c_str(), (unsigned)seq);
            const std::string full = att_dir + "/" + name;
            RNS::Utilities::OS::reset_watchdog();
            const size_t gsz = LXMF::GpxTrack::create(full.c_str(), use_sd, lat, lon);
            RNS::Utilities::OS::reset_watchdog();
            if (gsz > 0) {
              AttachmentMeta meta;
              meta.tag          = LXMF::FIELD_FILE_ATTACHMENTS;
              meta.size         = (uint32_t)gsz;
              meta.filename     = name;
              meta.display_name = LXMF::GpxTrack::download_name(seq);
              meta.mime         = "application/gpx+xml";
              meta.backend      = use_sd ? "sd" : "flash";
              local.attachments.push_back(meta);
              TelemetryShare::set_feed_gpx(p->id, rec.peer_hash, full, use_sd, lat, lon);
              NOTICEF("LXMF: live GPX track started %s (backend=%s)", name, meta.backend.c_str());
            }
          }
        }
        p->inbox->append(local);
        // Push to any WS client subscribed to this identity. SSE
        // pollers still pick up the same record via inbox->since().
        Web::WS::publish_incoming(p->id, local);
        // Screen identity: surface the sender + preview on the OLED
        // marquee (ScreenNotify.h).
        if (p->screen) screen_notify_incoming(local);
      });
      // Outbound lifecycle: link-mode sends start as Queued in the outbox
      // (see LXMFMinimal::send_message), and transition to Sent / Delivered
      // / Failed as the Link or Resource completes. The lookup key is the
      // link hash that was stamped onto the outbox record's packet_hash.
      a.lxmf.set_outbox_status_callback(
          [p](const RNS::Bytes& link_hash, OutboxStatus status) {
            if (!p->active || !p->outbox) return;
            p->outbox->update_status(link_hash, status);
            // Telemetry sends have no outbox record (see send()); their
            // receipts only land here.
            TelemetrySender::on_outbox_status(link_hash, status);
            // Update the OLED messenger's result page when this is a
            // device-button send (Messenger.h).
            Messenger::on_outbox_status(link_hash, status);
            // Push a typed status frame to any WS subscriber - gives
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
      //
      // Transfer progress is deliberately NOT surfaced on the OLED
      // ticker: Common::Status renders a single latest-wins slot, so
      // two concurrent transfers interleave their percentages into one
      // line of nonsense ("Recv 19K 42%" flapping to "Send 7K 13%" per
      // part). The ticker carries only self-contained event messages
      // until there is a status surface that can key live state per
      // transfer. The SPA keeps full per-transfer progress via
      // publish_lxmf_progress.
      a.lxmf.set_progress_callback(
          [p](const RNS::Bytes& peer_hash, const RNS::Bytes& link_hash,
              bool incoming, uint32_t bytes_done, uint32_t bytes_total) {
            if (!p->active) return;
            Web::publish_lxmf_progress(p->id, peer_hash, link_hash,
                                       incoming, bytes_done, bytes_total,
                                       /*finished=*/false);
          });
      // Inbound receive-complete: symmetric counterpart to the outbox
      // Sent/Delivered terminal in set_outbox_status_callback above.
      // Fires when the receiving Resource finishes streaming bytes,
      // BEFORE the decrypt step - so even decrypt-fail / malformed-
      // payload cases get a terminal message_complete on the wire and
      // the SPA's synthetic "Incoming attachment …" row + topbar strip
      // clear correctly. peer_hash is empty here (the LXMF source hash
      // is only known post-decrypt); the SPA keys the clear off the
      // link hash that earlier message_progress events also carried.
      a.lxmf.set_receive_complete_callback(
          [p](const RNS::Bytes& link_hash, uint32_t bytes_total, bool /*ok*/) {
            if (!p->active) return;
            Web::publish_lxmf_progress(
                p->id, /*peer=*/RNS::Bytes{}, link_hash, /*incoming=*/true,
                /*bytes_done=*/bytes_total, /*bytes_total=*/bytes_total,
                /*finished=*/true);
          });
      // Attachment persistence - when an incoming LXMF message carries
      // FIELD_FILE_ATTACHMENTS / FIELD_IMAGE / FIELD_AUDIO blobs, write
      // each one to <identity_dir>/attachments/ (routing: SD if a
      // card is mounted, else LittleFS). On-disk filenames are always
      // "<msg_hash_hex>_<tag>_<idx>.bin". The sender-supplied filename
      // (Sideband convention, populated by the FieldBlob) is propagated
      // onto AttachmentMeta.display_name for the SPA to use as the
      // download-prompt label.
      a.lxmf.set_attachment_persist_callback(
          [p](const RNS::Bytes& msg_hash,
              const std::vector<LXMFMinimal::FieldBlob>& fields) -> LXMF::PsVector<AttachmentMeta> {
            LXMF::PsVector<AttachmentMeta> out;
            if (!p->active) return out;
            const bool use_sd = Storage::SDCard::present();
            const std::string att_dir = p->dir() + "/attachments";
            // LittleFS-side directory still gets prepared even when SD
            // is mounted - small attachments (or fallback) land here.
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
                  WARNINGF("LXMF: SD attachment write short (wrote %u/%u for %s) - falling back to flash",
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
      // Outbound attachment persistence - copy the staging bytes to
      // the sender's identity dir so their own chat bubble can show
      // an inline preview of what they sent. Same filename layout as
      // the inbound path; the SPA renders both from the same endpoint.
      a.lxmf.set_outbound_persist_callback(
          [p](const RNS::Bytes& msg_hash,
              const std::vector<LXMFMinimal::OutgoingAttachment>& outgoing)
              -> LXMF::PsVector<AttachmentMeta> {
            LXMF::PsVector<AttachmentMeta> out;
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
                else WARNINGF("LXMF: SD outbound write short (%u/%u) for %s - fallback to flash",
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
      // Resume sends a reboot interrupted mid-stamp-generation: every
      // outbox record still in GeneratingStamp has (should have) a
      // payload sidecar on disk - re-queue the proof-of-work from it.
      // Records whose sidecar is gone (SD removed, write failed before
      // the reboot, RAM-fallback entry) flip to Failed so the UI never
      // shows a perpetual "generating stamp" that can't complete.
      {
        std::vector<uint32_t> resume_seqs;
        for (const auto& rec : a.outbox->ring()) {
          if (!rec.incoming && rec.status == OutboxStatus::GeneratingStamp) {
            resume_seqs.push_back(rec.seq);
          }
        }
        for (uint32_t seq : resume_seqs) {
          const std::string path = pending_stamp_sidecar_path(a, seq);
          RNS::Bytes dest, mid;
          uint8_t cost = 0;
          const bool ok = pending_stamp_sends().size() < PENDING_STAMP_MAX
              && read_pending_stamp_sidecar(path, &dest, &cost, &mid, nullptr)
              && cost >= 1;
          if (!ok) {
            WARNINGF("LXMF: cannot resume stamped send (outbox seq %lu) - marking failed",
                     (unsigned long)seq);
            a.outbox->mutate_by_seq(seq, [](MessageRecord& m) {
              m.status = OutboxStatus::Failed;
            });
            remove_pending_stamp_sidecar(path);
            continue;
          }
          PendingStampSend p2;
          p2.iden_id    = a.id;
          p2.dest       = dest;
          p2.message_id = mid;
          p2.outbox_seq = seq;
          p2.cost       = cost;
          p2.sidecar    = path;
          pending_stamp_sends().push_back(std::move(p2));
          NOTICEF("LXMF: resuming stamped send (outbox seq %lu, cost %u) after reboot",
                  (unsigned long)seq, (unsigned)cost);
        }
      }
      a.last_announce_ms = 0;  // announce on first loop tick
    }

    static void load_existing_identities() {
      const char* accts = LXMF_GATEWAY_ROOT "/identities";
      // PosixFileSystem::exists() returns false for directories (it does
      // open(O_RDONLY) which fails on dirs). Use isDirectory() here so we
      // don't silently skip identity loading. Same applies in ensure_root()
      // and ensure_identity_dir().
      if (!filesystem.isDirectory(accts)) {
        NOTICEF("LXMFGateway: %s is not a directory - no identities to load", accts);
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
        // pre-ratchet builds won't have a file - that's fine, the ring
        // stays empty and will populate on first announce.
        slot->ratchets.load(slot->ratchet_path(), filesystem);
        activate(*slot);
        loaded++;
        NOTICEF("LXMFGateway: loaded identity %s (%s) → %s (ratchets=%u)",
                slot->id.c_str(), slot->display_name.c_str(),
                slot->address_hex().c_str(), (unsigned)slot->ratchets.size());
      }
      // Single-holder invariant for the screen flag. Two metas can both
      // claim it after a crash between the two write_meta calls of a
      // hand-off; keep the first loaded holder and clear the rest.
      LXMFIdentity* screen_holder = nullptr;
      for (auto& a : identities_storage()) {
        if (!a.active || !a.screen) continue;
        if (!screen_holder) { screen_holder = &a; continue; }
        WARNINGF("LXMFGateway: clearing duplicate screen flag on %s", a.id.c_str());
        a.screen = false;
        write_meta(a);
      }
      // Bring the holder's private preset store up (Messenger.h).
      if (screen_holder) {
        Messenger::on_screen_identity_changed(true, screen_holder->dir());
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
    // destination" - the announce should not include a ratchet.
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
