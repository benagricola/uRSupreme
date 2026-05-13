#pragma once

// AnnounceLog — two parallel RAM rings of recent Reticulum announces:
//
//   * `announces` — only lxmf.delivery destinations, with decoded
//     display_name. Drives the "Recent announces" UI.
//   * `paths` — every announce regardless of aspect. Drives the "Known
//     paths" UI. We need our own ring because microReticulum's
//     in-memory `_path_table` is dead code (see Transport.cpp:2204
//     and its surrounding CBA-microStore migration comments); the live
//     path entries go into the private `_new_path_table` microStore,
//     which has no public iteration API.
//
// Two `Transport::AnnounceHandler` instances back the rings: one with
// `aspect_filter="lxmf.delivery"` for the announces ring, one with no
// filter for the paths ring. lxmf.delivery announces land in both.

#include <Log.h>
#include <Transport.h>
#include <Bytes.h>
#include <Identity.h>

#include <deque>
#include <string>

#include "LXMFTypes.h"
#include "LXMFMinimal.h"   // for RawMsgPack helpers

namespace LXMF {

  struct AnnounceRecord {
    uint32_t    received_ms;
    RNS::Bytes  destination;
    std::string display_name;  // empty for non-LXMF announces
  };

  // Implemented below (in LXMFGateway.h) to break the include cycle.
  bool announce_log_is_own_account(const RNS::Bytes& destination_hash);

  class AnnounceLog {
  public:
    static constexpr size_t CAPACITY = 32;

    static void setup() {
      if (_lxmf_handler) return;
      _lxmf_handler = std::make_shared<Handler>(true);   // aspect_filter=lxmf.delivery
      _path_handler = std::make_shared<Handler>(false);  // no filter
      RNS::Transport::register_announce_handler(_lxmf_handler);
      RNS::Transport::register_announce_handler(_path_handler);
      NOTICE("LXMF::AnnounceLog: registered handlers (lxmf.delivery + all)");
    }

    // Lxmf-only ring — feeds /api/announces.
    static std::deque<AnnounceRecord>& announces() {
      static std::deque<AnnounceRecord> r;
      return r;
    }

    // All-aspect ring — feeds /api/paths.
    static std::deque<AnnounceRecord>& paths() {
      static std::deque<AnnounceRecord> r;
      return r;
    }

  private:
    class Handler : public RNS::AnnounceHandler {
    public:
      // lxmf_only=true → aspect_filter "lxmf.delivery" so only LXMF
      // endpoints arrive. false → nullptr filter (everything).
      explicit Handler(bool lxmf_only)
        : RNS::AnnounceHandler(lxmf_only ? "lxmf.delivery" : nullptr),
          _lxmf_only(lxmf_only) {}

      virtual void received_announce(const RNS::Bytes& destination_hash,
                                     const RNS::Identity& announced_identity,
                                     const RNS::Bytes& app_data) override {
        if (announce_log_is_own_account(destination_hash)) return;

        std::string display_name;
        if (app_data.size() > 0) {
          const uint8_t* data = app_data.data();
          size_t len = app_data.size();
          size_t off = 0;
          if (len >= 1 && (data[off] & 0xF0) == 0x90) {
            off++;
            display_name = RawMsgPack::read_bin_or_str(data, len, off);
          }
        }

        auto& ring = _lxmf_only ? AnnounceLog::announces() : AnnounceLog::paths();
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
        NOTICEF("LXMF::AnnounceLog: %s announce %s (%s)",
                _lxmf_only ? "lxmf" : "path",
                destination_hash.toHex().c_str(),
                display_name.c_str());
      }

    private:
      bool _lxmf_only;
    };

    static inline RNS::HAnnounceHandler _lxmf_handler{nullptr};
    static inline RNS::HAnnounceHandler _path_handler{nullptr};
  };

}
