# Pending Tasks

_Exported 2026-05-20 — 6 tasks (power-management and WiFi watchdog items omitted)_

---

## #71 — Migrate SPA to Alpine.js

**Status:** pending

Final SPA task. After all other features land, retrofit existing vanilla-JS render functions (`renderConversations`, `renderThread`, `renderAnnounces`, `renderPaths`, modals) to Alpine.js. Retire `withPreservedState`, the manual `el()` factory, and the wipe-and-rebuild pattern. Ship Alpine inline in the SPA blob; ~15 KB gzipped cost. See `[[spa-alpine-js]]` memory for rationale.

---

## #99 — Network topology view: graph of seen nodes

**Status:** pending

New tab in the SPA that shows a visual graph of nodes we can see (us in the middle, heard peers around). Use SVG, force-directed or circular layout. Data: `/api/announces` + `/api/paths`. Show edges with RSSI/SNR if known. Click a node to open a conversation with it. Useful for at-a-glance "who's on the mesh right now". Sizes ~1–2 days of SPA work.

---

## #162 — Browser desktop notifications for incoming messages

**Status:** pending

Add Web Notifications API support to the SPA so users get a desktop / mobile-OS notification when an LXMF message arrives while the tab is backgrounded or the device is locked.

**Scope:**

- Request `Notification.permission` on first WS `incoming` event after the user has interacted with the SPA (not on load — browsers throttle unsolicited prompts).
- Fire a Notification with sender display name + message preview when an incoming message arrives AND `document.visibilityState !== 'visible'` (foregrounded chats already update inline; firing a system toast in that case would just be noise).
- Tap on the notification refocuses the SPA tab and opens the conversation for that peer.
- Provide a per-user preference toggle in Settings → Identity (alongside "Persist outbound attachments" etc) so users can disable the noise.
- Persist the preference in localStorage (matches `state.prefs.autoDownloadAttachments` pattern).
- Coalesce: if multiple messages from the same peer arrive within a short window, replace the previous notification rather than stack up N system toasts (use the `tag` field on Notification).
- PWA-installed mode: the same notification path should work when launched as an installed app on Android/iOS; iOS Safari needs the SPA installed to home-screen for notifications to fire at all, so document this in the toggle's description.

**Out of scope:** server-side push (Web Push API) for fully-backgrounded "tab closed" delivery — that requires a push service the device can't talk to without internet.

---

## #163 — rmap.world v4 interface-discovery announce

**Status:** pending

Make the device auto-discoverable by https://rmap.world v4 by emitting an RNS `rnstransport.discovery.interface` aspect announce. **NOT MQTT** — it's a standard Reticulum announce that the upstream Python `rns` ≥1.1 library produces when `discoverable = yes` is set on an interface; the C++ microReticulum port doesn't implement this at all (`grep` for `discoverable` returns zero hits).

**Plan:**

1. Read upstream Python RNS source to determine the exact wire format of the discovery announce `app_data` — file paths likely `RNS/Interfaces/Interface.py` and the announce-builder in `RNS/Transport.py`. Clone the upstream Reticulum repo locally as a sibling sister to our existing `microReticulum/` clone if it isn't already.
2. Port the announce emitter into microReticulum on the `rnstransport.discovery.interface` aspect. Reuse our existing announce path; the new aspect just needs a fresh destination + a periodic timer (default 6h per the upstream default).
3. Plumb the fields we already have:
   - **latitude/longitude/height** — from `Web::Gps::last_fix()` (we already collect this); height is unused for now (could come from GPS altitude later).
   - **radio params** — `lora_freq`, `lora_bw`, `lora_sf`, `lora_cr`, `lora_txp` are already in scope.
   - **discovery_name** — default to the existing RNode-style name (`RNode <xxxx>` derived from MAC; same pattern as `bt_devname`). User-overridable via the backend.
   - **announce_interval** — user-configurable in the backend, default 360 min.
   - **reachable_on** — leave nil for now; we're not a TCP server.
4. SPA: add a "RMAP discovery" section to the Connectivity popover (the one anchored to the antenna icon). Surface the `discovery_name` (editable), the announce interval, the lat/lon being broadcast, and an Enable toggle. Default OFF — privacy-by-design per rmap's docs ("Only interfaces where you explicitly set `discoverable = yes` will appear on RMAP v4. Your node's existence on the network is never revealed without your consent.").
5. Add a `[[RMAP World]]` TCPClientInterface equivalent so the announce reaches `rmap.world:4242` directly. Our devices have WiFi, so we don't need a separate transport-node bridge.
6. Persist the config to `/lxmf/rmap.json` (or similar) so the toggle survives reboots.

**Out of scope for v1:** dynamic announce-interval adjustment based on whether we have a fix yet; height from GPS altitude; encrypted-discovery / `publish_ifac`. Default to the rmap-public-anonymous path.

**Source-of-truth references:**

- https://rmap.world/info.html — describes the discovery system from the consumer side.
- https://reticulum.network/manual/interfaces.html#discoverable-interfaces — lists config keys.
- The upstream RNS Python source (to be cloned locally) — authoritative for the announce wire format.

---

## #168 — In-firmware diagnostic log ring exposed over HTTP (replace dead serial)

**Status:** pending

USB CDC serial on T-Beam Supreme returns 0 bytes for both `cat` and pyserial reads — confirmed across multiple sessions. Blocks all serial-based debugging including the Resource spill crash (#167) and any future field issues.

**Approach:** ring buffer of last N log lines (e.g. 4 KiB) inside the `NOTICE` / `NOTICEF` / `INFO` / `INFOF` / `ERROR` / `ERRORF` / `WARNING` / `WARNINGF` macros via a sink hook on `RNS::log` / `logf`. Expose via:

- `GET /api/debug/log` → tail of ring as `text/plain` (bearer-auth gated)
- WS event `debug_log` streaming new lines to connected clients (optional)
- SPA "Diagnostics" tab to view live + dump

**Storage:** in-PSRAM (cheap, big enough for a session's worth of logs). Optional opt-in dump to SD on FATAL/restart for post-crash forensics.

**Trigger to build:** blocks #167 progress until logs are visible again. Independent of the serial USB CDC issue (which may be an esp-idf USB stack bug we can't fix locally).

---

## #175 — LXMFInbox::recent()/since() returns vector by value — copies the whole ring per HTTP fetch

**Status:** pending

`LXMFInbox::recent(size_t)` and `LXMFInbox::since(uint32_t)` both return `std::vector<MessageRecord>` **BY VALUE**. Each call to `GET /api/identities/{}/{inbox|outbox}` or the new `/body` endpoint's seq lookup copies the entire in-memory ring (up to 50–200 `MessageRecord`s, each with `std::string` title + attachments vector). At ~100 bytes per record, that's ~10–20 KiB of transient allocation on the default heap (internal SRAM) per HTTP request.

Audit-found during #170 (Pattern B — in-mem duplication via copying). Same anti-pattern: the canonical data lives in `_ring` (`std::deque<MessageRecord>`), but every accessor returns a fresh copy.

**Fix options:**

1. Return `const std::deque<MessageRecord>&` + a filter callback (caller iterates without copying).
2. Replace with an iterator-pair API (begin/end with filter predicates).
3. If the JSON serializer can be made streaming (chunked emit, one record at a time), even better — no temp vector at all.

The streaming `/body` endpoint's seq lookup is the worst offender right now since it calls `recent(static_cast<size_t>(-1))` — full ring copy just to find one record by seq. Fix: add a `find_by_seq(uint32_t)` returning `const MessageRecord*` direct from the deque.

---

## Investigate + rework message retention model (per-chat defaults, eviction)

**Status:** pending

The current retention scaffolding has two overlapping layers and an
unverified eviction path:

- **Global per-identity caps** in `LXMFInbox`: `ram_capacity` (max
  messages per identity, default 200) and `ttl_seconds` (wall-clock
  TTL across the whole identity, default off). Wired through
  `/api/inbox_config` and the Settings → App → "Inbox retention"
  card.
- **Per-peer TTL override** in `_peer_ttl`: the gear-modal next to
  each conversation (`modal-conv-retention`) sets a per-chat
  override that wins over the global TTL.

What the user wants instead:

- **App-level settings become DEFAULTS for new chats**, not active
  caps that apply across the whole identity.
- When a new chat is created (first inbound OR outbound message to
  a new peer, OR the user explicitly creates one), it inherits the
  current default retention value AT CREATION TIME. Changing the
  global later does not retroactively change existing chats.
- Each chat carries its own retention setting that the user can
  edit independently. The retention is either:
  - a maximum **age** (wall-clock TTL — what we have now), OR
  - a maximum **message count** (per-chat cap — new).
  Likely surfaced as a single "Retention" picker with both kinds
  of options (e.g. "Last 7 days", "Last 30 days", "Last 500
  messages", "Last 1000 messages", "Off").

Open questions to answer in the investigation phase:

1. **Is eviction actually happening?** `LXMFInbox::evict_expired()`
   exists for TTL eviction, but how / when is it called? Is there
   a periodic tick? Is it called on every read? Is the spool file
   compacted, or only the in-RAM deque? Trace the call graph and
   confirm that an expired message disappears both from RAM AND
   from disk within a reasonable window.
2. **Does the per-peer cap need a new storage shape?** Today
   `_peer_ttl` is a per-peer override on TTL; we'd need a parallel
   `_peer_max_messages` (or a single `_peer_retention` struct that
   holds {ttl_seconds, max_messages} and is per-peer). Persist
   to `/lxmf/<identity>/peer_retention.json` or extend whatever
   file currently stores the per-peer TTLs.
3. **Inheritance semantics on chat-creation:** snapshot the global
   default into the per-peer config the moment a peer first
   appears? Or store a sentinel "inherit" value and resolve at
   read time? Snapshot is simpler (no spooky retroactive
   changes); resolve-at-read is more flexible (default change
   propagates to any chat that hasn't been overridden). User's
   description leans toward snapshot.
4. **UI changes:** the per-chat retention modal currently only
   shows TTL options; needs to gain message-count options too.
   The Settings → App → "Inbox retention" card needs renaming /
   reframing as "Default retention for new chats" so the
   semantics are clear. The global `ram_capacity` and
   `ttl_seconds` ride together in one default setting — but the
   user wants ONE OR THE OTHER per chat (time OR count, not both
   simultaneously), so the global default should also be a single
   picker.
5. **Migration:** existing identities have `ram_capacity` /
   `ttl_seconds` set as global caps. On the first run after this
   change, snapshot the current global value onto every existing
   peer that doesn't already have an override, then collapse the
   global to a "default for NEW chats" role. This avoids losing
   users' explicit overrides.

This is an investigation-first task: answer the open questions
above, document the proposed model, then implement in a follow-up
once the design is agreed.

---

## Coalesce sensor updates into a periodic WS frame

**Status:** pending

Today each sensor driver that takes a periodic reading calls
`Web::WebSocket::publish_sensor(kind, fill)` synchronously the
moment it has a fresh value. Each call ships a separate
`sensor_update` frame with one `kind`. When several sensors share
the same interval (e.g. three sensors on a 60 s cadence), they
each fire their own WS frame at the same instant — three small
frames instead of one.

Rework:

- Sensor drivers stop calling `publish_sensor` directly. Instead,
  when a sensor takes a reading it stashes the latest value in a
  small pending map keyed by `kind` (overwriting any earlier
  pending value for that kind — only the freshest one ships).
- A periodic task on the web/loop side drains the pending map. If
  it's empty: emit nothing. If it has entries: emit ONE frame
  `{type:"sensor_update", values:{kind1:..., kind2:..., ...}}`
  containing every kind that updated since the last drain.
- SPA: switch the receive handler to the multi-kind shape, applying
  each kind to its cached snapshot. The single-kind shape is
  removed entirely — every emitter goes through the pending map.
- Drain cadence is a hard-coded constant for now (NOT
  user-configurable) — start at 5 s, which is the natural floor
  for SPA-visible updates. Faster than the slowest sensor → no
  effect; slower than the fastest sensor → coalesces.
- The drain task should be cheap when there's nothing to send:
  early-return on an empty pending map.

Benefits: fewer WS frames per second under heavy sensor load
(important when several sensors share a cadence), and a single
place to throttle if we ever want a configurable knob. Doesn't
change sensor sampling intervals — those stay per-sensor.

Out of scope: per-sensor enable/disable (already a separate
mechanism); compression; client-side debouncing.

---

## Investigate where messages are stored + whether SD migration makes sense

**Status:** done (investigation only — no code change)

Current state, audited from the code:

- **Filesystem backend** on supreme: `microStore::InternalFSFileSystem`
  is selected in `RNode_Firmware.ino` for the ESP32 path (the only
  branch the supreme builds hit; `MCU_VARIANT == MCU_NRF52` is the
  branch that picks RAK15001 or internal flash on Adafruit nRF
  variants). microStore::InternalFSFileSystem wraps the platform-
  default LittleFS partition on the device's internal flash
  (4.4 MB `spiffs` partition in `partitions_supreme_8mb.csv`).
- **Message spool**: `LXMFInbox` writes `inbox.jsonl` and
  `outbox.jsonl` under `<identity_dir>/`, which is
  `/lxmf/identities/<id>/`. So **all messages live on internal
  flash**, JSONL-appended, bounded by `ram_capacity` records.
- **Attachments**: written under `<identity_dir>/attachments/<name>`,
  also on internal flash by default. Each per-message attachment
  record carries a `backend` field (`"flash"` or `"sd"`) that
  tracks where the bytes actually are.
- **SD migration**: `Storage::Migration::run()` (triggered from the
  SPA storage popover) walks every identity's `attachments/`
  directory, copies each file flash→SD, deletes the flash original,
  and flips `backend` strings from `flash` → `sd` in the inbox /
  outbox records via `update_attachment_backends`. **Only
  attachments are migrated — the JSONL spools themselves are not
  moved.**

**Should the message spools also live on SD?**

Tradeoffs:

- *In favour*: a 200-message ring at ≤4 KiB per record is at most
  ~800 KiB, which is a noticeable chunk of the 4.4 MB partition.
  Multiple identities multiply that.
- *Against*: SD is removable. If the user pulls the card, they
  lose the entire message history — not just the optional media.
  Flash-resident messages give users the "I always have my history
  even if the card goes" guarantee that messaging apps rely on.
  The current split (small text-y stuff on flash, big media on SD)
  matches how phone messaging apps treat removable storage.

**Recommendation (not implemented):** keep the spools on flash by
default. If we revisit, the right unit of choice is per-identity,
not per-message — splitting a single JSONL across backends is
not worth the bookkeeping. A user toggle "store this identity's
messages on SD" could rewrite both `inbox.jsonl` and `outbox.jsonl`
to the SD subtree and update a path constant. That's a separate
task; the present recommendation is to do nothing.

**Closes** the question raised in conversation; no follow-up
expected unless the recommendation is reversed.
