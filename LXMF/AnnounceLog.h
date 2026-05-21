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
#include <Destination.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "LXMFTypes.h"
#include "../Common/MsgPack.h"

namespace LXMF {

  struct AnnounceRecord {
    uint32_t    received_ms;
    RNS::Bytes  destination;
    std::string display_name;  // empty for non-LXMF announces
    std::string aspect;        // detected aspect tuple, e.g. "lxmf.delivery", "lxmf.propagation", or "" if unrecognised
  };

  // Implemented below (in LXMFGateway.h) to break the include cycle.
  bool announce_log_is_own_identity(const RNS::Bytes& destination_hash);

  class AnnounceLog {
  public:
    static constexpr size_t CAPACITY = 32;

    // Subscribers invoked synchronously after each new announce has been
    // appended to the rings. is_lxmf=true for lxmf.delivery announces
    // (the "announces" ring), false for the everything-ring "paths".
    // Subscribers must not block — they run on the LoRa RX thread.
    using AnnounceCallback = std::function<void(const AnnounceRecord&, bool is_lxmf)>;
    static void on_new_announce(AnnounceCallback cb) {
      callbacks().push_back(std::move(cb));
    }

    static std::vector<AnnounceCallback>& callbacks() {
      static std::vector<AnnounceCallback> s;
      return s;
    }

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
        if (announce_log_is_own_identity(destination_hash)) return;

        std::string display_name;
        if (app_data.size() > 0) {
          const uint8_t* data = app_data.data();
          size_t len = app_data.size();
          size_t off = 0;
          if (len >= 1 && (data[off] & 0xF0) == 0x90) {
            off++;
            display_name = Common::MsgPack::read_bin_or_str(data, len, off);
          }
        }

        // Identify which aspect tuple this announce is for by recomputing
        // the destination hash for each well-known aspect and comparing.
        // The aspect_filter mechanism in Transport already does this for
        // filter matching; we replicate it here for labelling so the
        // operator can see at a glance whether the announce was for
        // lxmf.delivery (chat-addressable), lxmf.propagation (propnode),
        // nomadnetwork.node (Nomad Network), or unknown/bare-identity.
        std::string aspect;
        static const char* candidates[] = {
          "lxmf.delivery", "lxmf.propagation", "nomadnetwork.node"
        };
        for (const char* a : candidates) {
          if (RNS::Destination::hash_from_name_and_identity(a, announced_identity) == destination_hash) {
            aspect = a;
            break;
          }
        }

        auto& ring = _lxmf_only ? AnnounceLog::announces() : AnnounceLog::paths();
        for (auto& rec : ring) {
          if (rec.destination == destination_hash) {
            rec.received_ms = millis();
            if (!display_name.empty()) rec.display_name = display_name;
            if (!aspect.empty())       rec.aspect       = aspect;
            notify_subscribers(rec, _lxmf_only);
            return;
          }
        }
        AnnounceRecord rec;
        rec.received_ms = millis();
        rec.destination = destination_hash;
        rec.display_name = display_name;
        rec.aspect       = aspect;
        ring.push_back(rec);
        while (ring.size() > CAPACITY) ring.pop_front();
        NOTICEF("LXMF::AnnounceLog: %s announce %s aspect=%s (%s)",
                _lxmf_only ? "lxmf" : "path",
                destination_hash.toHex().c_str(),
                aspect.empty() ? "transport/unknown" : aspect.c_str(),
                display_name.c_str());
        notify_subscribers(rec, _lxmf_only);
      }

      static void notify_subscribers(const AnnounceRecord& rec, bool is_lxmf) {
        for (auto& cb : AnnounceLog::callbacks()) {
          try { cb(rec, is_lxmf); }
          catch (...) { /* one subscriber's failure must not break the chain */ }
        }
      }

    private:
      bool _lxmf_only;
    };

    static inline RNS::HAnnounceHandler _lxmf_handler{nullptr};
    static inline RNS::HAnnounceHandler _path_handler{nullptr};
  };

}
