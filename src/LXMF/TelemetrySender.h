// Periodic LXMF telemetry to one or more Sideband collectors.
//
// At each due tick, packs the device's sensor readings once in
// Sideband's Telemeter format (Telemetry/Telemeter.h) and sends the
// same sample to every configured collector as an LXMF message with
// empty title/content and the blob in FIELD_TELEMETRY -
// the exact message shape Sideband builds for its own collector
// updates (sbapp/sideband/core.py:1515 @ 863b925: LXMessage(dest,
// source, "", fields={FIELD_TELEMETRY: packed})).
//
// Telemetry sends do not enter the outbox or the conversation view:
// Sideband's sending side keeps collector updates out of the
// conversation too, and a message every interval would crowd the
// bounded outbox. Delivery state is tracked here instead, keyed by
// the returned packet hash, and surfaced on /api/telemetry/config.
//
// Config persists to /lxmf/telemetry.json. Disabled by default -
// location telemetry is opt-in by design.

#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <stdint.h>
#include <string>

#include <microStore/FileSystem.h>
#include "../Common/PsramAllocator.h"
#include "../Common/MsgPack.h"
#include "../Telemetry/Telemeter.h"
#include "../Sensors/Position/Gnss.h"
#include "LXMFTypes.h"
#include "LXMFGateway.h"

extern microStore::FileSystem filesystem;

namespace LXMF {
namespace TelemetrySender {

inline constexpr const char* CONFIG_PATH        = "/lxmf/telemetry.json";
inline constexpr uint32_t    MIN_INTERVAL_S     = 60;
inline constexpr uint32_t    MAX_INTERVAL_S     = 7 * 24 * 3600;
inline constexpr uint32_t    DEFAULT_INTERVAL_S = 15 * 60;
// Diag mode sends a tiny single packet, so it may beacon faster than a
// full telemetry update for usable field-debug resolution.
inline constexpr uint32_t    DIAG_MIN_INTERVAL_S = 10;

struct Config {
  bool        enabled = false;
  IdentityId  identity;          // sending identity id; empty = first active
  std::vector<std::string> collectors;  // 16-byte LXMF dest hashes, hex (32 chars)
  uint32_t    interval_s = DEFAULT_INTERVAL_S;
  Telemetry::Telemeter::Include include;  // same items to every collector
  // Field-debug mode: send a human-readable GNSS diagnostics line as
  // the message content instead of a Sideband telemetry blob, so a
  // plain LXMF node logs it directly while the device is out of WiFi
  // range. A visible message (it lands in the conversation), unlike
  // the silent telemetry path.
  bool        diag = false;
};

namespace _detail {
  inline Config& config_ref()          { static Config c; return c; }
  inline uint32_t& last_attempt_ms()   { static uint32_t v = 0; return v; }
  inline double&  last_sent_epoch()    { static double v = 0.0; return v; }
  inline std::string& last_result()    { static std::string s = "never"; return s; }
  inline std::string& last_error()     { static std::string s; return s; }
  // One packet hash per collector reached in the last batch; delivery
  // receipts are matched against any of them.
  inline std::vector<RNS::Bytes>& last_packets() {
    static std::vector<RNS::Bytes> v; return v;
  }
}

inline Config&       config()      { return _detail::config_ref(); }
inline const std::string& last_result() { return _detail::last_result(); }
inline const std::string& last_error()  { return _detail::last_error(); }
inline double        last_sent_epoch()  { return _detail::last_sent_epoch(); }

inline void load(microStore::FileSystem& fs) {
  if (!fs.exists(CONFIG_PATH)) return;
  std::vector<uint8_t> data;
  if (fs.readFile(CONFIG_PATH, data) == 0) return;
  Common::PsramJsonDocument doc;
  if (deserializeJson(doc, data.data(), data.size()) != DeserializationError::Ok) return;
  Config& c = config();
  c.enabled       = doc["enabled"] | false;
  c.identity      = (const char*)(doc["identity"] | "");
  c.collectors.clear();
  if (doc["collectors"].is<JsonArray>()) {
    for (JsonVariant v : doc["collectors"].as<JsonArray>()) {
      const char* s = v.as<const char*>();
      if (s && *s) c.collectors.emplace_back(s);
    }
  } else {
    // Migrate the old single-collector field.
    const char* legacy = doc["collector"] | "";
    if (legacy && *legacy) c.collectors.emplace_back(legacy);
  }
  c.interval_s    = doc["interval_s"] | DEFAULT_INTERVAL_S;
  if (c.interval_s < MIN_INTERVAL_S) c.interval_s = MIN_INTERVAL_S;
  if (c.interval_s > MAX_INTERVAL_S) c.interval_s = MAX_INTERVAL_S;
  c.include.battery     = doc["battery"]     | true;
  c.include.location    = doc["location"]    | true;
  c.include.environment = doc["environment"] | true;
  c.include.magnetic    = doc["compass"]     | false;
  c.diag                = doc["diag"]        | false;
}

inline void persist(microStore::FileSystem& fs) {
  const Config& c = config();
  Common::PsramJsonDocument doc;
  doc["enabled"]     = c.enabled;
  doc["identity"]    = c.identity;
  JsonArray cols = doc["collectors"].to<JsonArray>();
  for (const std::string& h : c.collectors) cols.add(h);
  doc["interval_s"]  = c.interval_s;
  doc["battery"]     = c.include.battery;
  doc["location"]    = c.include.location;
  doc["environment"] = c.include.environment;
  doc["compass"]     = c.include.magnetic;
  doc["diag"]        = c.diag;
  String out;
  serializeJson(doc, out);
  fs.writeFile(CONFIG_PATH,
               reinterpret_cast<const uint8_t*>(out.c_str()),
               out.length());
}

// Resolve the sending identity: the configured one, or the first
// active identity when unset. Returns empty id if none exists.
inline IdentityId resolve_identity() {
  const Config& c = config();
  if (!c.identity.empty() && LXMFGateway::identity_by_id(c.identity)) {
    return c.identity;
  }
  const auto& actives = LXMFGateway::active_identities();
  if (!actives.empty()) return actives.front()->id;
  return IdentityId{};
}

// Field-debug payload: the GNSS diagnostics snapshot as a compact
// msgpack map under "urtn_gps" in FIELD_CUSTOM_META. Carried as a
// meta field (content stays empty) so the send stays out of the
// outbox/conversation, and small (short keys) so it stays a single
// opportunistic packet over LoRa. A uR receiver swallows it as
// machinery (no urtn_msg marker); a plain LXMF node decodes the map.
// Keys: pm power-mode, ps pulse-state, m1 m10-power (enums); ef
// ever-fixed; bo backoff; fx fix-valid; sv/su sats visible/used; sn
// snr dB; ag agc%, ar agc-raw, nz noise/ms, jm jamming, cw cw-jam;
// ak/nk valset acks/naks.
inline RNS::Bytes pack_gps_diag_meta() {
  using namespace Common::MsgPack;
  const Sensors::Gnss::DiagSnapshot d = Sensors::Gnss::diag_snapshot();
  uint8_t buf[128];
  size_t pos = 0, n = 0;
  auto put_int = [&](const char* key, int64_t v) -> bool {
    size_t m = pack_str(&buf[pos], sizeof(buf) - pos, key);
    if (m == 0) return false;
    pos += m;
    m = pack_int(&buf[pos], sizeof(buf) - pos, v);
    if (m == 0) return false;
    pos += m;
    return true;
  };
  n = pack_map_header(buf, sizeof(buf), 1);            // { urtn_gps: {...} }
  if (n == 0) return RNS::Bytes{};
  pos += n;
  n = pack_str(&buf[pos], sizeof(buf) - pos, "urtn_gps");
  if (n == 0) return RNS::Bytes{};
  pos += n;
  n = pack_map_header(&buf[pos], sizeof(buf) - pos, 15);
  if (n == 0) return RNS::Bytes{};
  pos += n;
  if (!put_int("pm", d.mode)          || !put_int("ps", d.pulse_state) ||
      !put_int("m1", d.m10_power)     || !put_int("ef", d.ever_fixed ? 1 : 0) ||
      !put_int("bo", d.backoff_count) || !put_int("fx", d.fix_valid ? 1 : 0) ||
      !put_int("sv", d.sats_visible)  || !put_int("su", d.sats_used) ||
      !put_int("sn", d.snr_db)        || !put_int("ag", d.agc_pct) ||
      !put_int("ar", d.agc_raw)       || !put_int("nz", d.noise) ||
      !put_int("jm", d.jamming_state) || !put_int("ak", (int64_t)d.valset_acks) ||
      !put_int("nk", (int64_t)d.valset_naks)) {
    return RNS::Bytes{};
  }
  return RNS::Bytes(buf, pos);
}

// Build and send one telemetry update now. Returns true when the
// message was handed to the gateway (sent or queued for a route);
// false otherwise, with the reason in last_error().
inline bool send_now() {
  auto fail = [&](const char* result, const char* err) {
    _detail::last_result() = result;
    _detail::last_error()  = err ? err : "";
    return false;
  };
  _detail::last_attempt_ms() = millis();

  const Config& c = config();
  if (c.collectors.empty()) {
    return fail("config_error", "Add at least one collector address.");
  }
  // Validate every collector before sending so a bad entry fails fast
  // rather than half-sending the batch.
  std::vector<RNS::Bytes> dests;
  dests.reserve(c.collectors.size());
  for (const std::string& hex : c.collectors) {
    if (hex.size() != 32) {
      return fail("config_error", "Collector address must be 32 hex characters.");
    }
    RNS::Bytes dest;
    dest.assignHex(hex.c_str());
    if (dest.size() != 16) {
      return fail("config_error", "Collector address is not valid hex.");
    }
    dests.push_back(dest);
  }

  const IdentityId iden = resolve_identity();
  if (iden.empty()) {
    return fail("no_identity", "No active identity to send from.");
  }
  const LXMFIdentity* a = LXMFGateway::identity_by_id(iden);
  if (!a) return fail("no_identity", "No active identity to send from.");

  const double now_epoch = a->lxmf.get_timestamp();

  // Two meta-field shapes, both with empty content so the send stays
  // out of the outbox/conversation (LXMFGateway machinery path). Diag:
  // the GNSS diagnostics map in FIELD_CUSTOM_META. Normal: the Sideband
  // telemetry blob in FIELD_TELEMETRY.
  ExtraFields        extra;
  const ExtraFields* extra_ptr = nullptr;
  if (c.diag) {
    extra.custom_meta = pack_gps_diag_meta();
    if (extra.custom_meta.size() == 0) {
      return fail("no_readings", "GPS diagnostics unavailable right now.");
    }
    extra_ptr = &extra;
  } else {
    uint8_t packed[Telemetry::Telemeter::MAX_PACKED];
    const size_t packed_len = Telemetry::Telemeter::pack(
        packed, sizeof(packed), c.include, now_epoch);
    if (packed_len == 0) {
      return fail("no_readings", "No sensor readings available right now.");
    }
    extra.telemetry = RNS::Bytes(packed, packed_len);
    extra_ptr = &extra;
  }

  // Send the same sample to every collector. Track each outcome so the
  // batch result reflects the worst case the user needs to act on.
  _detail::last_packets().clear();
  size_t n_sent = 0, n_queued = 0, n_failed = 0;
  const char* last_err = nullptr;
  for (const RNS::Bytes& dest : dests) {
    MessageRecord rec;
    const char* err = nullptr;
    bool queued = false;
    const bool ok = LXMFGateway::send(iden, dest, "", "", nullptr, rec, &err,
                                      &queued, /*use_seq=*/0, extra_ptr);
    if (ok) {
      n_sent++;
      _detail::last_packets().push_back(rec.packet_hash);
    } else if (queued) {
      n_queued++;
    } else {
      n_failed++;
      last_err = err;
    }
  }

  if (n_sent == 0 && n_queued == 0) {
    return fail("failed", last_err ? last_err : "Send failed.");
  }
  _detail::last_sent_epoch() = now_epoch;
  _detail::last_error()      = "";
  if (n_failed > 0) {
    _detail::last_result() = "partial";
  } else if (n_sent == 0) {
    // All collectors are awaiting a route; the samples go out once a
    // path arrives for each.
    _detail::last_result() = "finding_route";
  } else {
    _detail::last_result() = "sent";
  }
  return true;
}

// Delivery-state hook, called from the gateway's outbox status
// callback. Telemetry records are not in the outbox, so this is the
// only consumer of their receipts.
inline void on_outbox_status(const RNS::Bytes& hash, OutboxStatus status) {
  auto& packets = _detail::last_packets();
  bool match = false;
  for (const RNS::Bytes& p : packets) {
    if (hash == p) { match = true; break; }
  }
  if (!match) return;
  switch (status) {
    case OutboxStatus::Delivered: _detail::last_result() = "delivered"; break;
    case OutboxStatus::Failed:    _detail::last_result() = "failed";    break;
    case OutboxStatus::Sent:      _detail::last_result() = "sent";      break;
    default: break;
  }
}

// Main-loop tick: send when enabled and the interval has elapsed.
// First send happens one full interval after boot/enable, not
// immediately, so a crash-looping device cannot spam the collector.
inline void tick() {
  const Config& c = config();
  if (!c.enabled) return;
  const uint32_t now = millis();
  if (_detail::last_attempt_ms() != 0 &&
      (now - _detail::last_attempt_ms()) < c.interval_s * 1000UL) {
    return;
  }
  if (_detail::last_attempt_ms() == 0) {
    // Arm the first interval instead of sending at boot - sensors
    // (GPS especially) have nothing useful yet.
    _detail::last_attempt_ms() = now;
    return;
  }
  send_now();
}

// Status block for the web handler.
inline void fill_status(JsonObject o) {
  const Config& c = config();
  o["enabled"]     = c.enabled;
  o["identity"]    = c.identity;
  JsonArray cols = o["collectors"].to<JsonArray>();
  for (const std::string& h : c.collectors) cols.add(h);
  o["interval_s"]  = c.interval_s;
  o["battery"]     = c.include.battery;
  o["location"]    = c.include.location;
  o["environment"] = c.include.environment;
  o["compass"]     = c.include.magnetic;
  o["diag"]        = c.diag;
  o["last_result"] = _detail::last_result();
  if (!_detail::last_error().empty()) o["last_error"] = _detail::last_error();
  if (_detail::last_sent_epoch() > 0.0) {
    o["last_sent_epoch"] = (uint64_t)_detail::last_sent_epoch();
  }
}

}  // namespace TelemetrySender
}  // namespace LXMF
