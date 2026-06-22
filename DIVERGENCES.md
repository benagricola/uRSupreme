# Upstream divergence ledger

This file lists every place this firmware (or its pinned microReticulum
fork) deliberately behaves differently from upstream Reticulum / LXMF /
RNode. Rules (see CLAUDE.md §1):

- Any commit that introduces or removes a divergence updates this file
  in the same commit.
- Every entry cites the code and the upstream counterpart, and carries
  the reason. Entries were last verified against the uR pin in
  `platformio.ini` (`[libdeps]`) on 2026-06-10; re-verify against the
  current pin before relying on one.

## Active divergences

### 1. LoRa interface defaults to MODE_ACCESS_POINT
- Where: `src/RNode_Firmware.ino` (LoRa `apply_mode_ifac` default).
- Upstream: every interface defaults to `MODE_FULL`.
- Why: deliberate (#81). The bridge node must answer path discovery for
  its LoRa edge; AP mode is in `DISCOVER_PATHS_FOR` and also suppresses
  per-announce egress on the duty-cycled link. Low request volume on
  the LoRa side keeps the discovery cost acceptable, unlike the TCP
  `MODE_GATEWAY` default this fork removed in `6aef568`.

### 2. Interface-mode encodings are bit-flags
- Where: uR `src/Type.h` (`FULL=0x01 ... GATEWAY=0x40`).
- Upstream: sequential values `0x01..0x06`.
- Why: lets `DISCOVER_PATHS_FOR` work as a bitmask. Mode is local-only
  and never serialized onto the wire. Do NOT "fix" these to upstream's
  values; the bitmask tests depend on the flag encoding.

### 3. Path-request dedup window bounded at 4096 tags
- Where: uR `src/Transport.cpp` (`RNS_PR_TAGS_MAX 4096`).
- Upstream: `max_pr_tags = 32000`.
- Why: RAM bound. Eviction is FIFO-by-recency matching upstream
  behaviour (the earlier random `std::set` eviction is fixed); only the
  window size differs.
- Verify: `git -C ../microReticulum show <pin>:src/Transport.cpp | grep -n RNS_PR_TAGS_MAX`

### 4. Resource proof recovery re-waits instead of re-querying the packet cache
- Where: uR `src/Resource.cpp` sender watchdog (`AWAITING_PROOF` branch).
- Upstream: `Resource.py:635-654` lowers the timeout via
  `PROOF_TIMEOUT_FACTOR` and issues a packet-cache request per retry.
- Here: same tight `PROOF_TIMEOUT_FACTOR` window and retry budget, but
  no cache re-query; on exhaustion the transfer fails fast and the LXMF
  layer re-sends the whole message, which the receiver deduplicates by
  transient id. Rationale (in-code): a cache re-query is a no-op on a
  direct 2-node link.
- Consequence to keep in mind: on a multi-hop Resource transfer a lost
  proof costs a full message re-send rather than a one-packet recovery.

### 5. Inbound LXMF dedup window is a 128-entry RAM FIFO
- Where: firmware `src/LXMF/LXMFMinimal.h` (`cd982ff`).
- Upstream: persisted transient-id store with a 180-day horizon.
- Why: flash-wear and RAM bounds. A reboot can therefore readmit one
  duplicate of a message whose retry straddles the reboot.

### 6. Table sizes and cull cadence are MCU-bounded
- Where: uR transport tables (announce rates, path requests, path
  states), `tables_cull_interval` 60 s vs upstream 5 s.
- Why: RAM bounds; culls are batched to keep main-loop cost flat.

### 7. LXMF scope gaps (not yet ported)
- No propagation-node sync (store-and-forward) and no tickets.
- Delivery stamps are in flight on `feature/upstream-divergences`.
- Consequence: a stamp-enforcing peer drops our messages only where
  stamps are required AND that branch is not yet merged; propagation
  delivery silently unavailable.

### 8. Announce app_data stays uncompressed
- Where: firmware announce serialization (3-element app_data list).
- Why: uR's Resource path cannot bz2-decompress; emitting the
  uncompressed form keeps every upstream client able to parse our
  announces. Wire-compatible by design.

### 9. In-flight outbound send maps are count-capped
- Where: firmware `src/LXMF/LXMFMinimal.h` (`send_prepared` capacity
  guard, `MAX_INFLIGHT_SENDS` = 64; the two `pending_*_sends` maps).
- Upstream: `LXMRouter.pending_outbound` is an unbounded list
  (`LXMF/LXMRouter.py:99`, appended at 1690/2496, pruned only on
  delivery/failure).
- Why: RAM bound. The maps hold one entry per send awaiting a delivery
  proof or link establishment; their normal exits are the completion
  callbacks, with a 30-min orphan sweep as the only other backstop. A
  burst to a reachable-but-slow peer (or a stuck-entry leak) could grow
  them unbounded between sweeps. At/above the cap, send_prepared rejects
  the new send with a user-facing message rather than evicting a live
  entry (which would silently drop a real message). No-route sends are
  unaffected; they wait in the gateway's separately-bounded auto-send
  queue. Map nodes are PSRAM-backed (PsAlloc), not a wire change.

### 10. Off-loop receive over fast links (byte-bounded window, off-loop I/O)
- Where: uR `src/Resource.cpp` / `src/ResourceBuffer.*` / `src/Type.h` /
  `src/ResourceData.h`; the firmware registers the hooks in
  `src/RnsConclude.h` (worker) and `src/RNode_Firmware.ino`. Detailed in
  the uR fork's own `DIVERGENCES.md` (entries 1-3).
- Upstream: RNS sizes the Resource request window by part count
  (`Resource.py`, ramps to 75) and writes parts + concludes (read-back,
  decrypt, verify, app callback) inline on the receive path.
- Why: on this single-threaded receiver a part-count window overruns a
  fast large-SDU link, and the inline SD writes + whole-transfer
  read+decrypt freeze the main loop (starving the radio). uR exposes a
  byte cap (`RECV_MAX_INFLIGHT_BYTES`) and two firmware-registered hooks
  (part-write, conclude); the firmware does the SD I/O off-loop on a
  core-0 worker. With no hook registered uR is upstream-identical, so the
  divergence is opt-in and owned here.
- Also here: a non-blocking `Storage::SDCard::BusGuard{TryLock}` for the
  on-loop IMU poll (`src/Sensors/Motion/QMI8658.h`), so a best-effort
  loop-task SD-bus user yields to the off-loop writer instead of blocking
  the loop. Not an upstream divergence (firmware-only mechanism).

### 11. Idle OLED blanking defaults on (Supreme)
- Where: `src/Display.h` (`display_init` Supreme default, `DIVERGES:`; and
  the `update_display` blank gate), `src/Display/ScreenFramework.h`
  (`active_page`).
- Upstream: RNode firmware defaults `display_blanking_enabled = false` and
  enables blanking only when the RNode app writes a non-zero display
  timeout. Upstream has no screen framework, so its blank gate is a pure
  input-inactivity timer.
- Why: the Supreme is a battery messenger, not a headless modem. A
  permanently lit OLED is a continuous load (controller + charge pump),
  so the idle home screen blanks after `DISPLAY_HOME_BLANK_DEFAULT_MS` (60 s)
  and the panel is slept (`SH110X_DISPLAYOFF`). Blanking applies on battery
  only: on external power the screen stays lit. Two parts:
  (1) Default ON, keyed on the timeout byte `ADDR_CONF_DBLK` itself
  (0 factory / 0xFF unwritten = unset, use the Supreme default; a non-zero
  RNode-app timeout still wins) rather than on the `ADDR_CONF_BSET` marker,
  so it holds however the EEPROM was initialised.
  (2) Inactivity-based gate. The fork's screen framework keeps the home
  page permanently active AND it reports `is_live` (radio waterfall), so
  the prior `!Screens::active()` gate suppressed blanking forever. The
  gate now keys on input silence (matching upstream's model): the home
  page uses the idle timeout, pages the user navigated to hold for
  `DISPLAY_LIVE_BLANK_TIMEOUT_MS` (5 min), and the identity code is never
  blanked.
- Consequence: a Supreme user can no longer disable blanking via the RNode
  app's 0 timeout (it now means "use the default"); a long timeout
  approximates always-on. Acceptable for a battery device.
- Scope: the default-on is Supreme-only (`BOARD_TBEAM_S_V1` /
  `BOARD_TBEAM_S_LR_V1`). The inactivity gate is shared, but degrades to
  the upstream inactivity timer on boards that register no screens.

## Recently retired (do not reintroduce)

Removed by `6aef568` + the June 2026 uR re-pins, all matching upstream
now: TCP/UDP interface defaults (`MODE_FULL`), `PATH_REQUEST_MI` 20 s,
path TTLs (1 week / 1 day / 6 h), LXMF size cutoff compared against
content size (295/319), LXMF retry cadence (5 attempts / 10 s),
ingress-control constants and burst-deactivate `ic_held_release`,
Resource `MAX_RETRIES` 16, and the adaptive Resource sliding window
(4 → 10 → 75). The history of why each was wrong is in the 2026-06-10
fork-history audit report.
