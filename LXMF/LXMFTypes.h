#pragma once

#include <Bytes.h>
#include <string>
#include <stdint.h>

namespace LXMF {

  // LXMF wire protocol constants.
  static constexpr size_t HASH_LEN   = 16;   // RNS truncated hash (128 bits)
  static constexpr size_t SIG_LEN    = 64;   // Ed25519 signature
  static constexpr size_t HEADER_LEN = HASH_LEN + SIG_LEN;  // 80 bytes

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
  };

}
