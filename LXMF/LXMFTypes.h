#pragma once

#include <Bytes.h>
#include <string>
#include <vector>
#include <stdint.h>
#include <string.h>

namespace LXMF {

  // LXMF wire protocol constants.
  static constexpr size_t HASH_LEN   = 16;   // RNS truncated hash (128 bits)
  static constexpr size_t SIG_LEN    = 64;   // Ed25519 signature
  static constexpr size_t HEADER_LEN = HASH_LEN + SIG_LEN;  // 80 bytes

  // LXMF fields-dict tags we recognise (LXMF-upstream/LXMF/LXMF.py).
  // Only the file-shaped tags are persisted on-device for now —
  // FIELD_TELEMETRY / FIELD_RENDERER / FIELD_TICKET / etc. flow through
  // as unparsed bytes that the SPA can read out of /api/info if it
  // ever needs them.
  static constexpr uint8_t FIELD_FILE_ATTACHMENTS = 0x05;
  static constexpr uint8_t FIELD_IMAGE            = 0x06;
  static constexpr uint8_t FIELD_AUDIO            = 0x07;

  // Identity identifier — the first 16 hex chars of an Identity's hexhash.
  using IdentityId = std::string;

  // Per-peer retention policy. Two shapes:
  //   Time:  keep messages newer than `value` seconds. value=0 invalid.
  //   Count: keep the newest `value` messages from this peer. value=0 invalid.
  //   None:  no expiry / no per-peer cap (effectively "keep forever",
  //          bounded only by the LXMFInbox-wide ring capacity).
  //
  // The global LXMFInbox default is snapshotted onto each peer the
  // first time we see a message from / to them, so subsequent changes
  // to the default don't retroactively touch existing chats. Per-peer
  // overrides are set via the SPA's per-chat retention modal.
  struct Retention {
    enum class Kind : uint8_t { None = 0, Time = 1, Count = 2 };
    Kind     kind  = Kind::None;
    uint32_t value = 0;
    bool operator==(const Retention& o) const { return kind == o.kind && value == o.value; }
    bool operator!=(const Retention& o) const { return !(*this == o); }
  };
  inline const char* retention_kind_name(Retention::Kind k) {
    switch (k) {
      case Retention::Kind::Time:  return "time";
      case Retention::Kind::Count: return "count";
      default:                     return "none";
    }
  }
  inline Retention::Kind retention_kind_from_str(const char* s) {
    if (!s) return Retention::Kind::None;
    if (strcmp(s, "time")  == 0) return Retention::Kind::Time;
    if (strcmp(s, "count") == 0) return Retention::Kind::Count;
    return Retention::Kind::None;
  }

  // Outbound message status, tracked per send.
  enum class OutboxStatus : uint8_t {
    Queued    = 0,
    Sent      = 1,
    Delivered = 2,
    Failed    = 3,
  };

  inline const char* outbox_status_name(OutboxStatus s) {
    switch (s) {
      case OutboxStatus::Queued:    return "queued";
      case OutboxStatus::Sent:      return "sent";
      case OutboxStatus::Delivered: return "delivered";
      case OutboxStatus::Failed:    return "failed";
    }
    return "unknown";
  }

  // Attachment metadata persisted alongside a MessageRecord. The actual
  // bytes live at <a->dir()>/attachments/<filename> — we just store a
  // pointer so the inbox JSONL line stays small (the body itself is
  // capped at LXMF_MAX_BODY_BYTES per LXMFMinimal.h, and per-attachment
  // name + mime are capped via LXMF_MAX_ATTACHMENT_NAME / _MIME).
  //
  // tag matches the LXMF FIELD_* constant. filename is the on-disk stem
  // ("<msg_hash_hex>_<tag>_<n>.bin") generated at receive time — used
  // as the URL path on /api/.../attachments/<filename>. display_name
  // is the sender-supplied original name (Sideband convention) and is
  // ONLY for UI display + download-prompt; never trust it for disk
  // access. mime is the sender-declared content type if any; empty if
  // the wire format didn't carry it.
  struct AttachmentMeta {
    uint8_t     tag;
    uint32_t    size;
    std::string filename;       // on-disk stem (attacker-safe)
    std::string display_name;   // sender-supplied label (display-only)
    std::string mime;
    // Where the bytes actually live. "flash" (default) = LittleFS via
    // the microStore filesystem; "sd" = T-Beam Supreme's microSD slot
    // via the Arduino SD library. The download endpoint dispatches
    // on this value.
    std::string backend;
  };

  // A received or sent message, in normalized form for inbox/outbox storage.
  //
  // Ordering: the authoritative in-device ordering key is the tuple
  // (boot_epoch, received_ms), NOT received_ms alone. millis() resets
  // to 0 on each reboot, so received_ms is only monotonic within a
  // single boot session — boot_epoch (incremented at each boot and
  // persisted via Web::BootCounter) makes the tuple globally monotonic
  // across reboots. `ts` is the LXMF wire timestamp; it depends on
  // peer clocks and is not reliable for sorting.
  // Scalar fields default-initialised so a stack-allocated record never
  // carries garbage into append(). LXMFInbox::append relies on seq==0
  // to allocate a fresh seq, so an uninitialised seq turns into the
  // garbage stack value and short-circuits the auto-assignment.
  struct MessageRecord {
    uint32_t      seq         = 0;       // Monotonic per-identity-per-mailbox sequence number (inbox and outbox have independent counters).
    double        ts          = 0.0;     // LXMF timestamp (peer's clock for incoming, local for outgoing). Display-only; not reliable for ordering when peer clocks aren't synced.
    uint32_t      boot_epoch  = 0;       // Web::BootCounter value at append time. Combined with received_ms forms (boot_epoch, received_ms) — a tuple monotonic across reboots.
    uint32_t      received_ms = 0;       // millis() at the moment this record was appended to its inbox/outbox. Monotonic only within boot_epoch.
    RNS::Bytes    peer_hash;             // Remote LXMF address (16 bytes). For incoming: source. For outgoing: destination.
    std::string   title;                 // LXMF title field (often empty).
    std::string   content;               // Plaintext message content, always inline. Bounded at LXMF_MAX_BODY_BYTES (4 KiB) by the send path.
    uint32_t      body_size   = 0;       // Total body size in bytes (== content.size()). Kept as a separate field for wire-shape stability with the SPA's body_size === content.length check, and so per-record size queries don't have to count the std::string each time.
    bool          incoming    = false;   // True for received, false for sent.
    bool          signature_ok = false;  // For incoming: did the Ed25519 signature verify against a known identity? For outgoing: always true.
    OutboxStatus  status      = OutboxStatus::Delivered;  // Only meaningful for outgoing messages; Delivered for incoming.
    RNS::Bytes    packet_hash;           // RNS packet hash, used to correlate delivery receipts.
    std::vector<AttachmentMeta> attachments;  // Empty for messages without LXMF fields.
  };

}
