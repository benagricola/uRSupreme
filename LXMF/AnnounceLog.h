#pragma once

// AnnounceLog — bounded RAM ring of recent LXMF (lxmf.delivery)
// announces received via Reticulum's Transport. Registers a
// `Transport::AnnounceHandler` with `aspect_filter="lxmf.delivery"` so
// only addressable LXMF endpoints land here — transport-relay
// announces (which are noise to the messaging UI) are filtered at the
// Reticulum layer.
//
// LXMF announce app_data is msgpack `[display_name(bin), stamp_cost(nil)]`.
// We decode element 0 to surface a human-readable label.

#include <Log.h>
#include <Transport.h>
#include <Bytes.h>
#include <Identity.h>

#include <deque>
#include <string>

#include "LXMFTypes.h"
#include "LXMFMinimal.h"   // For RawMsgPack helpers

namespace LXMF {

  struct AnnounceRecord {
    uint32_t    received_ms;
    RNS::Bytes  destination;   // 16-byte lxmf.delivery destination hash
    std::string display_name;
  };

  // Defined out-of-class below to avoid a circular include with LXMFGateway.
  bool announce_log_is_own_account(const RNS::Bytes& destination_hash);

  class AnnounceLog {
  public:
    static constexpr size_t CAPACITY = 32;

    static void setup() {
      if (_handler) return;
      _handler = std::make_shared<Handler>();
      RNS::Transport::register_announce_handler(_handler);
      NOTICE("LXMF::AnnounceLog: registered handler with aspect_filter=lxmf.delivery");
    }

    static std::deque<AnnounceRecord>& records() {
      static std::deque<AnnounceRecord> r;
      return r;
    }

  private:
    class Handler : public RNS::AnnounceHandler {
    public:
      Handler() : RNS::AnnounceHandler("lxmf.delivery") {}

      virtual void received_announce(const RNS::Bytes& destination_hash,
                                     const RNS::Identity& announced_identity,
                                     const RNS::Bytes& app_data) override {
        std::string display_name;
        if (app_data.size() > 0) {
          const uint8_t* data = app_data.data();
          size_t len = app_data.size();
          size_t off = 0;
          if (len >= 1 && (data[off] & 0xF0) == 0x90) {
            off++;  // fixarray header
            display_name = RawMsgPack::read_bin_or_str(data, len, off);
          }
        }

        // Skip our own accounts' announces.
        if (announce_log_is_own_account(destination_hash)) return;

        auto& ring = AnnounceLog::records();
        for (auto& rec : ring) {
          if (rec.destination == destination_hash) {
            rec.received_ms = millis();
            if (!display_name.empty()) rec.display_name = display_name;
            return;
          }
        }
        AnnounceRecord rec;
        rec.received_ms = millis();
        rec.destination = destination_hash;
        rec.display_name = display_name;
        ring.push_back(rec);
        while (ring.size() > CAPACITY) ring.pop_front();
        NOTICEF("LXMF::AnnounceLog: new %s (%s)",
                destination_hash.toHex().c_str(),
                display_name.c_str());
      }
    };

    static inline RNS::HAnnounceHandler _handler{nullptr};
  };

}
