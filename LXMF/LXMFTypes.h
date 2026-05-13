#pragma once

#include <Bytes.h>
#include <string>
#include <stdint.h>

namespace LXMF {

  // LXMF wire protocol constants.
  static constexpr size_t HASH_LEN   = 16;   // RNS truncated hash (128 bits)
  static constexpr size_t SIG_LEN    = 64;   // Ed25519 signature
  static constexpr size_t HEADER_LEN = HASH_LEN + SIG_LEN;  // 80 bytes

  // Account identifier — the first 16 hex chars of an Identity's hexhash.
  using AccountId = std::string;

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
  struct MessageRecord {
    uint32_t      seq;            // Monotonic per-account sequence number.
    double        ts;             // LXMF timestamp (peer's clock for incoming, local for outgoing).
    RNS::Bytes    peer_hash;      // Remote LXMF address (16 bytes). For incoming: source. For outgoing: destination.
    std::string   title;          // LXMF title field (often empty).
    std::string   content;        // Plaintext message content.
    bool          incoming;       // True for received, false for sent.
    bool          signature_ok;   // For incoming: did the Ed25519 signature verify against a known identity? For outgoing: always true.
    OutboxStatus  status;         // Only meaningful for outgoing messages; Delivered for incoming.
    RNS::Bytes    packet_hash;    // RNS packet hash, used to correlate delivery receipts.
  };

}
