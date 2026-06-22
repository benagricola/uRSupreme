#pragma once

// LXMF propagation-node CLIENT support.
//
// This header holds the device-wide propagation config (persisted to
// /lxmf/propagation.json, exposed by the web app) and a RAM registry of
// propagation nodes learned from lxmf.propagation announces (mirrors
// AnnounceLog; rebuilds from announces after a reboot, so it is not
// persisted). The sync/transfer state machine and the outbound PROPAGATED
// path are not yet implemented; they will pull current() / the chosen node
// here.
//
// The config is DEVICE-WIDE: one propagation node for all identities (upstream
// LXMRouter is router-wide, LXMRouter.py:389). Inbound sync will identify per
// identity at sync time so each gets its own mailbox.
//
// Faithful to upstream LXMF: a PN announces on Destination(identity, SINGLE,
// "lxmf", "propagation") (LXMRouter.py:509) with msgpack app_data
//   [ legacy(bool), timebase(uint), node_state(bool), per_transfer_kb(uint),
//     per_sync(uint), [stamp_cost, flex, peering](uint[]), metadata(map) ]
// (LXMRouter.get_propagation_node_app_data, LXMRouter.py:306-318). The node
// name is metadata key PN_META_NAME = 0x01 (LXMF.py:99).

#include <stdint.h>
#include <vector>
#include <deque>
#include <string>
#include <memory>

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Log.h>
#include <Bytes.h>
#include <Identity.h>
#include <Transport.h>
#include <Link.h>
#include <Destination.h>
#include <microStore/FileSystem.h>

#include "../Common/PsramAllocator.h"
#include "../Common/MsgPack.h"
#include "LXMFMinimal.h"

namespace LXMF {
namespace Propagation {

// ---- device-wide config (/lxmf/propagation.json) -------------------------

inline constexpr const char* CONFIG_PATH = "/lxmf/propagation.json";

struct Config {
  bool        enabled          = false;  // master switch for propagation
  std::string pn_hash;                   // chosen PN destination hash (hex); "" = none
  uint32_t    sync_interval_s  = 0;      // 0 = manual sync only; else periodic cadence
  bool        retain_on_node   = false;  // true = keep synced messages on the PN
  bool        use_when_offline = true;   // outbound: fall back to the PN when a peer is unreachable
};

inline Config& current() { static Config c; return c; }

// A Reticulum destination hash is 16 bytes = 32 hex chars; clamp anything
// else to empty so a malformed value can't target a bogus destination.
inline void sanitize(Config& c) {
  if (!c.pn_hash.empty() && c.pn_hash.size() != 32) c.pn_hash.clear();
  if (c.sync_interval_s != 0 && c.sync_interval_s < 60) c.sync_interval_s = 60;
  if (c.sync_interval_s > 24UL * 3600) c.sync_interval_s = 24UL * 3600;
}

inline void load(microStore::FileSystem& fs) {
  Config& c = current();
  c = Config{};
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  if (doc["enabled"].is<bool>())           c.enabled          = doc["enabled"].as<bool>();
  if (doc["pn_hash"].is<const char*>())     c.pn_hash          = doc["pn_hash"].as<const char*>();
  if (doc["sync_interval_s"].is<uint32_t>()) c.sync_interval_s = doc["sync_interval_s"].as<uint32_t>();
  if (doc["retain_on_node"].is<bool>())     c.retain_on_node   = doc["retain_on_node"].as<bool>();
  if (doc["use_when_offline"].is<bool>())   c.use_when_offline = doc["use_when_offline"].as<bool>();
  sanitize(c);
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = current();
  Common::PsramJsonDocument doc;
  doc["enabled"]          = c.enabled;
  doc["pn_hash"]          = c.pn_hash;
  doc["sync_interval_s"]  = c.sync_interval_s;
  doc["retain_on_node"]   = c.retain_on_node;
  doc["use_when_offline"] = c.use_when_offline;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

inline void set(microStore::FileSystem& fs, const Config& next) {
  current() = next;
  sanitize(current());
  persist(fs);
}

// ---- PN registry (discovered nodes, RAM-only) ----------------------------

struct Node {
  RNS::Bytes  hash;                 // destination hash
  std::string name;                 // metadata name (may be empty)
  bool        active          = true;  // node_state (announce field 2)
  uint16_t    stamp_cost      = 0;     // propagation stamp cost (field 5[0])
  uint32_t    per_transfer_kb = 0;     // field 3
  uint32_t    per_sync        = 0;     // field 4
  uint32_t    last_seen_ms    = 0;
};

static constexpr size_t CAPACITY = 16;

inline std::deque<Node>& nodes() { static std::deque<Node> d; return d; }

// Best-effort parse of the announce app_data into n (hash/last_seen set by the
// caller). Returns false only when the payload isn't a usable array; partial
// arrays keep the fields parsed so far.
inline bool parse_app_data(const RNS::Bytes& app_data, Node& n) {
  const uint8_t* d = app_data.data();
  size_t len = app_data.size();
  size_t off = 0;
  int count = Common::MsgPack::read_array_header(d, len, off);
  if (count < 3) return false;                          // need at least node_state
  Common::MsgPack::skip_element(d, len, off);           // 0: legacy
  Common::MsgPack::skip_element(d, len, off);           // 1: timebase
  n.active = Common::MsgPack::read_bool(d, len, off);   // 2: node_state
  if (count < 4) return true;
  n.per_transfer_kb = (uint32_t)Common::MsgPack::read_uint(d, len, off);  // 3
  if (count < 5) return true;
  n.per_sync = (uint32_t)Common::MsgPack::read_uint(d, len, off);         // 4
  if (count < 6) return true;
  int ac = Common::MsgPack::read_array_header(d, len, off);               // 5: [cost,flex,peer]
  if (ac >= 1) {
    n.stamp_cost = (uint16_t)Common::MsgPack::read_uint(d, len, off);
    for (int i = 1; i < ac; i++) Common::MsgPack::skip_element(d, len, off);
  }
  if (count < 7) return true;
  int mc = Common::MsgPack::read_map_header(d, len, off);                 // 6: metadata
  for (int i = 0; i < mc; i++) {
    bool kok = false;
    uint64_t key = Common::MsgPack::read_uint(d, len, off, &kok);
    if (kok && key == 0x01) n.name = Common::MsgPack::read_bin_or_str(d, len, off);  // PN_META_NAME
    else Common::MsgPack::skip_element(d, len, off);
  }
  return true;
}

class Handler : public RNS::AnnounceHandler {
public:
  // aspect_filter "lxmf.propagation" => Transport only delivers PN announces.
  Handler() : RNS::AnnounceHandler("lxmf.propagation") {}

  virtual void received_announce(const RNS::Bytes& destination_hash,
                                 const RNS::Identity& announced_identity,
                                 const RNS::Bytes& app_data) override {
    Node n;
    n.hash = destination_hash;
    n.last_seen_ms = millis();
    parse_app_data(app_data, n);

    auto& d = nodes();
    for (auto& e : d) {
      if (e.hash == destination_hash) {
        e.last_seen_ms    = n.last_seen_ms;
        e.active          = n.active;
        e.stamp_cost      = n.stamp_cost;
        e.per_transfer_kb = n.per_transfer_kb;
        e.per_sync        = n.per_sync;
        if (!n.name.empty()) e.name = n.name;
        return;
      }
    }
    d.push_front(n);
    while (d.size() > CAPACITY) d.pop_back();
  }
};

inline void setup() {
  static std::shared_ptr<Handler> h;
  if (h) return;
  h = std::make_shared<Handler>();
  RNS::Transport::register_announce_handler(h);
  NOTICE("LXMF::Propagation: registered lxmf.propagation announce handler");
}

// ---- inbound sync (pull stored messages from the configured PN) ----------
//
// Ports upstream LXMRouter.request_messages_from_propagation_node
// (LXMRouter.py:484), message_list_response (1506) and message_get_response
// (1551). Device-wide: iterates the local delivery identities, identifying as
// each on its own Link to the PN so each gets its own mailbox. The RNS Link
// request callbacks are bare C function pointers, so all sync state is static
// (one sync runs at a time, mirroring upstream's single outbound link).
//
// The /get protocol (LXMRouter.message_get_request, LXMRouter.py:1426):
//   [nil, nil]            -> a list of transient_ids the PN holds for us
//   [wants, haves, limit] -> the wanted message blobs (and purges the haves)
//   [nil, haves]          -> purge (skipped when retain_on_node is set)
// Each blob is dest_hash(16)||encrypted_payload; transient_id = full_hash(blob).

namespace Sync {

enum State : uint8_t {
  IDLE = 0, PATH_WAIT, LINK_WAIT, SETTLE, LISTING, GETTING, ACK_FLUSH, ADVANCE, COMPLETE, FAILED
};

// Bounded so a hung PN cannot wedge the loop.
inline constexpr uint32_t PATH_TIMEOUT_MS = 15000;
inline constexpr uint32_t LINK_TIMEOUT_MS = 20000;
inline constexpr uint32_t SETTLE_MS       = 1500;  // after identify, let the node finish the handshake before requesting
inline constexpr uint32_t REQ_TIMEOUT_MS  = 30000;
inline constexpr uint32_t ACK_FLUSH_MS    = 2000;
inline constexpr size_t   MAX_WANTS       = 32;   // messages per identity per sync
inline constexpr size_t   SEEN_MAX        = 64;   // RAM dedup of synced transient ids
inline constexpr uint32_t PER_TRANSFER_KB = 256;  // limit we advertise on the get

struct Ctx {
  State       state         = IDLE;
  RNS::Bytes  pn_hash;                  // PN destination hash (bytes)
  RNS::Bytes  cur_iden;                 // identity dest hash being synced
  uint32_t    deadline_ms   = 0;
  uint32_t    last_sync_ms  = 0;
  uint32_t    next_auto_ms  = 0;
  int         run_received  = 0;        // messages this run
  int         last_received = 0;        // messages last completed run
  bool        last_ok       = false;
  bool        reached       = false;    // a link established at least once this run
  const char* last_error    = "";
};
inline Ctx& ctx() { static Ctx c; return c; }
inline RNS::Link& slink() { static RNS::Link l{RNS::Type::NONE}; return l; }
inline std::vector<RNS::Bytes>& queue() { static std::vector<RNS::Bytes> q; return q; }
inline std::vector<RNS::Bytes>& haves() { static std::vector<RNS::Bytes> h; return h; }
inline std::deque<RNS::Bytes>&  seen()  { static std::deque<RNS::Bytes> s; return s; }

inline bool is_busy() {
  State s = ctx().state;
  return s != IDLE && s != COMPLETE && s != FAILED;
}
inline bool seen_transient(const RNS::Bytes& t) {
  for (auto& e : seen()) if (e == t) return true;
  return false;
}
inline void mark_transient(const RNS::Bytes& t) {
  seen().push_back(t);
  while (seen().size() > SEEN_MAX) seen().pop_front();
}

// msgpack request packers (append to a byte vector)
inline void mp_nil(std::vector<uint8_t>& v) { v.push_back(0xc0); }
inline void mp_arr(std::vector<uint8_t>& v, size_t n) {
  if (n <= 15) v.push_back(0x90 | (uint8_t)n);
  else { v.push_back(0xdc); v.push_back((n >> 8) & 0xff); v.push_back(n & 0xff); }
}
inline void mp_bin(std::vector<uint8_t>& v, const RNS::Bytes& b) {
  size_t n = b.size();
  if (n <= 255) { v.push_back(0xc4); v.push_back((uint8_t)n); }
  else { v.push_back(0xc5); v.push_back((n >> 8) & 0xff); v.push_back(n & 0xff); }
  v.insert(v.end(), b.data(), b.data() + n);
}
inline void mp_uint(std::vector<uint8_t>& v, uint64_t n) {
  if (n <= 0x7f) v.push_back((uint8_t)n);
  else if (n <= 0xff) { v.push_back(0xcc); v.push_back((uint8_t)n); }
  else if (n <= 0xffff) { v.push_back(0xcd); v.push_back((n >> 8) & 0xff); v.push_back(n & 0xff); }
  else { v.push_back(0xce); v.push_back((n >> 24) & 0xff); v.push_back((n >> 16) & 0xff); v.push_back((n >> 8) & 0xff); v.push_back(n & 0xff); }
}
inline RNS::Bytes get_path() { RNS::Bytes p; const char* s = "/get"; p.append((const uint8_t*)s, 4); return p; }
inline RNS::Bytes vec_to_bytes(const std::vector<uint8_t>& v) { RNS::Bytes b; b.append(v.data(), v.size()); return b; }

// Forward declarations (the callbacks reference each other and helpers below).
inline void on_established(RNS::Link& l);
inline void on_closed(RNS::Link& l);
inline void on_list(const RNS::RequestReceipt& rr);
inline void on_get(const RNS::RequestReceipt& rr);
inline void on_failed(const RNS::RequestReceipt& rr);
inline void begin_next();
inline void open_link();
inline void advance(const char* err);

// Tearing a link down from inside its own request/established callback faults
// RNS (the callback runs while RNS is mid-iteration over that link's state).
// So teardown() is only ever called from pump() and start(), never a callback.
inline void teardown() {
  RNS::Link& l = slink();
  if (l && l.status() != RNS::Type::Link::CLOSED) l.teardown();
  slink() = RNS::Link(RNS::Type::NONE);
}

inline void finish_run(bool ok) {
  teardown();
  Ctx& c = ctx();
  c.last_ok = ok && c.reached;
  c.last_received = c.run_received;
  c.last_sync_ms = millis();
  c.state = c.last_ok ? COMPLETE : FAILED;
  if (!c.reached && c.last_error[0] == '\0') c.last_error = "could not reach node";
  NOTICEF("LXMF::Propagation: sync done (%s), %d message(s)",
          c.last_ok ? "ok" : "failed", c.run_received);
}

inline void open_link() {
  RNS::Identity id = RNS::Identity::recall(ctx().pn_hash);
  if (!id) {  // identity arrives with the path; request it and wait
    RNS::Transport::request_path(ctx().pn_hash);
    ctx().state = PATH_WAIT;
    ctx().deadline_ms = millis() + PATH_TIMEOUT_MS;
    return;
  }
  RNS::Destination pn(id, RNS::Type::Destination::OUT, RNS::Type::Destination::SINGLE,
                      "lxmf", "propagation");
  slink() = RNS::Link(pn, on_established, on_closed);
  ctx().state = LINK_WAIT;
  ctx().deadline_ms = millis() + LINK_TIMEOUT_MS;
}

// Open a link to sync the next queued identity, or finish the run. The previous
// link must already be torn down by the caller (pump's ADVANCE / start), so
// this is never reached with a live link held inside a callback.
inline void begin_next() {
  haves().clear();
  if (queue().empty()) { finish_run(true); return; }
  ctx().cur_iden = queue().back();
  queue().pop_back();
  if (!RNS::Transport::has_path(ctx().pn_hash)) {
    RNS::Transport::request_path(ctx().pn_hash);
    ctx().state = PATH_WAIT;
    ctx().deadline_ms = millis() + PATH_TIMEOUT_MS;
    return;
  }
  open_link();
}

// Mark the current identity done (optionally with an error) and let pump() do
// the link teardown + move to the next identity outside the callback.
inline void advance(const char* err) {
  if (err && err[0]) ctx().last_error = err;
  ctx().state = ADVANCE;
}

// Send the message-list request ([nil, nil]). Called from pump() after the
// settle window, so the node has finished the handshake (LRRTT) and processed
// our identify before the request lands.
inline void send_list() {
  std::vector<uint8_t> req; mp_arr(req, 2); mp_nil(req); mp_nil(req);  // [nil, nil]
  slink().request(get_path(), vec_to_bytes(req), on_list, on_failed, nullptr, 0);
  ctx().state = LISTING;
  ctx().deadline_ms = millis() + REQ_TIMEOUT_MS;
}

inline void on_established(RNS::Link& l) {
  ctx().reached = true;
  auto& reg = LXMFMinimal::instances();
  auto it = reg.find(ctx().cur_iden);
  if (it == reg.end() || !it->second) { advance("identity gone"); return; }
  l.identify(it->second->identity());
  ctx().state = SETTLE;                  // request is issued from pump() after SETTLE_MS
  ctx().deadline_ms = millis() + SETTLE_MS;
}

inline void on_closed(RNS::Link& l) {
  if (ctx().state == LINK_WAIT || ctx().state == LISTING || ctx().state == GETTING)
    advance("link closed");
}

inline void on_failed(const RNS::RequestReceipt& rr) {
  advance("request failed");
}

inline void on_list(const RNS::RequestReceipt& rr) {
  RNS::Bytes resp = rr.get_response();
  const uint8_t* d = resp.data(); size_t len = resp.size(); size_t off = 0;
  int n = Common::MsgPack::read_array_header(d, len, off);
  if (n <= 0) { advance(nullptr); return; }   // empty or unparseable -> next identity
  std::vector<RNS::Bytes> wants;
  for (int i = 0; i < n && wants.size() < MAX_WANTS; i++) {
    std::string tid = Common::MsgPack::read_bin_or_str(d, len, off);
    if (tid.empty()) break;
    RNS::Bytes b; b.append((const uint8_t*)tid.data(), tid.size());
    if (!seen_transient(b)) wants.push_back(b);
  }
  if (wants.empty()) { advance(nullptr); return; }   // nothing new for this identity
  std::vector<uint8_t> req; mp_arr(req, 3);
  mp_arr(req, wants.size()); for (auto& w : wants) mp_bin(req, w);   // wants
  mp_nil(req);                                                        // haves (none here)
  mp_uint(req, PER_TRANSFER_KB);                                      // per-transfer limit (KB)
  slink().request(get_path(), vec_to_bytes(req), on_get, on_failed, nullptr, 0);
  ctx().state = GETTING;
  ctx().deadline_ms = millis() + REQ_TIMEOUT_MS;
}

inline void on_get(const RNS::RequestReceipt& rr) {
  RNS::Bytes resp = rr.get_response();
  const uint8_t* d = resp.data(); size_t len = resp.size(); size_t off = 0;
  int n = Common::MsgPack::read_array_header(d, len, off);
  auto& reg = LXMFMinimal::instances();
  auto it = reg.find(ctx().cur_iden);
  LXMFMinimal* inst = (it != reg.end()) ? it->second : nullptr;
  haves().clear();
  if (n > 0 && inst) {
    for (int i = 0; i < n; i++) {
      std::string blob = Common::MsgPack::read_bin_or_str(d, len, off);
      if (blob.empty()) break;
      RNS::Bytes b; b.append((const uint8_t*)blob.data(), blob.size());
      RNS::Bytes tid = RNS::Identity::full_hash(b);
      if (inst->ingest_propagated(b)) ctx().run_received++;
      mark_transient(tid);
      haves().push_back(tid);
    }
  }
  // Tell the PN we have them so it drops them, unless retain is configured.
  if (!current().retain_on_node && !haves().empty()) {
    std::vector<uint8_t> req; mp_arr(req, 2); mp_nil(req);            // [nil, haves]
    mp_arr(req, haves().size()); for (auto& h : haves()) mp_bin(req, h);
    slink().request(get_path(), vec_to_bytes(req), nullptr, on_failed, nullptr, 0);
    ctx().state = ACK_FLUSH;
    ctx().deadline_ms = millis() + ACK_FLUSH_MS;   // let the purge packet flush before teardown
  } else {
    advance(nullptr);
  }
}

// Public entry: sync all local identities from the configured PN.
inline bool start() {
  const Config& c = current();
  if (!c.enabled || c.pn_hash.empty()) return false;
  if (is_busy()) return false;
  teardown();   // clean slate (called from the web task / cadence, not a callback)
  Ctx& x = ctx();
  x.pn_hash = RNS::Bytes();
  x.pn_hash.assignHex(c.pn_hash.c_str());
  if (x.pn_hash.size() != 16) return false;
  queue().clear();
  for (auto& kv : LXMFMinimal::instances()) queue().push_back(kv.first);
  if (queue().empty()) return false;
  x.run_received = 0;
  x.reached = false;
  x.last_error = "";
  begin_next();
  return true;
}

// Drive timeouts, the path wait, the ack flush, the per-identity advance, and
// the auto-sync cadence. ALL link teardown happens here, never in a callback.
inline void pump() {
  Ctx& x = ctx();
  uint32_t now = millis();
  const Config& c = current();
  if (!is_busy()) {
    if (c.enabled && !c.pn_hash.empty() && c.sync_interval_s > 0) {
      if (x.next_auto_ms == 0) x.next_auto_ms = now + c.sync_interval_s * 1000;
      else if ((int32_t)(now - x.next_auto_ms) >= 0) { x.next_auto_ms = now + c.sync_interval_s * 1000; start(); }
    } else {
      x.next_auto_ms = 0;
    }
    return;
  }
  switch (x.state) {
    case ADVANCE:
      teardown();
      begin_next();
      break;
    case PATH_WAIT:
      if (RNS::Transport::has_path(x.pn_hash)) open_link();
      else if ((int32_t)(now - x.deadline_ms) >= 0) finish_run(false);  // no path -> abort run
      break;
    case SETTLE:
      if ((int32_t)(now - x.deadline_ms) >= 0) send_list();
      break;
    case ACK_FLUSH:
      if ((int32_t)(now - x.deadline_ms) >= 0) advance(nullptr);
      break;
    case LINK_WAIT:
      if ((int32_t)(now - x.deadline_ms) >= 0) advance("link not established");
      break;
    case LISTING:
      if ((int32_t)(now - x.deadline_ms) >= 0) advance("no list response");
      break;
    case GETTING:
      if ((int32_t)(now - x.deadline_ms) >= 0) advance("no get response");
      break;
    default:
      break;
  }
}

inline const char* state_name() {
  switch (ctx().state) {
    case IDLE:      return "idle";
    case PATH_WAIT:
    case LINK_WAIT: return "connecting";
    case SETTLE:
    case LISTING:
    case GETTING:
    case ACK_FLUSH:
    case ADVANCE:   return "syncing";
    case COMPLETE:  return "complete";
    case FAILED:    return "failed";
  }
  return "idle";
}

}  // namespace Sync

}  // namespace Propagation
}  // namespace LXMF
