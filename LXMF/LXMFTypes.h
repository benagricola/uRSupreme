#pragma once

#include <Bytes.h>
#include <string>
#include <vector>
#include <stdint.h>

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
  // capped at MAX_LINE_BYTES per LXMFInbox.h).
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
  struct MessageRecord {
    uint32_t      seq;            // Monotonic per-identity-per-mailbox sequence number (inbox and outbox have independent counters).
    double        ts;             // LXMF timestamp (peer's clock for incoming, local for outgoing). Display-only; not reliable for ordering when peer clocks aren't synced.
    uint32_t      boot_epoch;     // Web::BootCounter value at append time. Combined with received_ms forms (boot_epoch, received_ms) — a tuple monotonic across reboots.
    uint32_t      received_ms;    // millis() at the moment this record was appended to its inbox/outbox. Monotonic only within boot_epoch.
    RNS::Bytes    peer_hash;      // Remote LXMF address (16 bytes). For incoming: source. For outgoing: destination.
    std::string   title;          // LXMF title field (often empty).
    std::string   content;        // Plaintext message content.
    bool          incoming;       // True for received, false for sent.
    bool          signature_ok;   // For incoming: did the Ed25519 signature verify against a known identity? For outgoing: always true.
    OutboxStatus  status;         // Only meaningful for outgoing messages; Delivered for incoming.
    RNS::Bytes    packet_hash;    // RNS packet hash, used to correlate delivery receipts.
    std::vector<AttachmentMeta> attachments;  // Empty for messages without LXMF fields.
  };

}
