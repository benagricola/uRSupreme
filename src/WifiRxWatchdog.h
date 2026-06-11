// WiFi RX watchdog: detects the driver-level inbound-RX wedge and
// recovers it with a reconnect.
//
// Failure mode this guards (reproduced + instrumented on the bench, see
// commit message): under bursts of concurrent inbound TCP (two uploads,
// or upload + backbone sync), the WiFi driver's dynamic RX buffer pool
// hits its cap and the driver stops delivering ALL inbound data frames
// permanently. TX, association and management frames stay healthy, so
// the device looks "connected" while every inbound packet (TCP, UDP,
// ICMP) is dropped. Nothing above the driver ever sees an error, and
// the state never clears on its own; a reassociation resets the driver
// and traffic resumes immediately.
//
// Detection: a raw-lwIP ICMP echo to the gateway every PROBE_PERIOD_MS
// while the STA is associated. The watchdog only ever acts after the
// gateway has answered at least one probe this boot, so a network whose
// gateway never answers pings never triggers it. Once that baseline
// exists, a reply gap longer than SILENT_AFTER_MS triggers a
// WiFi.reconnect(). Consecutive firings with no reply in between back
// off exponentially (30 s doubling to a 10 min cap) so a gateway that
// legitimately stops answering costs at most one brief reassociation
// per backoff period, while a re-wedge that lands before the first
// post-reconnect reply still gets retried instead of stranding the
// device (a reply resets the backoff to its base).
//
// Cost bounds: one raw pcb, one 16-byte probe every PROBE_PERIOD_MS,
// fixed counters. All lwIP calls run on the tcpip thread via
// tcpip_try_callback; the loop-side tick is non-blocking.
#pragma once

#include <WiFi.h>
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/ip4.h"
#include "lwip/raw.h"
#include "lwip/tcpip.h"

namespace WifiRxWatchdog {

static const uint32_t PROBE_PERIOD_MS  = 10000;
static const uint32_t SILENT_AFTER_MS  = 35000;   // 3+ missed probes
static const uint32_t BACKOFF_BASE_MS  = 30000;   // doubles per dry firing
static const uint32_t BACKOFF_CAP_MS   = 600000;
static const uint16_t PROBE_ICMP_ID    = 0x5258;  // "RX"

// Counters (surfaced on /api/diag/mem).
inline volatile uint32_t& probes_sent()    { static volatile uint32_t v = 0; return v; }
inline volatile uint32_t& replies_seen()   { static volatile uint32_t v = 0; return v; }
inline volatile uint32_t& reconnects()     { static volatile uint32_t v = 0; return v; }
inline volatile uint32_t& last_reply_ms()  { static volatile uint32_t v = 0; return v; }

namespace _detail {

inline struct raw_pcb*& pcb() { static struct raw_pcb* v = nullptr; return v; }

// Runs on the tcpip thread. NB: the firmware has a global byte array
// named `pbuf` (KISS framing buffer), so the lwIP type is spelled
// `struct pbuf` throughout.
inline u8_t on_icmp_recv(void*, struct raw_pcb*, struct pbuf* p,
                         const ip_addr_t*) {
  if (p->tot_len >= sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr)) {
    const struct ip_hdr* iph = (const struct ip_hdr*)p->payload;
    const u16_t hlen = (u16_t)(IPH_HL(iph) * 4);
    if (p->tot_len >= hlen + sizeof(struct icmp_echo_hdr)) {
      const struct icmp_echo_hdr* ic =
        (const struct icmp_echo_hdr*)((const u8_t*)p->payload + hlen);
      if (ic->type == ICMP_ER && ic->id == lwip_htons(PROBE_ICMP_ID)) {
        last_reply_ms() = millis();
        replies_seen() = replies_seen() + 1;
        pbuf_free(p);
        return 1;   // consumed
      }
    }
  }
  return 0;   // not ours - let the stack process it
}

// Runs on the tcpip thread: lazily creates the pcb, sends one echo
// request to the gateway address packed into arg.
inline void probe_on_tcpip(void* arg) {
  if (!pcb()) {
    pcb() = raw_new(IP_PROTO_ICMP);
    if (pcb()) {
      raw_recv(pcb(), on_icmp_recv, nullptr);
      raw_bind(pcb(), IP_ANY_TYPE);
    }
  }
  const uint32_t gw_u32 = (uint32_t)(uintptr_t)arg;
  if (!pcb() || gw_u32 == 0) return;

  const u16_t len = sizeof(struct icmp_echo_hdr) + 8;
  struct pbuf* p = pbuf_alloc(PBUF_IP, len, PBUF_RAM);
  if (!p) return;
  struct icmp_echo_hdr* ic = (struct icmp_echo_hdr*)p->payload;
  static u16_t seq = 0;
  memset(p->payload, 0, len);
  ICMPH_TYPE_SET(ic, ICMP_ECHO);
  ICMPH_CODE_SET(ic, 0);
  ic->id    = lwip_htons(PROBE_ICMP_ID);
  ic->seqno = lwip_htons(++seq);
  ic->chksum = 0;
  ic->chksum = inet_chksum(ic, len);

  ip_addr_t dst;
  ip_addr_set_ip4_u32_val(dst, gw_u32);
  if (raw_sendto(pcb(), p, &dst) == ERR_OK) {
    probes_sent() = probes_sent() + 1;
  }
  pbuf_free(p);
}

}  // namespace _detail

// Call once per main-loop pass. Non-blocking.
inline void tick() {
  static uint32_t s_last_probe_ms = 0;
  static uint32_t s_last_reconnect_ms = 0;
  const uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED) return;

  if (now - s_last_probe_ms >= PROBE_PERIOD_MS) {
    s_last_probe_ms = now;
    const uint32_t gw = (uint32_t)WiFi.gatewayIP();
    if (gw != 0) {
      // Fire-and-forget; on a full tcpip mbox just skip this period.
      tcpip_try_callback(_detail::probe_on_tcpip, (void*)(uintptr_t)gw);
    }
  }

  static uint32_t s_backoff_ms = BACKOFF_BASE_MS;
  static uint32_t s_replies_at_last_fire = 0;
  if (replies_seen() > 0
      && (now - last_reply_ms()) > SILENT_AFTER_MS
      && (s_last_reconnect_ms == 0
          || (now - s_last_reconnect_ms) > s_backoff_ms)) {
    // A reply since the last firing proves the last reconnect worked:
    // reset the backoff. A dry firing (no reply in between) doubles it.
    if (s_last_reconnect_ms != 0) {
      if (replies_seen() != s_replies_at_last_fire) {
        s_backoff_ms = BACKOFF_BASE_MS;
      } else if (s_backoff_ms < BACKOFF_CAP_MS) {
        s_backoff_ms = (s_backoff_ms * 2 < BACKOFF_CAP_MS)
                         ? s_backoff_ms * 2 : BACKOFF_CAP_MS;
      }
    }
    WARNINGF("WifiRxWatchdog: no gateway reply for %lu ms with STA "
             "associated - reconnecting to clear a wedged RX path "
             "(next retry in %lu s if still silent)",
             (unsigned long)(now - last_reply_ms()),
             (unsigned long)(s_backoff_ms / 1000));
    s_replies_at_last_fire = replies_seen();
    s_last_reconnect_ms = now;
    reconnects() = reconnects() + 1;
    WiFi.reconnect();
  }
}

}  // namespace WifiRxWatchdog
