#pragma once

// LXMFMinimal — embedded LXMF parser/sender for microReticulum.
//
// Refactored from PR #17 on attermann/microReticulum_Firmware
// (varna9000's LXMF_Minimal.h, 641 lines). Key changes from the original:
//
//   - Multi-instance: dispatch via static std::map<destination_hash, LXMFMinimal*>
//     instead of a single static _instance. Each identity hosts its own
//     LXMFMinimal and they share the underlying static callback trampoline.
//   - Announce is explicit (not auto-fired in init), so the gateway can
//     schedule announces centrally.
//   - send_message(dest_hash, title, content) replaces send_reply()'s
//     hard-coded empty-title shape.
//   - Incoming signature verification: each incoming packet's Ed25519
//     signature is verified against the sender's recalled identity.
//     Result reported via MessageRecord::signature_ok rather than silently
//     skipped.
//   - Delivery callback is std::function<void(const MessageRecord&)> —
//     a notification, not a reply generator (PR #17 returned a string from
//     the handler to auto-reply, which makes sense for GPIO control but
//     not for a generic messaging gateway).
//   - All Serial.print(..) trace lines are routed through the RNS log
//     facility so output is consistent with the rest of the firmware.

#include <Arduino.h>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <Reticulum.h>
#include <Identity.h>
#include <Destination.h>
#include <Packet.h>
#include <Link.h>
#include <Resource.h>
#include <ResourceBuffer.h>   // for RNS::resource_tmp_path()
#include <Transport.h>
#include <Bytes.h>
#include <Log.h>
#include <Utilities/OS.h>     // for OS::open_file / OS::remove_file

#include "LXMFTypes.h"
#include "../Common/MsgPack.h"
#include "../Web/BootCounter.h"
#include "../Clock/Manager.h"
#include "../Storage/OutboundStaging.h"
#include <esp_heap_caps.h>

namespace LXMF {


  // LXMF wire-size thresholds.
  //
  // OPPORTUNISTIC_MAX = LXMF packet sent to a SINGLE destination, fits in
  // one encrypted radio packet. Empirically anything <= 295 bytes is safe
  // on our profile (Packet::ENCRYPTED_MDU ~ 399 minus header/IV margin).
  //
  // LINK_PACKET_MAX_CONTENT = LXMF packet sent inside an established Link
  // as a single DATA packet. Link::MDU is 431; LXMF_OVERHEAD is 80
  // (HASH_LEN + SIG_LEN), giving an effective 319-byte payload max with
  // some safety margin.
  //
  // Anything larger goes through a Resource transfer over the same Link.
  static constexpr size_t LXMF_OPPORTUNISTIC_MAX   = 295;
  static constexpr size_t LXMF_LINK_PACKET_MAX     = 319;

  // Per-message body-content cap. Matches the WhatsApp/Telegram
  // convention (4 KiB / ~4096 chars) — comfortably above the
  // longest text someone would reasonably type into a chat, well
  // below anything that'd stress LoRa airtime or the on-device
  // ring. Anything beyond opportunistic-max triggers a Link+Resource
  // transfer; a 4 KiB body at SF5/BW250 ≈ 1-2 s airtime, fine.
  static constexpr size_t LXMF_MAX_BODY_BYTES      = 4096;

  // Per-record trust-boundary caps. Every string field that crosses
  // either the SPA→firmware boundary or the peer→firmware boundary
  // gets a length cap so a single malicious or buggy peer can't
  // exhaust RAM with a multi-MB title or mime string. The on-disk
  // JSONL line size is bounded as a *consequence* of these caps —
  // there is no separate line-length cap any more.
  //
  // Exposed to the SPA via /api/info → `limits` so the compose UI
  // can enforce the same numbers client-side and give immediate
  // feedback when the user types past the cap.
  static constexpr size_t LXMF_MAX_TITLE_BYTES        = 256;   // Email-subject convention (RFC 2822); well above any chat-title use.
  static constexpr size_t LXMF_MAX_ATTACHMENTS        = 10;    // Per message. Sideband-style; covers realistic image+audio+files.
  static constexpr size_t LXMF_MAX_ATTACHMENT_NAME    = 256;   // Per-attachment display_name (sender-supplied).
  static constexpr size_t LXMF_MAX_ATTACHMENT_MIME    = 128;   // Per-attachment mime type. RFC 6838 type/subtype rarely > 80.

  class LXMFMinimal {
  public:
    using DeliveryCallback = std::function<void(const MessageRecord&)>;

    // Outbound status updates: called when an outbound message's lifecycle
    // changes (Queued -> Sent / Delivered / Failed). Looked up by the link
    // hash that was stored as the outbox record's packet_hash placeholder
    // at send_message time. Distinct from DeliveryCallback (which is for
    // inbound deliveries that append to the inbox).
    using OutboxStatusCallback =
        std::function<void(const RNS::Bytes& /*link_hash*/, OutboxStatus)>;

    // Per-Resource progress: fires repeatedly as parts are sent (outbound)
    // or received (inbound) during a DIRECT-mode transfer. Lets the SPA
    // show a progress bar on a message that's mid-flight. Resolution is
    // per-part — sub-second granularity at SF7/BW250k, slower at high SF.
    using ProgressCallback = std::function<void(
        const RNS::Bytes& /*peer_hash*/,
        const RNS::Bytes& /*link_hash*/,
        bool              /*incoming*/,
        uint32_t          /*bytes_done*/,
        uint32_t          /*bytes_total*/)>;

    // Inbound Resource completion — fires once when the receiving end's
    // Resource finishes streaming bytes, BEFORE the LXMF decrypt /
    // signature-verify step. Distinct from DeliveryCallback (which fires
    // only on successful decrypt) so the gateway can emit a symmetric
    // message_complete event even when decryption fails or the payload
    // is malformed — the SPA-side "Incoming attachment …" row would
    // otherwise stick at 100% forever. `ok` mirrors the Resource's
    // COMPLETE-vs-FAILED status so the SPA can distinguish "received
    // and decoded" from "received but unreadable" in future UX.
    using ReceiveCompleteCallback = std::function<void(
        const RNS::Bytes& /*link_hash*/,
        uint32_t          /*bytes_total*/,
        bool              /*ok*/)>;

    // Async state for an outbound DIRECT-mode send. We open a Link to the
    // peer in send_message and queue the wire bytes here keyed by the
    // link's hash; when the Link establishes, the static callback pops
    // the entry and dispatches the actual payload as either an in-link
    // Packet or a Resource. On completion (PROOF / Resource COMPLETE /
    // link closure / timeout) the entry is removed and the gateway is
    // notified via the delivery callback so the MessageRecord status
    // can advance.
    struct PendingLinkSend {
      // The LXMF wire payload (msgpack header + content + attachments).
      // For sends > LXMF_LINK_PACKET_MAX the wire is spilled to a file
      // under the resource-tmp dir so it doesn't dwell in PSRAM for
      // the (potentially minutes-long) link-establishment + retry
      // window. `wire_path` is set in that case; `wire` stays empty
      // and is filled on demand at link-established time. Small (<=
      // LXMF_LINK_PACKET_MAX) sends keep the wire in `wire` directly
      // since the disk round-trip would dwarf the in-RAM cost.
      RNS::Bytes   wire;
      std::string  wire_path;
      // NONE-construct the Link member so default-insertion into the
      // pending_link_sends map (via map[key] = value, which first
      // default-constructs then move-assigns) doesn't crash. The default
      // RNS::Link() constructor tries to derive Ed25519 keys from a NONE
      // destination and trips on a null _sig_prv. Type::NONE is the
      // explicit empty-object path that doesn't touch the crypto.
      RNS::Link    link{RNS::Type::NONE};
      uint64_t     started_ms = 0;
      RNS::Bytes   dest_hash;
      RNS::Bytes   resource_hash;     // set after Resource is built (if >319 B)
      uint32_t     total_bytes = 0;   // wire payload size, anchor for progress %
      LXMFMinimal* owner = nullptr;
      // Outbox-record handle: the link_hash we stamped onto the
      // MessageRecord at send_message time. Stays stable across
      // retries — even when we open a *new* Link with a new hash, the
      // outbox still keys updates off this original value, so the SPA
      // bubble survives the retry without going dark.
      RNS::Bytes   record_hash;
      // Retry budget. retries_left counts down on each failure; when
      // it reaches zero the entry is erased and status stays Failed
      // for manual user re-send. next_retry_at_ms is the millis()
      // deadline after which retry_pending entries are re-attempted.
      uint8_t      retries_left = DEFAULT_OUTBOX_RETRIES;
      bool         retry_pending = false;  // true once a failure schedules a retry
      uint64_t     next_retry_at_ms = 0;
      // Status drives the outbox transition (queued -> sent / delivered /
      // failed) reported via _on_outbox_status. We deliberately do NOT
      // keep the full MessageRecord here — its title/content strings are
      // duplicated in the outbox already, and storing them again caused
      // a heap-corruption canary trip during map-erase teardown.
      OutboxStatus status = OutboxStatus::Queued;
    };

    // In-flight opportunistic (single-packet) send. Unlike the Link path
    // there is no per-stage callback chain; instead we hold the RNS
    // PacketReceipt and poll its status from tick_opportunistic_receipts():
    // a delivery proof flips it to DELIVERED, a timeout (or Transport
    // receipt-table cull) to FAILED. record_hash is the outbox key (==
    // the packet hash) so the SPA bubble can be transitioned.
    struct PendingOppSend {
      RNS::PacketReceipt receipt{RNS::Type::NONE};
      RNS::Bytes         record_hash;
      RNS::Bytes         dest_hash;     // peer destination, for retain-on-delivery
      LXMFMinimal*       owner = nullptr;
      uint64_t           started_ms = 0;
    };
    // Defensive backstop: a receipt should resolve (proof or timeout)
    // within first_hop_timeout + per-hop allowance — tens of seconds even
    // on a multi-hop LoRa path. If one is still unresolved after this, it
    // was lost from Transport's receipt table without a terminal status;
    // fail it so the bubble doesn't hang queued forever.
    static constexpr uint64_t OPP_RECEIPT_MAX_MS = 5ULL * 60ULL * 1000ULL; // 5 min

    // Outbox-retry tuning. Both static for now — wire to per-identity
    // settings later (task #113). Backoff is 30s × attempt index, so:
    //   attempt 1: send fails -> wait 30s -> retry 1
    //   attempt 2: retry 1 fails -> wait 60s -> retry 2
    //   attempt 3: retry 2 fails -> wait 90s -> retry 3
    // After DEFAULT_OUTBOX_RETRIES exhausts the entry is dropped and
    // the user must manually retry from the SPA.
    static constexpr uint8_t  DEFAULT_OUTBOX_RETRIES = 3;
    static constexpr uint32_t RETRY_BACKOFF_MS_STEP   = 30 * 1000;

    LXMFMinimal()
      : _identity(RNS::Type::NONE),
        _destination(RNS::Type::NONE),
        _initialized(false),
        _time_offset(0),
        _time_calibrated(false),
        _display_name("LXMF Node"),
        _on_delivery(nullptr) {}

    // Register this instance under the given identity. Does NOT announce —
    // call announce() explicitly when ready.
    bool init(RNS::Identity& identity, const char* display_name = "LXMF Node") {
      _identity = identity;
      _display_name = display_name;

      _destination = RNS::Destination(
        _identity,
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        "lxmf", "delivery"
      );

      // Prove inbound opportunistic deliveries. Upstream LXMF sets the
      // delivery destination to PROVE_ALL so a single-packet message
      // generates an RNS proof back to the sender (Transport auto-proves
      // PROVE_ALL destinations on receive). Without this, a sender's
      // PacketReceipt can never reach DELIVERED, so the outbox bubble for
      // an opportunistic message would have no way to confirm delivery.
      _destination.set_proof_strategy(RNS::Type::Destination::PROVE_ALL);

      // Register this instance for dispatch by destination hash. The static
      // trampoline below looks us up on every incoming packet.
      registry()[_destination.hash()] = this;
      _destination.set_packet_callback(_static_packet_callback);

      // Accept inbound links (for DIRECT-mode LXMF deliveries) and route
      // them to the static trampoline that will register per-link callbacks.
      _destination.accepts_links(true);
      _destination.set_link_established_callback(_static_inbound_link_established);

      _initialized = true;
      NOTICEF("LXMF: identity %s delivery destination ready (%s)",
              display_name, _destination.hash().toHex().c_str());
      return true;
    }

    // Tear down the dispatch registration. Call from destructor / identity delete.
    void shutdown() {
      if (!_initialized) return;
      registry().erase(_destination.hash());
      _initialized = false;
    }

    ~LXMFMinimal() { shutdown(); }

    void set_delivery_callback(DeliveryCallback cb) { _on_delivery = std::move(cb); }
    void set_outbox_status_callback(OutboxStatusCallback cb) { _on_outbox_status = std::move(cb); }
    void set_progress_callback(ProgressCallback cb) { _on_progress = std::move(cb); }
    void set_receive_complete_callback(ReceiveCompleteCallback cb) { _on_receive_complete = std::move(cb); }

    // LXMF address of this identity (16-byte destination hash, hex-encoded).
    std::string address_hex() const {
      if (!_initialized) return "";
      return _destination.hash().toHex();
    }

    const RNS::Bytes& address() const { return _destination.hash(); }

    // Manually emit the LXMF announce packet for this destination.
    //
    // Wire format is the v0.5.0+ announce app_data layout:
    //   [ display_name : bin/nil,
    //     stamp_cost   : int/nil,
    //     supported_functionality : list<u8> ]
    //
    // The third element drives peers' compression decision. Per the
    // canonical implementation (LXMF/LXMF.py compression_support_from_app_data):
    //  - missing element 2 → peer defaults to "compression supported"
    //  - element 2 is a list → peer enables compression iff SF_COMPRESSION
    //    (0x00) is in the list
    //
    // microReticulum's Resource port doesn't support bz2 decompression
    // (`c=1` advertisements are refused) so we send an empty list at
    // index 2 to explicitly tell peers NOT to compress outgoing
    // messages to us. Without this, stock LXMF peers would auto-bz2
    // long messages and we'd reject them with RESOURCE_RCL.
    // Update the announcement label. Takes effect on the next announce()
    // call — does NOT trigger one. Callers (e.g. LXMFGateway) typically
    // emit one immediately after a rename so peers re-learn the label.
    void set_display_name(const char* name) {
      if (name) _display_name = name;
    }

    void announce() {
      if (!_initialized) return;
      uint8_t buf[128];
      size_t pos = 0;
      buf[pos++] = 0x93;  // fixarray(3)
      // [0] display_name
      size_t n = Common::MsgPack::pack_bin(&buf[pos], sizeof(buf) - pos,
                                      (const uint8_t*)_display_name.c_str(),
                                      _display_name.length());
      if (n == 0) { ERROR("LXMF: announce pack failed"); return; }
      pos += n;
      // [1] stamp_cost: nil (we don't run anti-spam stamps)
      buf[pos++] = 0xC0;
      // [2] supported_functionality: empty fixarray. No SF_COMPRESSION
      // (0x00) included → peers know not to bz2 messages to us.
      buf[pos++] = 0x90;
      RNS::Bytes app_data(buf, pos);
      _destination.announce(app_data);
      NOTICEF("LXMF: announced %s", _destination.hash().toHex().c_str());
    }

    // Send an LXMF message to a remote destination hash. Returns false if
    // the recipient identity isn't yet known (no announce seen) or pack/sign
    // failed. Status reporting on delivery is left to the caller via the
    // returned MessageRecord — Phase 1 just sets status=Sent after send().
    // Caller-owned attachment blob. `tag` is one of the FIELD_* values
    // (0x05 file / 0x06 image / 0x07 audio); `data` is the raw payload.
    // Wire encoding follows the Sideband convention:
    //   FIELD_FILE_ATTACHMENTS: [[filename, bytes], ...]    (uses `filename`)
    //   FIELD_IMAGE:            [ext, bytes]                (uses `ext`)
    //   FIELD_AUDIO:            [mode_int, bytes]           (uses `audio_mode`)
    // `mime` is local metadata only, mirrored onto the outbox record.
    struct OutgoingAttachment {
      uint8_t              tag;
      // Byte source — exactly one of these is populated:
      //   * staging_id != 0: bytes live in OutboundStaging (PSRAM or
      //     SD-backed). Read via Storage::OutboundStaging::read() during
      //     encoding. This is the only path the SPA exercises today
      //     (#130 wholesale switch).
      //   * data non-empty: bytes inlined here. Kept for code paths
      //     that don't use the staging upload (test fixtures, etc).
      uint32_t             staging_id = 0;
      uint32_t             staging_total_bytes = 0;
      std::vector<uint8_t> data;
      std::string          filename;     // FIELD_FILE_ATTACHMENTS
      std::string          ext;          // FIELD_IMAGE  e.g. "webp", "png"
      uint8_t              audio_mode = 0xFF;  // FIELD_AUDIO  (AM_CUSTOM default)
      std::string          mime;         // local-only, not on the wire

      size_t byte_count() const {
        return staging_id ? staging_total_bytes : data.size();
      }
    };

    bool send_message(const RNS::Bytes& dest_hash,
                      const std::string& title,
                      const std::string& content,
                      MessageRecord& out_rec,
                      const char** out_err = nullptr) {
      return send_message(dest_hash, title, content, nullptr, out_rec, out_err);
    }

    bool send_message(const RNS::Bytes& dest_hash,
                      const std::string& title,
                      const std::string& content,
                      const std::vector<OutgoingAttachment>* attachments,
                      MessageRecord& out_rec,
                      const char** out_err = nullptr) {
      auto fail = [&](const char* msg) { if (out_err) *out_err = msg; return false; };
      // send_message takes ownership of any staging buffers referenced
      // in `attachments` — they get released on every exit path so the
      // caller doesn't have to track them across success/failure.
      struct StagingReleaser {
        const std::vector<OutgoingAttachment>* atts;
        ~StagingReleaser() {
          if (!atts) return;
          for (const auto& a : *atts) {
            if (a.staging_id) Storage::OutboundStaging::release(a.staging_id);
          }
        }
      } releaser{attachments};
      if (!_initialized) return fail("LXMF gateway not initialised");
      if (dest_hash.size() != HASH_LEN) {
        ERROR("LXMF: send_message: destination hash must be 16 bytes");
        return fail("Destination address is the wrong length (expected 16 bytes).");
      }

      RNS::Identity remote_identity = RNS::Identity::recall(dest_hash);
      if (!remote_identity) {
        // We may have a transport path but not the recipient's public key — a
        // high-cardinality announce firehose can evict it from the identity
        // cache, and with the LoRa interface in access-point mode we no longer
        // flood announces, so keys are learned on demand. Issue a path request:
        // its response carries the recipient's announce, which re-populates the
        // public key, so a resend a few seconds later succeeds.
        // (Seamless auto-retry/queue is a follow-up; for now the caller resends.)
        RNS::Transport::request_path(dest_hash);
        WARNINGF("LXMF: identity for %s unknown — issued path request, asking caller to retry",
                 dest_hash.toHex().c_str());
        return fail("Fetching recipient's key (sent a path request) — resend in a few seconds.");
      }

      RNS::Destination remote_dest(
        remote_identity,
        RNS::Type::Destination::OUT,
        RNS::Type::Destination::SINGLE,
        "lxmf", "delivery"
      );

      // Build msgpack payload: [timestamp:f64, title:bin, content:bin, fields]
      // Heap-allocate sized to content + title + sum(attachments) + a
      // fixed overhead for the msgpack framing.
      size_t att_bytes = 0;
      size_t att_names_bytes = 0;
      if (attachments) {
        for (const auto& a : *attachments) {
          att_bytes += a.byte_count();
          att_names_bytes += a.filename.size() + a.ext.size();
        }
      }
      // 8 B per attachment for bin/array headers, +8 per element for
      // inner [name, bytes] wrapping, +map header.
      const size_t mp_cap = 96 + title.size() + content.size()
                          + att_bytes + att_names_bytes + 16 * (attachments ? attachments->size() : 0);
      // Large attachments (multi-MB) won't fit in DRAM. Allocate the
      // working buffer from PSRAM when above an SRAM-safe threshold;
      // small messages stay on the heap for zero PSRAM pressure. The
      // unique_ptr's custom deleter ensures cleanup on any return path.
      constexpr size_t DRAM_BUF_THRESHOLD = 64 * 1024;
      std::vector<uint8_t> mp_buf_dram;
      std::unique_ptr<uint8_t, void(*)(void*)> mp_psram(nullptr, heap_caps_free);
      uint8_t* mp = nullptr;
      if (mp_cap > DRAM_BUF_THRESHOLD) {
        mp_psram.reset((uint8_t*)heap_caps_malloc(mp_cap, MALLOC_CAP_SPIRAM));
        if (!mp_psram) {
          ERRORF("LXMF: send: PSRAM alloc of %u bytes failed", (unsigned)mp_cap);
          return fail("Not enough memory to encode this message.");
        }
        mp = mp_psram.get();
      } else {
        mp_buf_dram.resize(mp_cap);
        mp = mp_buf_dram.data();
      }
      size_t mp_pos = 0;
      mp[mp_pos++] = 0x94;  // fixarray(4)

      double ts = get_timestamp();
      size_t n = Common::MsgPack::pack_float64(&mp[mp_pos], mp_cap - mp_pos, ts);
      if (n == 0) { ERROR("LXMF: send: timestamp pack failed"); return fail("Internal error packing timestamp."); }
      mp_pos += n;

      n = Common::MsgPack::pack_bin_str(&mp[mp_pos], mp_cap - mp_pos, title);
      if (n == 0) { ERROR("LXMF: send: title pack failed"); return fail("Title is too long to encode."); }
      mp_pos += n;

      n = Common::MsgPack::pack_bin_str(&mp[mp_pos], mp_cap - mp_pos, content);
      if (n == 0) { ERROR("LXMF: send: content pack failed"); return fail("Internal error: content pack returned 0 unexpectedly."); }
      mp_pos += n;

      // Field encoding follows the Sideband convention so peers (Sideband,
      // Nomadnet, Columba) can decode our attachments natively:
      //   FIELD_IMAGE:            [ext_str, bytes]
      //   FIELD_AUDIO:            [mode_int, bytes]
      //   FIELD_FILE_ATTACHMENTS: [[name_str, bytes], ...]
      // Group by tag first so multi-file messages produce ONE map entry
      // with an array value (not a malformed map with duplicate keys).
      if (!attachments || attachments->empty()) {
        mp[mp_pos++] = 0xC0;  // fields: nil
      } else {
        // Collect indices per tag in attachment order.
        std::vector<size_t> file_idxs, image_idxs, audio_idxs;
        for (size_t i = 0; i < attachments->size(); ++i) {
          const uint8_t t = (*attachments)[i].tag;
          if      (t == FIELD_FILE_ATTACHMENTS) file_idxs.push_back(i);
          else if (t == FIELD_IMAGE)            image_idxs.push_back(i);
          else if (t == FIELD_AUDIO)            audio_idxs.push_back(i);
        }
        size_t field_count = (file_idxs.empty() ? 0 : 1)
                           + (image_idxs.empty() ? 0 : 1)
                           + (audio_idxs.empty() ? 0 : 1);
        n = Common::MsgPack::pack_map_header(&mp[mp_pos], mp_cap - mp_pos, field_count);
        if (n == 0) { ERROR("LXMF: send: map header pack failed"); return fail("Too many attachments."); }
        mp_pos += n;

        // Pack one [name_or_mode, bytes] pair. When the attachment was
        // staged (uploaded via the multipart endpoint), stream the
        // bytes out of OutboundStaging in 4 KiB chunks rather than
        // expecting them in `a.data`.
        auto pack_pair = [&](const std::string& name_or_ext, uint8_t audio_mode,
                             bool use_audio_mode,
                             const OutgoingAttachment& a,
                             const char* err_label) -> bool {
          size_t m = Common::MsgPack::pack_array_header(&mp[mp_pos], mp_cap - mp_pos, 2);
          if (m == 0) { ERROR("LXMF: send: pair header pack failed"); return false; }
          mp_pos += m;
          if (use_audio_mode) {
            m = Common::MsgPack::pack_uint8(&mp[mp_pos], mp_cap - mp_pos, audio_mode);
          } else {
            m = Common::MsgPack::pack_str(&mp[mp_pos], mp_cap - mp_pos, name_or_ext);
          }
          if (m == 0) { ERRORF("LXMF: send: %s name pack failed", err_label); return false; }
          mp_pos += m;
          const size_t dlen = a.byte_count();
          const size_t hdr = Common::MsgPack::pack_bin_header(&mp[mp_pos], mp_cap - mp_pos, dlen);
          if (hdr == 0) { ERRORF("LXMF: send: %s bin header pack failed", err_label); return false; }
          mp_pos += hdr;
          if (mp_pos + dlen > mp_cap) { ERRORF("LXMF: send: %s bytes overflow", err_label); return false; }
          if (a.staging_id) {
            size_t off = 0;
            while (off < dlen) {
              const size_t want = std::min((size_t)4096, dlen - off);
              const size_t got = Storage::OutboundStaging::read(a.staging_id, off, want, &mp[mp_pos]);
              if (got == 0) { ERRORF("LXMF: send: %s staging read failed at %u", err_label, (unsigned)off); return false; }
              mp_pos += got;
              off += got;
            }
          } else {
            memcpy(&mp[mp_pos], a.data.data(), dlen);
            mp_pos += dlen;
          }
          return true;
        };

        if (!image_idxs.empty()) {
          const auto& a = (*attachments)[image_idxs.front()];
          n = Common::MsgPack::pack_uint8(&mp[mp_pos], mp_cap - mp_pos, FIELD_IMAGE);
          if (n == 0) return fail("Internal error packing image key."); mp_pos += n;
          if (!pack_pair(a.ext, 0, false, a, "image"))
            return fail("Internal error packing image attachment.");
        }
        if (!audio_idxs.empty()) {
          const auto& a = (*attachments)[audio_idxs.front()];
          n = Common::MsgPack::pack_uint8(&mp[mp_pos], mp_cap - mp_pos, FIELD_AUDIO);
          if (n == 0) return fail("Internal error packing audio key."); mp_pos += n;
          if (!pack_pair("", a.audio_mode, true, a, "audio"))
            return fail("Internal error packing audio attachment.");
        }
        if (!file_idxs.empty()) {
          n = Common::MsgPack::pack_uint8(&mp[mp_pos], mp_cap - mp_pos, FIELD_FILE_ATTACHMENTS);
          if (n == 0) return fail("Internal error packing file-attachments key."); mp_pos += n;
          n = Common::MsgPack::pack_array_header(&mp[mp_pos], mp_cap - mp_pos, file_idxs.size());
          if (n == 0) return fail("Too many file attachments to encode."); mp_pos += n;
          for (size_t idx : file_idxs) {
            const auto& a = (*attachments)[idx];
            if (!pack_pair(a.filename, 0, false, a, "file"))
              return fail("Internal error packing file attachment.");
          }
        }
      }

      // Build signed_part = dest_hash || src_hash || payload || sha256(...)
      const RNS::Bytes& src_hash = _destination.hash();
      RNS::Bytes hashed_part;
      hashed_part.append(dest_hash.data(), HASH_LEN);
      hashed_part.append(src_hash.data(), HASH_LEN);
      hashed_part.append(mp, mp_pos);
      RNS::Bytes message_hash = RNS::Identity::full_hash(hashed_part);
      RNS::Bytes signed_part;
      signed_part.append(hashed_part);
      signed_part.append(message_hash);

      RNS::Bytes signature;
      try {
        signature = _identity.sign(signed_part);
      } catch (const std::exception& e) {
        ERRORF("LXMF: send: signing failed: %s", e.what());
        return fail("Signing failed — identity may be misconfigured.");
      }
      if (signature.size() != SIG_LEN) {
        ERRORF("LXMF: send: unexpected signature length %u", (unsigned)signature.size());
        return fail("Internal error: signature was not the expected length.");
      }

      // Build the on-wire LXMF blob: src_hash || sig || payload
      size_t total = HASH_LEN + SIG_LEN + mp_pos;
      if (total > 1 * 1024 * 1024) {
        return fail("Message body is too large (> 1 MiB).");
      }
      RNS::Bytes wire;
      wire.append(src_hash.data(), HASH_LEN);
      wire.append(signature.data(), SIG_LEN);
      wire.append(mp, mp_pos);

      out_rec.ts           = ts;
      // (boot_epoch, received_ms) is the monotonic-across-reboots
      // sort key for inbox/outbox records — see LXMFTypes.h.
      out_rec.boot_epoch   = Web::BootCounter::current();
      out_rec.received_ms  = millis();
      out_rec.peer_hash    = dest_hash;
      out_rec.title        = title;
      out_rec.content      = content;
      out_rec.incoming     = false;
      out_rec.signature_ok = true;
      // Outbox attachments. When the gateway has registered an
      // outbound-persist callback (and the per-identity toggle is on),
      // each attachment's bytes are copied to disk and the resulting
      // filename + backend land on AttachmentMeta — same shape the
      // inbox uses, so the SPA can render an inline preview of what
      // we sent. With persistence off, we fall back to metadata-only
      // entries: tag + size + display_name + mime, no filename.
      if (attachments) {
        std::vector<AttachmentMeta> persisted;
        if (_persist_outbound_fn) {
          persisted = _persist_outbound_fn(message_hash, *attachments);
        }
        const bool have_persisted = persisted.size() == attachments->size();
        for (size_t i = 0; i < attachments->size(); ++i) {
          const auto& a = (*attachments)[i];
          AttachmentMeta m = have_persisted ? persisted[i] : AttachmentMeta{};
          m.tag  = a.tag;
          m.size = (uint32_t)a.byte_count();
          if (a.tag == FIELD_IMAGE) {
            m.display_name = a.ext.empty() ? a.filename : ("image." + a.ext);
          } else {
            m.display_name = a.filename;
          }
          m.mime = a.mime;
          out_rec.attachments.push_back(m);
        }
      }

      // OPPORTUNISTIC: single packet to the SINGLE destination. Fast path
      // for short messages. send() returns a PacketReceipt; we hold it and
      // poll its status (tick_opportunistic_receipts). This mirrors
      // upstream LXMF (LXMessage.py:467-472): the message goes Sent on
      // hand-off and is upgraded to Delivered only if the recipient's
      // delivery proof arrives. Upstream sets NO timeout callback for
      // opportunistic, so a missing proof does NOT mark the message
      // failed — best-effort delivery over a lossy link (LoRa) routinely
      // delivers the packet while the small return proof is lost, and
      // showing "failed" for a message that actually arrived would be
      // worse than the old always-"sent". The previous code here latched
      // Sent the instant send() returned and never updated it; now Sent
      // is the honest hand-off state and Delivered is a real confirmation.
      if (total <= LXMF_OPPORTUNISTIC_MAX) {
        RNS::Packet packet(remote_dest, wire);
        RNS::PacketReceipt receipt = packet.send();
        out_rec.packet_hash = packet.get_hash();
        if (receipt) {
          out_rec.status = OutboxStatus::Sent;
          PendingOppSend op;
          op.receipt     = receipt;
          op.record_hash = out_rec.packet_hash;
          op.dest_hash   = dest_hash;
          op.owner       = this;
          op.started_ms  = (uint64_t)millis();
          pending_opp_sends()[out_rec.packet_hash] = std::move(op);
          NOTICEF("LXMF: sent OPPORTUNISTIC to %s (%u bytes), awaiting proof",
                  dest_hash.toHex().c_str(), (unsigned)total);
        } else {
          // No interface accepted the packet — a genuine transmit failure
          // (no path / no interface), distinct from a missing proof.
          out_rec.status = OutboxStatus::Failed;
          WARNINGF("LXMF: OPPORTUNISTIC send to %s failed — no interface accepted the packet",
                   dest_hash.toHex().c_str());
        }
        return true;
      }

      // DIRECT-mode: open a Link to the peer; on link_established the
      // static callback will pop our queued send and dispatch as either
      // an in-link PACKET (<= 319 B) or a RESOURCE (> 319 B), then teardown.
      // The MessageRecord transitions Queued -> Sent (after PROOF/COMPLETE)
      // -> Delivered via the gateway's status update path.
      RNS::Link link(remote_dest,
                     _static_outbound_link_established,
                     _static_outbound_link_closed);
      RNS::Bytes link_hash = link.hash();
      PendingLinkSend ps;
      // Spill large wires to disk so PSRAM is freed for the duration
      // of the link-establishment + transfer window (which can run
      // into minutes on airtime-throttled links). Small wires (<=
      // LXMF_LINK_PACKET_MAX) stay in PSRAM since they go out as a
      // single packet on link-established and the file round-trip
      // would dwarf the in-RAM cost.
      // DIRECT delivery hands the bytes straight to LXMF's
      // unpack_from_bytes — delivery_packet() for an in-link packet,
      // delivery_resource_concluded() for a resource — and neither prepends
      // the destination hash the way the opportunistic packet path does
      // (RNS LXMRouter.py:1829 prepends only for non-LINK packets; :1833 and
      // :1884 pass the data raw). So the full LXMF blob,
      //   destination_hash || source_hash || signature || payload,
      // must be on the wire here, unlike the dest-less opportunistic form
      // above (where the receiver re-derives the dest from the packet).
      RNS::Bytes direct_wire;
      direct_wire.append(dest_hash.data(), HASH_LEN);
      direct_wire.append(wire);
      if (direct_wire.size() > LXMF_LINK_PACKET_MAX) {
        ps.wire_path = _spill_wire_to_disk(direct_wire, link_hash);
        if (ps.wire_path.empty()) {
          // Fall back to keeping the wire in PSRAM.
          WARNING("LXMF: wire spill failed; keeping in PSRAM");
          ps.wire = direct_wire;
        } else {
          ps.total_bytes = (uint32_t)direct_wire.size();
          DEBUGF("LXMF: spilled %u-byte wire to %s",
                 (unsigned)direct_wire.size(), ps.wire_path.c_str());
        }
      } else {
        ps.wire = direct_wire;
      }
      ps.link        = link;
      ps.started_ms  = (uint64_t)millis();
      ps.dest_hash   = dest_hash;
      ps.owner       = this;
      ps.status      = OutboxStatus::Queued;
      // record_hash is the outbox key — stays stable across
      // retries so the SPA bubble doesn't lose track when we open a
      // new Link with a new link_hash on retry.
      ps.record_hash = link_hash;
      pending_link_sends()[link_hash] = std::move(ps);
      out_rec.status      = OutboxStatus::Queued;
      out_rec.packet_hash = link_hash;  // placeholder; real packet hash once sent
      NOTICEF("LXMF: opening Link to %s for %u-byte DIRECT send",
              dest_hash.toHex().c_str(), (unsigned)total);
      return true;
    }

    // LXMF timestamps are Unix-epoch seconds as float64. Defer to the
    // shared TimeManager (Web/TimeManager.h, #111) which arbitrates
    // across all time sources — GPS, NTP, Browser, RNS peer, RTC.
    // Falls back to a compile-time-epoch + millis() guess until the
    // manager is calibrated, so outbound messages don't carry ts=0.
    double get_timestamp() {
      if (Clock::Manager::is_calibrated()) {
        return Clock::Manager::now_epoch();
      }
      double up = (double)millis() / 1000.0;
      return _compile_time_epoch() + up;
    }

    void calibrate_time(double remote_ts) {
      // Hand the peer-supplied ts to TimeManager as a low-priority
      // RNS source; the manager will adopt it only if no
      // higher-priority source has reported and the value is sane.
      if (Clock::Manager::report_time(
            Clock::Manager::Source::RNS, remote_ts)) {
        NOTICEF("LXMF: time calibrated from peer (epoch %.0f)", remote_ts);
      }
    }

  private:
    static std::map<RNS::Bytes, LXMFMinimal*>& registry() {
      static std::map<RNS::Bytes, LXMFMinimal*> r;
      return r;
    }

    // Outbound link state, keyed by link.hash(). Each pending send sits
    // here from the time send_message kicks off the link establishment
    // until the resource (or in-link packet) concludes / link closes.
    static std::map<RNS::Bytes, PendingLinkSend>& pending_link_sends() {
      static std::map<RNS::Bytes, PendingLinkSend> p;
      return p;
    }

    // In-flight opportunistic sends awaiting a delivery proof / timeout,
    // keyed by the outbox record hash (== packet hash). Shared static
    // across all LXMFMinimal instances, polled by
    // tick_opportunistic_receipts().
    static std::map<RNS::Bytes, PendingOppSend>& pending_opp_sends() {
      static std::map<RNS::Bytes, PendingOppSend> p;
      return p;
    }

    // Write the wire bytes to a temp file under the SD-aware resource
    // tmp dir and return the chosen path on success, empty string on
    // failure. Used to spill large outbound payloads off PSRAM during
    // the link-establishment dwell (typically seconds, up to minutes
    // on retry). Caller is responsible for unlinking the file when
    // the PendingLinkSend goes away.
    static std::string _spill_wire_to_disk(const RNS::Bytes& wire,
                                           const RNS::Bytes& link_hash) {
      char path[256];
      snprintf(path, sizeof(path), "%s/outbound_%s_%lu.bin",
               RNS::resource_tmp_path(),
               link_hash.toHex().substr(0, 8).c_str(),
               (unsigned long)millis());
      try {
        microStore::File f = RNS::Utilities::OS::open_file(
            path, microStore::File::ModeReadWrite);
        if (!f) {
          ERRORF("LXMF: wire spill open '%s' failed", path);
          return {};
        }
        const size_t wrote = f.write(wire.data(), wire.size());
        f.flush();
        f.close();
        if (wrote != wire.size()) {
          ERRORF("LXMF: wire spill short %zu/%zu", wrote, wire.size());
          RNS::Utilities::OS::remove_file(path);
          return {};
        }
        return std::string(path);
      }
      catch (const std::exception& e) {
        ERRORF("LXMF: wire spill threw: %s", e.what());
        return {};
      }
    }

    // Inverse: load a previously spilled wire file back into a Bytes.
    // Returns the bytes on success, empty on failure. Used at link-
    // established time so the Resource constructor gets the same
    // payload that send_message built.
    static RNS::Bytes _load_wire_from_disk(const std::string& path,
                                           size_t expected_size) {
      try {
        microStore::File f = RNS::Utilities::OS::open_file(
            path.c_str(), microStore::File::ModeRead);
        if (!f) {
          ERRORF("LXMF: wire load open '%s' failed", path.c_str());
          return {};
        }
        RNS::Bytes out;
        uint8_t* dst = out.writable(expected_size);
        if (dst == nullptr) {
          ERRORF("LXMF: wire load alloc %zu bytes failed", expected_size);
          return {};
        }
        const size_t got = f.read(dst, expected_size);
        f.close();
        if (got != expected_size) {
          ERRORF("LXMF: wire load short %zu/%zu", got, expected_size);
          return {};
        }
        return out;
      }
      catch (const std::exception& e) {
        ERRORF("LXMF: wire load threw: %s", e.what());
        return {};
      }
    }

    // Idempotent cleanup of the on-disk wire file. Called when a
    // PendingLinkSend is being erased from the map.
    static void _release_wire_file(PendingLinkSend& ps) {
      if (ps.wire_path.empty()) return;
      try {
        if (RNS::Utilities::OS::file_exists(ps.wire_path.c_str())) {
          RNS::Utilities::OS::remove_file(ps.wire_path.c_str());
        }
      }
      catch (const std::exception& e) {
        WARNINGF("LXMF: wire unlink '%s' threw: %s",
                 ps.wire_path.c_str(), e.what());
      }
      ps.wire_path.clear();
    }

    // Static trampoline: looks up the LXMFMinimal* by the packet's
    // destination hash and dispatches to that instance.
    static void _static_packet_callback(const RNS::Bytes& data, const RNS::Packet& packet) {
      auto& reg = registry();
      auto it = reg.find(packet.destination_hash());
      if (it == reg.end() || it->second == nullptr) {
        WARNINGF("LXMF: incoming packet for unknown destination %s",
                 packet.destination_hash().toHex().c_str());
        return;
      }
      it->second->_on_packet(data, packet);
    }

    // ------------------------------------------------------------------
    // DIRECT-mode (Link + optional Resource) wiring (plan step 10)
    //
    // OUTBOUND flow:
    //  send_message > 295 B
    //    -> open Link(dest, _static_outbound_link_established, ...)
    //    -> store PendingLinkSend keyed by link.hash()
    //    -> link establishes asynchronously
    //  _static_outbound_link_established(link)
    //    -> if wire <= 319: send as in-link Packet, status -> Sent,
    //       wait for Link teardown
    //    -> else: construct Resource(wire, link,
    //                                _static_outbound_resource_concluded)
    //       and record resource_hash in the pending entry
    //
    // INBOUND flow:
    //  peer opens Link to our delivery destination
    //    -> _static_inbound_link_established(link)
    //    -> link.set_resource_strategy(ACCEPT_ALL)
    //    -> link.set_packet_callback(_static_inbound_link_packet)
    //    -> link.set_resource_concluded_callback(_static_inbound_resource_concluded)
    //  peer sends short LXMF wire: in-link Packet path
    //    -> _static_inbound_link_packet  -> _deliver_lxmf_payload
    //  peer sends long LXMF wire: Resource path
    //    -> _static_inbound_resource_concluded -> _deliver_lxmf_payload
    // ------------------------------------------------------------------

    static void _static_outbound_link_established(RNS::Link& link) {
      auto& m = pending_link_sends();
      auto it = m.find(link.hash());
      if (it == m.end()) {
        WARNINGF("LXMF: outbound link established but no pending send for %s",
                 link.hash().toHex().c_str());
        return;
      }
      PendingLinkSend& ps = it->second;

      // Materialise the wire — either it's already in PSRAM (small
      // send) or we spilled it to disk in send_message and need to
      // read it back for the Resource constructor.
      RNS::Bytes wire_for_send;
      const RNS::Bytes* wire_ptr = nullptr;
      if (!ps.wire_path.empty()) {
        wire_for_send = _load_wire_from_disk(ps.wire_path, ps.total_bytes);
        if (!wire_for_send.size()) {
          ERRORF("LXMF: failed to reload spilled wire for %s",
                 ps.dest_hash.toHex().c_str());
          _schedule_retry_or_fail(it, m, "wire reload failed");
          return;
        }
        wire_ptr = &wire_for_send;
      } else {
        wire_ptr = &ps.wire;
      }
      const size_t wire_len = wire_ptr->size();
      NOTICEF("LXMF: outbound link established to %s (%u-byte payload)",
              ps.dest_hash.toHex().c_str(), (unsigned)wire_len);

      if (wire_len <= LXMF_LINK_PACKET_MAX) {
        // Single in-link DATA packet path.
        bool sent_ok = false;
        try {
          RNS::Packet pkt(link, *wire_ptr);
          pkt.send();
          sent_ok = true;
        }
        catch (const std::exception& e) {
          ERRORF("LXMF: in-link packet send failed: %s", e.what());
        }
        if (sent_ok) {
          ps.status = OutboxStatus::Sent;
          if (ps.owner && ps.owner->_on_outbox_status) {
            try { ps.owner->_on_outbox_status(ps.record_hash, ps.status); } catch (...) {}
          }
          _release_wire_file(ps);
          m.erase(it);
        }
        else {
          // Schedule retry (or give up if exhausted).
          _schedule_retry_or_fail(it, m, "in-link packet send threw");
        }
        // No manual link.teardown() — calling it from inside the
        // inbound-callback chain leaves dangling references that trip
        // the heap-poisoning canary when the pending entry is erased.
        // The link's own state machine will close after timeout.
      }
      else {
        // Resource transfer. The Resource constructor encrypts the
        // wire and (for sizes > RAM_BUFFER_THRESHOLD) spills the
        // ciphertext to its own temp file under the resource-tmp dir,
        // so wire_for_send / ps.wire can be released after this.
        RNS::Resource res(*wire_ptr, link,
                          /*advertise=*/true, /*auto_compress=*/false,
                          _static_outbound_resource_concluded,
                          _static_outbound_resource_progress,
                          /*timeout=*/0.0);
        ps.resource_hash = res.hash();
        if (ps.total_bytes == 0) ps.total_bytes = (uint32_t)wire_len;
        // Release the in-RAM wire copy now that the Resource owns the
        // ciphertext (on disk if it spilled). The on-disk wire file
        // stays around — see _release_wire_file in the concluded /
        // closed / cancel paths — so a retry can rebuild the Resource
        // from the same payload without re-msgpacking.
        ps.wire = RNS::Bytes();
        DEBUGF("LXMF: outbound resource advertised %s",
               ps.resource_hash.toHex().c_str());
      }
    }

    // Progress trampoline for outbound Resources — fires as parts are
    // sent. Looks up the matching pending entry by resource hash, then
    // dispatches to the owning LXMFMinimal's _on_progress callback if
    // any. Cheap; called once per outbound part.
    static void _static_outbound_resource_progress(const RNS::Resource& res) {
      auto& m = pending_link_sends();
      for (const auto& kv : m) {
        const PendingLinkSend& ps = kv.second;
        if (ps.resource_hash != res.hash()) continue;
        if (!ps.owner || !ps.owner->_on_progress) return;
        // Sender progress: parts_done = _sent_parts on the Resource.
        // The Resource object's _object is private, so we use the public
        // get_progress() helper which returns 0.0..1.0, scaled to bytes.
        const float frac = res.get_progress();
        const uint32_t total = ps.total_bytes;
        const uint32_t done  = (uint32_t)(frac * (float)total);
        try {
          ps.owner->_on_progress(ps.dest_hash, kv.first, /*incoming=*/false,
                                  done, total);
        } catch (...) {}
        return;
      }
    }

    static void _static_outbound_link_closed(RNS::Link& link) {
      auto& m = pending_link_sends();
      auto it = m.find(link.hash());
      if (it == m.end()) return;
      PendingLinkSend& ps = it->second;
      // Successful sends already concluded and erased themselves.
      // Getting here means an abnormal close — schedule a retry (or
      // give up, depending on the budget).
      if (ps.status != OutboxStatus::Sent &&
          ps.status != OutboxStatus::Delivered) {
        _schedule_retry_or_fail(it, m, "link closed before send completed");
      }
      else {
        // Normal post-Sent close on an in-link packet path. Erase.
        _release_wire_file(ps);
        m.erase(it);
      }
    }

    static void _static_outbound_resource_concluded(const RNS::Resource& res) {
      auto& m = pending_link_sends();
      // Find the pending send whose resource_hash matches.
      for (auto it = m.begin(); it != m.end(); ) {
        if (it->second.resource_hash == res.hash()) {
          PendingLinkSend& ps = it->second;
          const auto st = res.status();
          if (st == RNS::Type::Resource::COMPLETE) {
            ps.status = OutboxStatus::Delivered;
            NOTICEF("LXMF: outbound resource COMPLETE for %s",
                    ps.dest_hash.toHex().c_str());
            if (ps.owner && ps.owner->_on_outbox_status) {
              try { ps.owner->_on_outbox_status(ps.record_hash, ps.status); } catch (...) {}
            }
            // Retain-on-delivery: pin the peer against known-destinations churn,
            // mirroring upstream LXMRouter.process_outbound (LXMRouter.py:2516).
            RNS::Identity::retain_destination(ps.dest_hash);
            // Deliberately *not* firing a final _on_progress here — the
            // gateway's outbox-status callback already publishes
            // message_complete (finished=true) which the SPA treats as
            // the canonical end-of-transfer signal.
            _release_wire_file(ps);
            it = m.erase(it);
          }
          else {
            // Resource failed — schedule retry or give up.
            WARNINGF("LXMF: outbound resource FAILED for %s (status=%d)",
                     ps.dest_hash.toHex().c_str(), (int)st);
            _schedule_retry_or_fail(it, m, "resource transfer failed");
            // _schedule_retry_or_fail may erase or keep the entry; in
            // either case its iterator becomes invalid, so break out
            // and let the next call to this static function pick up
            // any other matching entries (there shouldn't be any).
            return;
          }
        }
        else {
          ++it;
        }
      }
    }

    // Either schedule the entry for retry (decrement retries_left,
    // set retry_pending + next_retry_at_ms, keep the entry alive) or
    // give up (status=Failed, fire callback, erase). The caller passes
    // an iterator that's valid on entry; after this returns the
    // iterator is invalid in the give-up branch and points to the
    // same entry in the schedule branch.
    static void _schedule_retry_or_fail(typename std::map<RNS::Bytes, PendingLinkSend>::iterator it,
                                         std::map<RNS::Bytes, PendingLinkSend>& m,
                                         const char* reason) {
      PendingLinkSend& ps = it->second;
      if (ps.retries_left == 0) {
        // Auto-retries exhausted. Mark Failed and fire the status
        // callback so the SPA shows the bubble as failed. Do NOT erase
        // — the entry stays so a manual /retry from the SPA can revive
        // it (the wire bytes are still here). Bound the surviving
        // entries via prune_stale_failed() so this isn't a leak.
        ps.status        = OutboxStatus::Failed;
        ps.retry_pending = false;
        ps.resource_hash = RNS::Bytes{};
        if (ps.owner && ps.owner->_on_outbox_status) {
          try { ps.owner->_on_outbox_status(ps.record_hash, ps.status); } catch (...) {}
        }
        WARNINGF("LXMF: outbox send to %s failed permanently (%s) — auto-retry budget exhausted; manual retry available",
                 ps.dest_hash.toHex().c_str(), reason);
        return;
      }
      const uint8_t attempts_done = DEFAULT_OUTBOX_RETRIES - ps.retries_left + 1;
      ps.retries_left--;
      ps.retry_pending    = true;
      ps.next_retry_at_ms = (uint64_t)millis() + (uint64_t)RETRY_BACKOFF_MS_STEP * attempts_done;
      ps.status           = OutboxStatus::Queued;
      ps.resource_hash    = RNS::Bytes{};  // stale; new Link will rebuild Resource
      NOTICEF("LXMF: outbox send to %s failed (%s) — retry %u/%u scheduled in %us",
              ps.dest_hash.toHex().c_str(), reason,
              (unsigned)attempts_done, (unsigned)DEFAULT_OUTBOX_RETRIES,
              (unsigned)(RETRY_BACKOFF_MS_STEP * attempts_done / 1000));
    }

  public:
    // Manually re-queue a Failed outbox entry. Called by the WebUI
    // /api/identities/{id}/outbox/{seq}/retry handler after it looks
    // up the MessageRecord's packet_hash (== PendingLinkSend.record_hash).
    // Resets retries_left to the full budget so the user gets a fresh
    // set of automatic attempts, and schedules the first one
    // immediately.
    static bool manual_retry(const RNS::Bytes& record_hash) {
      auto& m = pending_link_sends();
      for (auto& kv : m) {
        if (kv.second.record_hash != record_hash) continue;
        PendingLinkSend& ps = kv.second;
        if (ps.status != OutboxStatus::Failed) {
          // Only Failed entries are eligible — a Queued/Sent record
          // is either in flight or already done.
          return false;
        }
        ps.retries_left     = DEFAULT_OUTBOX_RETRIES;
        ps.retry_pending    = true;
        ps.next_retry_at_ms = (uint64_t)millis();  // fire on next tick
        ps.status           = OutboxStatus::Queued;
        NOTICEF("LXMF: manual retry requested for outbox record %s (peer %s)",
                ps.record_hash.toHex().c_str(),
                ps.dest_hash.toHex().c_str());
        return true;
      }
      return false;
    }

    // Periodic tick to advance scheduled retries. Call from a loop
    // that holds the rns_lock (LXMFGateway::loop is the right spot).
    // For each entry with retry_pending && millis() >= next_retry_at_ms,
    // open a fresh Link with the same wire bytes + dest, re-key the
    // map under the new link_hash. The original record_hash stays so
    // the outbox bubble survives.
    static void tick_retries() {
      auto& m = pending_link_sends();
      const uint64_t now = (uint64_t)millis();
      // Snapshot keys whose entries are due — re-keying mid-iteration
      // would invalidate the iterator.
      std::vector<RNS::Bytes> due;
      for (auto& kv : m) {
        if (kv.second.retry_pending && now >= kv.second.next_retry_at_ms) {
          due.push_back(kv.first);
        }
      }
      for (const RNS::Bytes& old_key : due) {
        auto it = m.find(old_key);
        if (it == m.end()) continue;
        PendingLinkSend ps = std::move(it->second);
        m.erase(it);
        ps.retry_pending = false;

        RNS::Identity remote_identity = RNS::Identity::recall(ps.dest_hash);
        if (!remote_identity) {
          WARNINGF("LXMF: retry — cannot recall identity for %s, marking failed",
                   ps.dest_hash.toHex().c_str());
          ps.status = OutboxStatus::Failed;
          if (ps.owner && ps.owner->_on_outbox_status) {
            try { ps.owner->_on_outbox_status(ps.record_hash, ps.status); } catch (...) {}
          }
          _release_wire_file(ps);
          continue;
        }
        try {
          RNS::Destination remote_dest(
            remote_identity,
            RNS::Type::Destination::OUT,
            RNS::Type::Destination::SINGLE,
            "lxmf", "delivery"
          );
          RNS::Link new_link(remote_dest,
                             _static_outbound_link_established,
                             _static_outbound_link_closed);
          RNS::Bytes new_link_hash = new_link.hash();
          ps.link       = new_link;
          ps.started_ms = now;
          NOTICEF("LXMF: retry — new link %s for outbox record %s (peer %s)",
                  new_link_hash.toHex().c_str(),
                  ps.record_hash.toHex().c_str(),
                  ps.dest_hash.toHex().c_str());
          m[new_link_hash] = std::move(ps);
        }
        catch (const std::exception& e) {
          ERRORF("LXMF: retry — open Link threw: %s; marking failed", e.what());
          ps.status = OutboxStatus::Failed;
          if (ps.owner && ps.owner->_on_outbox_status) {
            try { ps.owner->_on_outbox_status(ps.record_hash, ps.status); } catch (...) {}
          }
          _release_wire_file(ps);
        }
      }
    }

    // Poll opportunistic (single-packet) sends for a delivery proof. The
    // receipt is the same object Transport tracks, so it advances on its
    // own: a delivery proof flips it to DELIVERED, the periodic receipt
    // check flips it to FAILED on timeout (or CULLED when the table
    // overflows). Only DELIVERED is surfaced (Sent -> Delivered); a
    // timed-out/culled receipt is NOT marked failed — it just stops being
    // polled and the message stays Sent, mirroring upstream LXMF, which
    // registers no timeout callback for opportunistic sends (a lost return
    // proof on a lossy link is not a delivery failure). Shared static map
    // -> one call covers every identity; called from LXMFGateway::loop.
    static void tick_opportunistic_receipts() {
      auto& m = pending_opp_sends();
      const uint64_t now = (uint64_t)millis();
      for (auto it = m.begin(); it != m.end(); ) {
        PendingOppSend& op = it->second;
        const auto st = op.receipt ? op.receipt.status()
                                   : RNS::Type::PacketReceipt::FAILED;
        if (st == RNS::Type::PacketReceipt::DELIVERED) {
          // Proof returned: upgrade Sent -> Delivered.
          if (op.owner && op.owner->_on_outbox_status) {
            try { op.owner->_on_outbox_status(op.record_hash, OutboxStatus::Delivered); } catch (...) {}
          }
          // Retain-on-delivery: pin the peer so its identity/key survives the
          // known-destinations churn on a node bridging a busy backbone.
          // Mirrors upstream LXMRouter.process_outbound (LXMRouter.py:2516).
          RNS::Identity::retain_destination(op.dest_hash);
          it = m.erase(it);
        } else if (st == RNS::Type::PacketReceipt::FAILED ||
                   st == RNS::Type::PacketReceipt::CULLED ||
                   (op.started_ms > 0 && (now - op.started_ms) > OPP_RECEIPT_MAX_MS)) {
          // No proof within the receipt window (or receipt lost from
          // tracking). Stop polling; leave the message Sent — opportunistic
          // delivery is best-effort and unconfirmed, not failed.
          it = m.erase(it);
        } else {
          ++it;
        }
      }
    }

    // Defensive sweep for orphaned outbound entries. Every normal send
    // lifecycle (Sent / Delivered / Failed / Closed / retry-exhausted)
    // erases its PendingLinkSend, but the chain has several hops
    // (Resource::concluded → outbound_resource_concluded → erase, or
    // Link::closed_callback → outbound_link_closed → erase, etc.). If
    // any callback is dropped — radio dies mid-transfer, async-server
    // event loop falls behind, etc. — the entry could leak. Catch
    // anything > PENDING_SEND_ORPHAN_MS old that's neither in flight
    // (Queued/Sent waiting for a callback that's reasonably timely)
    // nor scheduled for retry. Conservative threshold so big-payload
    // transfers don't get accidentally killed.
    //
    // Called from tick_retries() so it shares the same periodic cadence
    // as the existing retry-scheduler — no new timer needed.
    static constexpr uint64_t PENDING_SEND_ORPHAN_MS = 30ULL * 60ULL * 1000ULL; // 30 min
    static void sweep_orphaned_pending() {
      auto& m = pending_link_sends();
      const uint64_t now = (uint64_t)millis();
      for (auto it = m.begin(); it != m.end(); ) {
        PendingLinkSend& ps = it->second;
        const bool stale = ps.started_ms > 0 &&
                           (now - ps.started_ms) > PENDING_SEND_ORPHAN_MS;
        const bool terminal = (ps.status == OutboxStatus::Delivered) ||
                              (ps.status == OutboxStatus::Failed);
        if (stale && !ps.retry_pending && !terminal) {
          WARNINGF("LXMF: sweeping orphaned pending_link_send for %s "
                   "(record %s, age %lus, status=%s) — no callback ever "
                   "completed the send",
                   ps.dest_hash.toHex().c_str(),
                   ps.record_hash.toHex().c_str(),
                   (unsigned long)((now - ps.started_ms) / 1000),
                   outbox_status_name(ps.status));
          // Mark Failed so the SPA bubble shows the right state. Then
          // erase so the entry doesn't sit around forever.
          if (ps.owner && ps.owner->_on_outbox_status) {
            try {
              ps.owner->_on_outbox_status(ps.record_hash, OutboxStatus::Failed);
            } catch (...) {}
          }
          _release_wire_file(ps);
          it = m.erase(it);
        } else {
          ++it;
        }
      }
    }

  private:

    static void _static_inbound_link_established(RNS::Link& link) {
      // The destination this link landed on tells us which LXMFMinimal
      // instance owns it. (Multi-identity firmware can host several.)
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) {
        WARNINGF("LXMF: inbound link to unknown destination %s",
                 our_dest_hash.toHex().c_str());
        return;
      }
      NOTICEF("LXMF: inbound link established on %s",
              our_dest_hash.toHex().c_str());

      // Accept all Resources on this link; route in-link packets +
      // resource conclusions to the static trampolines that look up
      // the owning LXMFMinimal by the link's destination.
      link.set_resource_strategy(RNS::Type::Link::ACCEPT_ALL);
      link.set_packet_callback(_static_inbound_link_packet);
      link.set_resource_concluded_callback(_static_inbound_resource_concluded);
      link.set_resource_progress_callback(_static_inbound_resource_progress);
    }

    // Progress trampoline for inbound Resources — fires as parts arrive.
    // Resolves the owning LXMFMinimal by the link's destination hash.
    // peer_hash is left empty: the Reticulum responder side has no way
    // to know who opened an anonymous inbound link until the LXMF wire
    // payload is decrypted at message_complete (LXMF doesn't send
    // LINKIDENTIFY). The SPA keys in-flight progress by link_hash and
    // correlates to the conversation when message_complete carries the
    // source_hash. transfer_size on the Resource is the authoritative
    // byte total; get_progress() returns received/parts as a fraction.
    static void _static_inbound_resource_progress(const RNS::Resource& res) {
      const RNS::Link& link = res.link();
      if (!link) return;
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) return;
      if (!it->second->_on_progress) return;
      const float    frac  = res.get_progress();
      const uint32_t total = (uint32_t)res.size();
      const uint32_t done  = (uint32_t)(frac * (float)total);
      try {
        it->second->_on_progress(RNS::Bytes(), link.hash(),
                                  /*incoming=*/true, done, total);
      } catch (...) {}
    }

    static void _static_inbound_link_packet(const RNS::Bytes& plaintext,
                                            const RNS::Packet& packet) {
      // Find the owning LXMFMinimal via the link's destination hash.
      const RNS::Link& link = packet.destination_link();
      if (!link) return;
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) return;
      NOTICEF("LXMF: inbound in-link PACKET on %s (%u bytes)",
              our_dest_hash.toHex().c_str(), (unsigned)plaintext.size());
      it->second->_deliver_lxmf_payload(plaintext, packet.get_hash(), /*has_dest_prefix=*/true);
    }

    static void _static_inbound_resource_concluded(const RNS::Resource& res) {
      const RNS::Link& link = res.link();
      const RNS::Bytes our_dest_hash = link.destination().hash();
      auto& reg = registry();
      auto it = reg.find(our_dest_hash);
      if (it == reg.end() || it->second == nullptr) return;
      const uint32_t total = (uint32_t)res.size();
      const bool ok = (res.status() == RNS::Type::Resource::COMPLETE);
      // Symmetric receive-complete event — fires regardless of whether
      // the LXMF decrypt step that follows succeeds. The SPA uses this
      // to clear synthetic "Incoming attachment …" rows + the topbar
      // progress strip, mirroring the outbound message_complete path.
      // Done BEFORE delivery so the conv list updates ahead of the
      // new-message insertion. peer hash is unknown here (the LXMF
      // payload hasn't been decrypted yet — that's why this exists);
      // the SPA keys the clear off the link hash.
      if (it->second->_on_receive_complete) {
        try {
          it->second->_on_receive_complete(link.hash(), total, ok);
        } catch (...) {}
      }
      if (!ok) {
        WARNINGF("LXMF: inbound resource FAILED/CORRUPT (status=%d)",
                 (int)res.status());
        return;
      }
      const RNS::Bytes& plaintext = res.plaintext();
      NOTICEF("LXMF: inbound RESOURCE COMPLETE on %s (%u bytes)",
              our_dest_hash.toHex().c_str(), (unsigned)plaintext.size());
      it->second->_deliver_lxmf_payload(plaintext, res.hash(), /*has_dest_prefix=*/true);
    }

    // Common parse + signature-verify + delivery path used by all three
    // inbound modes (OPPORTUNISTIC Packet, in-link Packet, Resource).
    void _deliver_lxmf_payload(const RNS::Bytes& wire,
                               const RNS::Bytes& packet_or_resource_hash,
                               bool has_dest_prefix = false) {
      const uint8_t* raw = wire.data();
      size_t raw_len = wire.size();

      // DIRECT delivery (in-link packet / resource) carries the full LXMF
      // blob: dest_hash || src_hash || signature || payload — RNS hands it
      // straight to unpack_from_bytes (LXMRouter.py:1833 for in-link packets,
      // :1884 for resources). The opportunistic packet path omits the dest
      // hash (it is the packet's destination, prepended by the receiver at
      // LXMRouter.py:1829). Skip the dest here so the layout below is the
      // common src_hash || signature || payload for all three inbound modes.
      if (has_dest_prefix) {
        if (raw_len < HASH_LEN) {
          WARNING("LXMF: incoming DIRECT payload too short for dest hash");
          return;
        }
        raw     += HASH_LEN;
        raw_len -= HASH_LEN;
      }

      if (raw_len < HEADER_LEN + 5) {
        WARNING("LXMF: incoming payload too short for LXMF header");
        return;
      }

      RNS::Bytes source_hash(raw, HASH_LEN);
      RNS::Bytes signature(raw + HASH_LEN, SIG_LEN);
      const uint8_t* payload = raw + HEADER_LEN;
      size_t payload_len = raw_len - HEADER_LEN;

      double msg_ts = 0;
      std::string title;
      std::string content;
      std::vector<FieldBlob> fields;
      if (!_parse_lxmf_payload(payload, payload_len, &msg_ts, &title, &content, &fields)) {
        WARNING("LXMF: malformed payload");
        return;
      }
      if (msg_ts > 0) calibrate_time(msg_ts);

      bool sig_ok = false;
      RNS::Identity sender = RNS::Identity::recall(source_hash);
      if (sender) {
        RNS::Bytes hashed_part;
        hashed_part.append(_destination.hash().data(), HASH_LEN);
        hashed_part.append(source_hash.data(), HASH_LEN);
        hashed_part.append(payload, payload_len);
        RNS::Bytes mh = RNS::Identity::full_hash(hashed_part);
        RNS::Bytes signed_part;
        signed_part.append(hashed_part);
        signed_part.append(mh);
        sig_ok = sender.validate(signature, signed_part);
      }
      else {
        WARNINGF("LXMF: sender %s identity not known — signature cannot be verified",
                 source_hash.toHex().c_str());
      }

      if (_on_delivery) {
        MessageRecord rec;
        rec.ts           = msg_ts;
        rec.boot_epoch   = Web::BootCounter::current();
        rec.received_ms  = millis();
        rec.peer_hash    = source_hash;
        rec.title        = title;
        rec.content      = content;
        rec.incoming     = true;
        rec.signature_ok = sig_ok;
        rec.status       = OutboxStatus::Delivered;
        rec.packet_hash  = packet_or_resource_hash;
        // Persist attachments (FIELD_FILE_ATTACHMENTS / FIELD_IMAGE /
        // FIELD_AUDIO) under the calling identity's attachments dir
        // and annotate the record with metadata.
        _persist_attachments(packet_or_resource_hash, fields, rec.attachments);
        try { _on_delivery(rec); }
        catch (const std::bad_alloc&) {
          ERROR("LXMF: delivery callback bad_alloc");
        }
        catch (const std::exception& e) {
          ERRORF("LXMF: delivery callback exception: %s", e.what());
        }
      }
    }

    void _on_packet(const RNS::Bytes& data, const RNS::Packet& packet) {
      // Acknowledge delivery to the sender (Reticulum proof).
      const_cast<RNS::Packet&>(packet).prove();

      if (data.size() < HEADER_LEN + 5) {
        WARNING("LXMF: incoming packet too short for LXMF header");
        return;
      }
      const uint8_t* raw = data.data();
      size_t raw_len = data.size();

      RNS::Bytes source_hash(raw, HASH_LEN);
      RNS::Bytes signature(raw + HASH_LEN, SIG_LEN);
      const uint8_t* payload = raw + HEADER_LEN;
      size_t payload_len = raw_len - HEADER_LEN;

      double msg_ts = 0;
      std::string title;
      std::string content;
      std::vector<FieldBlob> fields;
      if (!_parse_lxmf_payload(payload, payload_len, &msg_ts, &title, &content, &fields)) {
        WARNING("LXMF: malformed payload");
        return;
      }
      if (msg_ts > 0) calibrate_time(msg_ts);

      // Reconstruct the signed payload and verify the Ed25519 signature.
      bool sig_ok = false;
      RNS::Identity sender = RNS::Identity::recall(source_hash);
      if (sender) {
        RNS::Bytes hashed_part;
        hashed_part.append(_destination.hash().data(), HASH_LEN);
        hashed_part.append(source_hash.data(), HASH_LEN);
        hashed_part.append(payload, payload_len);
        RNS::Bytes mh = RNS::Identity::full_hash(hashed_part);
        RNS::Bytes signed_part;
        signed_part.append(hashed_part);
        signed_part.append(mh);
        sig_ok = sender.validate(signature, signed_part);
      } else {
        WARNINGF("LXMF: sender %s identity not known — signature cannot be verified",
                 source_hash.toHex().c_str());
      }

      NOTICEF("LXMF: incoming from %s (%u bytes, sig %s)",
              source_hash.toHex().c_str(), (unsigned)data.size(),
              sig_ok ? "ok" : "BAD");

      if (_on_delivery) {
        MessageRecord rec;
        rec.ts           = msg_ts;
        rec.boot_epoch   = Web::BootCounter::current();
        rec.received_ms  = millis();
        rec.peer_hash    = source_hash;
        rec.title        = title;
        rec.content      = content;
        rec.incoming     = true;
        rec.signature_ok = sig_ok;
        rec.status       = OutboxStatus::Delivered;
        rec.packet_hash  = packet.get_hash();
        _persist_attachments(packet.get_hash(), fields, rec.attachments);
        try {
          _on_delivery(rec);
        } catch (const std::bad_alloc&) {
          ERROR("LXMF: delivery callback bad_alloc");
        } catch (const std::exception& e) {
          ERRORF("LXMF: delivery callback exception: %s", e.what());
        }
      }
    }

  public:
    // Single raw-bytes view of one msgpack field-value. tag is the LXMF
    // FIELD_* key (the dict key, e.g. 0x05 for FIELD_FILE_ATTACHMENTS),
    // raw points into the original payload buffer and is only valid for
    // the duration of the parse call. The caller copies what it wants
    // to keep before that buffer goes out of scope.
    struct FieldBlob {
      uint8_t        tag;
      const uint8_t* raw;
      size_t         raw_len;
      // Sender-supplied filename (Sideband convention) — empty for legacy
      // peers or fields that don't carry one. For FIELD_IMAGE this holds
      // the bare extension ("webp", "png"); for FIELD_FILE_ATTACHMENTS
      // it's the original filename. FIELD_AUDIO doesn't use this.
      std::string    filename;
      // For FIELD_AUDIO under Sideband: codec mode (AM_*). 0xFF (AM_CUSTOM)
      // is the safe default for legacy / unknown.
      uint8_t        audio_mode = 0xFF;
    };

    // Hook the gateway sets so we can write attachment blobs to the
    // owning identity's storage dir without LXMFMinimal needing to
    // know the disk layout. The lambda receives (msg_hash, fields)
    // and returns the persisted [{tag, size, filename}] metadata.
    using AttachmentPersistFn = std::function<std::vector<AttachmentMeta>(
        const RNS::Bytes& /*msg_hash*/,
        const std::vector<FieldBlob>& /*fields*/)>;
    void set_attachment_persist_callback(AttachmentPersistFn cb) {
      _persist_attachments_fn = std::move(cb);
    }

    // Sibling hook for outbound: when we send an attachment, the
    // gateway gets a chance to copy the bytes to the sender's storage
    // dir so the SPA can render the same inline preview in the
    // sender's own chat bubble. The returned vector mirrors the input
    // order; entries with non-empty filename get rendered inline.
    using OutboundPersistFn = std::function<std::vector<AttachmentMeta>(
        const RNS::Bytes& /*msg_hash*/,
        const std::vector<OutgoingAttachment>& /*outgoing*/)>;
    void set_outbound_persist_callback(OutboundPersistFn cb) {
      _persist_outbound_fn = std::move(cb);
    }
    void _persist_attachments(const RNS::Bytes& msg_hash,
                              const std::vector<FieldBlob>& fields,
                              std::vector<AttachmentMeta>& out) {
      if (fields.empty()) return;
      // Cap inbound attachment count at the trust boundary. A peer
      // could send arbitrarily many fields; we truncate before any
      // persistence work so a flood can't exhaust storage or RAM.
      std::vector<FieldBlob> capped_fields = fields;
      if (capped_fields.size() > LXMF_MAX_ATTACHMENTS) {
        WARNINGF("LXMF: peer attachments %u exceeds cap %u, dropping extras",
                 (unsigned)capped_fields.size(), (unsigned)LXMF_MAX_ATTACHMENTS);
        capped_fields.resize(LXMF_MAX_ATTACHMENTS);
      }
      if (!_persist_attachments_fn) {
        // No persist hook wired — surface metadata without on-disk
        // storage so the SPA at least knows attachments arrived. The
        // bytes are dropped on the floor; this only happens before
        // LXMFGateway::activate runs (i.e. never in normal operation).
        for (const auto& f : capped_fields) {
          AttachmentMeta m;
          m.tag = f.tag;
          m.size = (uint32_t)f.raw_len;
          out.push_back(m);
        }
        return;
      }
      out = _persist_attachments_fn(msg_hash, capped_fields);
    }

    bool _parse_lxmf_payload(const uint8_t* data, size_t len,
                             double* out_ts, std::string* out_title, std::string* out_content,
                             std::vector<FieldBlob>* out_fields = nullptr) {
      if (len < 2) return false;
      size_t off = 0;
      uint8_t tag = data[off++];
      size_t arr_len = 0;
      if ((tag & 0xF0) == 0x90) {
        arr_len = tag & 0x0F;
      } else if (tag == 0xDC && off + 1 < len) {
        arr_len = (data[off] << 8) | data[off + 1];
        off += 2;
      } else {
        return false;
      }
      if (arr_len < 3) return false;

      // Element 0: timestamp (float64)
      if (out_ts && off + 9 <= len && data[off] == 0xCB) {
        union { double d; uint8_t b[8]; } u;
        for (int i = 0; i < 8; i++) u.b[7 - i] = data[off + 1 + i];
        *out_ts = u.d;
      }
      if (!Common::MsgPack::skip_element(data, len, off)) return false;

      // Element 1: title. Truncate at the trust boundary — a peer
      // could send a multi-MB title and we'd otherwise carry it
      // through into the inbox spool, the WS broadcast, and the SPA.
      std::string title = Common::MsgPack::read_bin_or_str(data, len, off);
      if (title.size() > LXMF_MAX_TITLE_BYTES) {
        WARNINGF("LXMF: peer title %u B exceeds cap %u, truncating",
                 (unsigned)title.size(), (unsigned)LXMF_MAX_TITLE_BYTES);
        title.resize(LXMF_MAX_TITLE_BYTES);
      }
      if (out_title) *out_title = title;

      // Element 2: content. Same trust-boundary truncation as title.
      // Empty content is valid and must NOT be rejected: an attachment-only
      // message (image/audio/file with no caption) carries its whole payload
      // in the fields dict below. Rejecting empty content here dropped every
      // captionless image — the Resource transferred and proofed back, so the
      // sender saw "delivered", but the receiver discarded it at parse and it
      // never reached the inbox. read_bin_or_str consumes the (empty) element
      // either way, so field parsing below stays in sync.
      std::string content = Common::MsgPack::read_bin_or_str(data, len, off);
      if (content.size() > LXMF_MAX_BODY_BYTES) {
        WARNINGF("LXMF: peer body %u B exceeds cap %u, truncating",
                 (unsigned)content.size(), (unsigned)LXMF_MAX_BODY_BYTES);
        content.resize(LXMF_MAX_BODY_BYTES);
      }
      if (out_content) *out_content = content;

      // Element 3: fields dict (optional — may be nil, may be omitted).
      // LXMF wire format: [ts, title, content, fields, stamp?]
      // We only persist tags 0x05/0x06/0x07 (file / image / audio
      // attachments); other tags pass through unparsed. The caller
      // gets pointers into the input buffer for each interesting
      // field's raw msgpack-value, so the persistence step can write
      // them straight to flash without an extra copy.
      if (arr_len >= 4 && off < len && out_fields) {
        const uint8_t ftag = data[off];
        if (ftag == 0xC0) {
          // nil — no fields. Done.
        } else if ((ftag & 0xF0) == 0x80 || ftag == 0xDE || ftag == 0xDF) {
          size_t map_len = 0;
          if ((ftag & 0xF0) == 0x80) {
            map_len = ftag & 0x0F;
            off++;
          } else if (ftag == 0xDE) {
            if (off + 2 >= len) return true;
            map_len = (data[off + 1] << 8) | data[off + 2];
            off += 3;
          } else { // 0xDF
            if (off + 4 >= len) return true;
            map_len = ((uint32_t)data[off + 1] << 24) | ((uint32_t)data[off + 2] << 16)
                    | ((uint32_t)data[off + 3] << 8)  |  (uint32_t)data[off + 4];
            off += 5;
          }
          for (size_t i = 0; i < map_len && off < len; ++i) {
            // Key: positive fixint 0x00-0x7F (FIELD_* tags are u8).
            const uint8_t kt = data[off++];
            uint8_t key_tag = 0;
            if (kt <= 0x7F) {
              key_tag = kt;
            } else if (kt == 0xCC && off < len) {  // uint8
              key_tag = data[off++];
            } else {
              // Unsupported key shape — abort field parsing so we
              // don't desync on the rest of the payload.
              return true;
            }
            // Two shapes appear in the wild for FIELD_FILE/IMAGE/AUDIO:
            //   1. Sideband convention (interop):
            //        FIELD_IMAGE             = [ext_str, bytes]
            //        FIELD_AUDIO             = [mode_int, bytes]
            //        FIELD_FILE_ATTACHMENTS  = [[name_str, bytes], ...]
            //   2. Bare-bytes legacy (what this firmware sent before #115).
            // Detect by peeking the value tag — array means Sideband.
            const size_t value_start = off;
            if (!Common::MsgPack::skip_element(data, len, off)) return true;
            const size_t value_end = off;
            const bool is_target_tag = (key_tag == FIELD_FILE_ATTACHMENTS
                                      || key_tag == FIELD_IMAGE
                                      || key_tag == FIELD_AUDIO);
            if (!is_target_tag) continue;

            const uint8_t vt = data[value_start];
            const bool is_array = ((vt & 0xF0) == 0x90) || vt == 0xDC || vt == 0xDD;

            // Helper: read a [name|mode, bin] pair at offset `p` (which
            // points just past the pair's own array header). Populates
            // the emitted FieldBlob. Returns the next offset on success
            // or 0 on parse failure (caller bails out).
            auto emit_pair = [&](uint8_t tag, size_t p) -> size_t {
              if (p >= value_end) return 0;
              FieldBlob blob;
              blob.tag = tag;
              // Element 0 — name (str), ext (str), or mode (uint8).
              const uint8_t et = data[p];
              if (tag == FIELD_AUDIO) {
                if (et <= 0x7F)            { blob.audio_mode = et; p += 1; }
                else if (et == 0xCC && p + 1 < value_end) {
                  blob.audio_mode = data[p + 1]; p += 2;
                } else { return 0; }
              } else {
                size_t name_off = p;
                blob.filename = Common::MsgPack::read_bin_or_str(data, value_end, name_off);
                if (name_off == p) return 0;   // read failed
                p = name_off;
              }
              // Element 1 — bin payload.
              if (p >= value_end) return 0;
              const uint8_t bt = data[p];
              size_t hdr = 0, dlen = 0;
              if (bt == 0xC4 && p + 1 < value_end) { hdr = 2; dlen = data[p + 1]; }
              else if (bt == 0xC5 && p + 2 < value_end) {
                hdr = 3; dlen = (data[p+1] << 8) | data[p+2];
              } else if (bt == 0xC6 && p + 4 < value_end) {
                hdr = 5;
                dlen = ((uint32_t)data[p+1] << 24) | ((uint32_t)data[p+2] << 16)
                     | ((uint32_t)data[p+3] << 8)  |  (uint32_t)data[p+4];
              } else { return 0; }
              if (p + hdr + dlen > value_end) return 0;
              blob.raw     = data + p + hdr;
              blob.raw_len = dlen;
              out_fields->push_back(std::move(blob));
              return p + hdr + dlen;
            };

            if (is_array) {
              // Sideband: open the outer array.
              size_t p = value_start;
              size_t arr_n = 0;
              if ((vt & 0xF0) == 0x90) { arr_n = vt & 0x0F; p += 1; }
              else if (vt == 0xDC && p + 2 < value_end) {
                arr_n = (data[p+1] << 8) | data[p+2]; p += 3;
              } else if (vt == 0xDD && p + 4 < value_end) {
                arr_n = ((uint32_t)data[p+1] << 24) | ((uint32_t)data[p+2] << 16)
                      | ((uint32_t)data[p+3] << 8)  |  (uint32_t)data[p+4];
                p += 5;
              } else { continue; }

              if (key_tag == FIELD_IMAGE || key_tag == FIELD_AUDIO) {
                // The outer array IS the pair.
                (void)emit_pair(key_tag, p);
              } else {
                // FIELD_FILE_ATTACHMENTS: outer is a list of pairs. Each
                // element is itself a [name, bytes] array.
                for (size_t i = 0; i < arr_n && p < value_end; ++i) {
                  const uint8_t et = data[p];
                  size_t pair_p = p;
                  if ((et & 0xF0) == 0x90) { pair_p += 1; }
                  else if (et == 0xDC && p + 2 < value_end) { pair_p += 3; }
                  else if (et == 0xDD && p + 4 < value_end) { pair_p += 5; }
                  else break;
                  const size_t after = emit_pair(FIELD_FILE_ATTACHMENTS, pair_p);
                  if (after == 0) break;
                  p = after;
                }
              }
            } else {
              // Legacy bare-bytes shape: unwrap the bin/str header so
              // raw points at payload bytes only (not the type prefix).
              const uint8_t* inner_data = data + value_start;
              size_t inner_len = value_end - value_start;
              size_t hdr = 0, dlen = 0;
              if ((vt & 0xE0) == 0xA0) { hdr = 1; dlen = vt & 0x1F; }
              else if (vt == 0xD9 && value_start + 1 < value_end) { hdr = 2; dlen = data[value_start + 1]; }
              else if (vt == 0xDA && value_start + 2 < value_end) {
                hdr = 3; dlen = (data[value_start+1] << 8) | data[value_start+2];
              } else if (vt == 0xDB && value_start + 4 < value_end) {
                hdr = 5;
                dlen = ((uint32_t)data[value_start+1] << 24) | ((uint32_t)data[value_start+2] << 16)
                     | ((uint32_t)data[value_start+3] << 8)  |  (uint32_t)data[value_start+4];
              } else if (vt == 0xC4 && value_start + 1 < value_end) { hdr = 2; dlen = data[value_start + 1]; }
              else if (vt == 0xC5 && value_start + 2 < value_end) {
                hdr = 3; dlen = (data[value_start+1] << 8) | data[value_start+2];
              } else if (vt == 0xC6 && value_start + 4 < value_end) {
                hdr = 5;
                dlen = ((uint32_t)data[value_start+1] << 24) | ((uint32_t)data[value_start+2] << 16)
                     | ((uint32_t)data[value_start+3] << 8)  |  (uint32_t)data[value_start+4];
              }
              if (hdr != 0 && value_start + hdr + dlen <= value_end) {
                inner_data = data + value_start + hdr;
                inner_len  = dlen;
              }
              FieldBlob blob;
              blob.tag = key_tag;
              blob.raw = inner_data;
              blob.raw_len = inner_len;
              out_fields->push_back(std::move(blob));
            }
          }
        }
      }
      return true;
    }

    static double _compile_time_epoch() {
      // 2025-01-01 00:00:00 UTC = 1735689600. Conservative baseline until
      // calibrated by a peer.
      return 1735689600.0;
    }

  private:
    RNS::Identity     _identity;
    RNS::Destination  _destination;
    bool              _initialized;
    double            _time_offset;
    bool              _time_calibrated;
    std::string       _display_name;
    DeliveryCallback  _on_delivery;
    OutboxStatusCallback _on_outbox_status;
    ProgressCallback  _on_progress;
    ReceiveCompleteCallback _on_receive_complete;
    AttachmentPersistFn _persist_attachments_fn;
    OutboundPersistFn   _persist_outbound_fn;
  };

} // namespace LXMF
