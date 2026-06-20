// ============================== WS EVENT DISPATCH ==============================
function dispatchEvent(ev) {
  if (ev.type === '_reconnect') {
    // We've been disconnected long enough - re-fetch /state to resync.
    refreshState();
    // Drop the local radio-telemetry seed marker so the next popover
    // open re-seeds from /api/radio/telemetry rather than trusting a
    // stale ring that has a gap from the disconnect.
    if (state.radioTlm) state.radioTlm.seeded = false;
    if (state.netTlm)   state.netTlm.seeded   = false;
    return;
  }
  if (ev.type === 'hello') {
    // Snapshot the device clock anchor for relative-time displays.
    // Carries millis() + boot epoch (for received_ms anchoring) plus
    // unix_ms + calibrated flag (for the live clock pill). Pinning
    // both anchors once per WS session is the cheapest correct option:
    // localWallMs / localPerfMs come from the same wall-clock moment.
    const c = ev.clock || {};
    if (typeof c.now_ms === 'number') {
      state.clockAnchor = {
        deviceNowMs:      c.now_ms,
        deviceUnixMs:     Number(c.unix_ms || 0),
        calibrated:       !!c.calibrated,
        source:           c.source || 'none',
        // Device millis() of the last successful calibration. 0 = never.
        // Combined with deviceNowMs at render time to display
        // "Last calibrated Xs ago".
        lastCalibratedMs: Number(c.last_calibrated_ms || 0),
        currentBootEpoch: c.current_boot_epoch || 0,
        localWallMs:      Date.now(),
        localPerfMs:      performance.now(),
      };
    }
    // Hello carries the full system snapshot (storage / sensors /
    // battery / outbound_caps / rtc). Cache it so popovers can render
    // immediately without a /api/system_status fetch.
    state.system = {
      storage:       ev.storage       || {},
      sensors:       ev.sensors       || {},
      battery:       ev.battery       || null,
      outbound_caps: ev.outbound_caps || {},
      rtc:           ev.rtc           || {},
    };
    // Battery icon binds reactively to state.system.battery.
    // Sync the composer's staging cap + SD flag from frame zero.
    applySystemDerived();
    return;
  }
  if (ev.type === 'system_update') {
    // Periodic system refresh - replace cached snapshot wholesale. The
    // payload mirrors the hello shape, just nested under .payload.
    const p = ev.payload || {};
    state.system = {
      storage:       p.storage       || {},
      sensors:       p.sensors       || {},
      battery:       p.battery       || null,
      outbound_caps: p.outbound_caps || {},
      rtc:           p.rtc           || {},
    };
    applySystemDerived();
    // Repaint open popovers + the topbar battery icon in place.
    // The element ID is `popover-system` (set by the HTML); the
    // earlier `sys-popover` lookup was a typo, making this branch a
    // permanent no-op. Result: system_update WS frames silently
    // updated state.system but never repainted the open popover -
    // so any optimistic-patched value the SPA wrote (e.g. a sensor
    // interval just changed by the user) appeared correct, and a
    // later snapshot couldn't correct it because the render path
    // was unreachable.
    // System popover + topbar battery are reactive on state.system -
    // no explicit re-render needed.
    return;
  }
  if (ev.type === 'radio_telemetry') {
    // 1 Hz push from the firmware. Sample fields populate the local
    // ring for the chart; cumulative rx_packets/tx_packets are tagged
    // onto every live frame (not the history fill) so the popover's
    // RX/TX stat line can tick without waiting for the open-popover
    // /api/info refresh.
    if (typeof ev.ts === 'number') {
      const rt = state.radioTlm;
      const { type, rx_packets, tx_packets, ...s } = ev;
      rt.samples.push(s);
      while (rt.samples.length > rt.cap) rt.samples.shift();
      // Patch cumulative counters into the cached info snapshot so
      // both the popover's textual fact line and renderStatusPopover's
      // delta calculation stay current.
      if (state.lastInfo && state.lastInfo.radio && state.lastInfo.radio.stats) {
        if (typeof rx_packets === 'number') state.lastInfo.radio.stats.rx_packets = rx_packets;
        if (typeof tx_packets === 'number') state.lastInfo.radio.stats.tx_packets = tx_packets;
      }
      // Chart / CSMA / status text now reactive - the radioTlm push
      // mutation above is observed by the Alpine bindings.
    }
    return;
  }
  if (ev.type === 'network_telemetry') {
    // 1 Hz WiFi/transport tx/rx byte-rate push. Append to the netTlm ring;
    // the chart + rate line are reactive on samples.length.
    if (typeof ev.ts === 'number') {
      const nt = state.netTlm;
      const { type, ...s } = ev;
      nt.samples.push(s);
      while (nt.samples.length > nt.cap) nt.samples.shift();
    }
    return;
  }
  if (ev.type === 'sensors_update') {
    // Multi-kind sensor update. Patch each kind into the cached
    // snapshot so the periodic system_update doesn't drop fine-
    // grained state in between pushes. ev.values is a {kind: data}
    // object, one entry per sensor that had a fresh reading in the
    // last drain window.
    if (state.system && ev.values && typeof ev.values === 'object') {
      state.system.sensors = state.system.sensors || {};
      for (const kind in ev.values) {
        state.system.sensors[kind] = ev.values[kind];
      }
    }
    return;
  }
  if (ev.type === 'sensor_live_state') {
    // Which sensors are being fast-polled right now, from this client,
    // another client, or a device live screen (all share one window).
    // {environment:{live,ttl_ms}, magnetometer:{...}, imu:{...}, gps:{powered,mode}}
    state.sensorLive = ev.sensors || {};
    return;
  }
  if (ev.type === 'storage_changed') {
    // SD insert/eject transition. Refresh cached storage block, retune
    // the Settings sliders if they're visible, and toast the user.
    if (state.system) {
      state.system.storage = {
        user_max_send_bytes:      ev.user_max_send_bytes,
        user_max_receive_bytes:   ev.user_max_receive_bytes,
        effective_max_send_bytes: ev.effective_max_send_bytes,
        effective_max_recv_bytes: ev.effective_max_recv_bytes,
      };
    }
    if (typeof refreshStorageSliders === 'function') refreshStorageSliders();
    if (typeof toast === 'function') {
      const mib = (n) => Math.round((n || 0) / (1024 * 1024));
      if (ev.sd_present) {
        toast(`SD detected: up to ${mib(ev.effective_max_recv_bytes)} MiB receive`, 'info');
      } else {
        toast(`SD removed: caps clamped to ${mib(ev.effective_max_recv_bytes)} MiB receive`, 'warn');
      }
    }
    return;
  }
  if (ev.type === 'pong') return;
  if (ev.type === 'incoming') {
    const m = ev.msg || {};
    upsertMessage({
      peer: m.peer, seq: m.seq, ts: m.ts,
      // boot_epoch + received_ms are required for correct sort order -
      // without them new messages land at the bottom of the conversation
      // because msgCmp treats missing values as 0.
      boot_epoch: m.boot_epoch || 0,
      received_ms: m.received_ms || 0,
      title: m.title || '', body: m.body || '',
      in: true, sig_ok: !!m.sig_ok, status: 'delivered',
      // Stamp verdict rides along only when a stamp policy applied.
      ...(typeof m.stamp_ok === 'boolean' ? { stamp_ok: m.stamp_ok } : {}),
      ...(typeof m.stamp_value === 'number' ? { stamp_value: m.stamp_value } : {}),
      ...(m.tel ? { tel: true } : {}),
      ...(m.tele ? { tele: m.tele } : {}),
      attachments: m.attachments || [],
    });
    renderConversations();
    if (state.openPeer === m.peer) renderThread();
  } else if (ev.type === 'outbox_new') {
    // An outbound message hit the outbox - from the HTTP API, another
    // browser tab, or our own send. upsertMessage dedups on seq +
    // direction, so a bubble this tab already added optimistically
    // (sendMessage uses the same queued seq) is merged in place rather
    // than duplicated; sends from elsewhere create a fresh bubble and
    // the conversation if it's new. The firmware's `pkt` maps to our
    // `packet_hash` so the later outbox_status lifecycle pill can match.
    const m = ev.msg || {};
    upsertMessage({
      peer: m.peer, seq: m.seq, ts: m.ts,
      // boot_epoch + received_ms drive sort order (see msgCmp); without
      // them a new message mis-sorts to the top of the thread.
      boot_epoch: m.boot_epoch || 0,
      received_ms: m.received_ms || 0,
      title: m.title || '', body: m.body || '',
      in: false, sig_ok: true, status: m.status || 'sent',
      packet_hash: m.pkt || '',
      ...(typeof m.stamp_ok === 'boolean' ? { stamp_ok: m.stamp_ok } : {}),
      ...(typeof m.stamp_value === 'number' ? { stamp_value: m.stamp_value } : {}),
      ...(m.tel ? { tel: true } : {}),
      ...(m.tele ? { tele: m.tele } : {}),
      attachments: m.attachments || [],
    });
    renderConversations();
    if (state.openPeer === m.peer) renderThread();
  } else if (ev.type === 'telemetry_update') {
    // Fresh readings for a live feed. Patch the feed entry in place so
    // the strip re-renders without a round-trip.
    const f = (state.telemetryShares.feeds || []).find(f => f.peer === ev.peer);
    if (f) {
      f.tele        = ev.tele;
      f.updatedAtMs = Date.now();
    } else {
      refreshTelemetryShares();   // feed we didn't know about yet
    }
    // Reflect the latest reading on the message that started this live feed -
    // the most recent telemetry-bearing message from this peer - so its bubble
    // tracks the live state like the top strip, and shows the last value once
    // the feed ends. (Browser-side; the firmware persists the latest sample so
    // it survives a reload.)
    const conv = state.conversations[ev.peer];
    if (conv && ev.tele && typeof ev.tele.lat === 'number') {
      for (let i = conv.msgs.length - 1; i >= 0; i--) {
        if (conv.msgs[i].tele) { conv.msgs[i].tele = ev.tele; break; }
      }
      if (state.openPeer === ev.peer) renderThread();
    }
    // The position in this event is the new track point - append it to the
    // peer's live GPX in memory and redraw (thumbnail + open full track), with
    // no file download. The firmware writes the same point to disk for reload;
    // here we just extend what we already have. Only the most recent GPX-bearing
    // message from the peer (the live one) is touched.
    if (conv && typeof ev.tele.lat === 'number') for (let i = conv.msgs.length - 1; i >= 0; i--) {
      const g = messageGpx(conv.msgs[i]);
      if (g) { gpxLiveAppend(g.filename, ev.tele.lat, ev.tele.lon); break; }
    }
    mapLive(ev);                  // extend the peer's track + refresh the map
  } else if (ev.type === 'announce_seen') {
    // Convert the device's monotonic age_ms to a wall-clock anchor so
    // we can refresh "X ago" labels purely client-side.
    ev.seenAt = Date.now() - (ev.age_ms || 0);
    state.announces[ev.dest] = ev;
    renderAnnounces();
    renderConversations();  // also surfaces in the Discovered group
  } else if (ev.type === 'path_seen') {
    ev.seenAt = Date.now() - (ev.age_ms || 0);
    state.paths[ev.dest] = ev;
    renderPaths();
  } else if (ev.type === 'identity_code_available') {
    state.pendingIdCode = true;
    if (state.pendingIdCodeAction) {
      // Already waiting to consume - code is fresh on the OLED.
      openIdCodeModal({ codePresent: true });
    }
  } else if (ev.type === 'outbox_status') {
    // Lifecycle pill update for an outbound message bubble. The status pill
    // flips finding_route → sent → delivered, or → failed, without a re-fetch.
    // Most events key on the packet/link hash; a queued send that gave up
    // before it ever had a packet keys on its outbox seq instead.
    const link = ev.link_hash;
    const seq  = (typeof ev.seq === 'number') ? ev.seq : undefined;
    if (!link && seq === undefined) return;
    let touched = false;
    for (const conv of Object.values(state.conversations)) {
      for (const m of conv.msgs) {
        if (m.in) continue;
        if ((link && m.packet_hash === link) || (seq !== undefined && m.seq === seq)) {
          m.status = ev.status;
          // A seq-keyed frame can also carry the packet hash the send
          // acquired at dispatch (a stamped send spends its generation
          // phase hashless) - adopt it so the later hash-keyed frames
          // (sent → delivered) match this bubble.
          if (seq !== undefined && m.seq === seq && link && !m.packet_hash) {
            m.packet_hash = link;
          }
          touched = true;
        }
      }
    }
    // Self-heal the progress UI: once a send reaches a terminal status,
    // drop any transfer entry still keyed on its hash. message_complete
    // normally clears it, but if that event is missed (or the device
    // advanced status without a final progress frame) the entry would
    // otherwise strand every progress surface - contact-list row, global
    // bar, and the on-bubble bar - at its last percentage, and a later
    // send to the same peer would show the stale one. Keyed on `link`
    // (the packet hash), which equals the transfer key for outbound.
    if (link && state.transfers && state.transfers[link] &&
        (ev.status === 'sent' || ev.status === 'delivered' || ev.status === 'failed')) {
      delete state.transfers[link];
      renderConversations();
      renderTopbarProgress();
    }
    if (touched) {
      renderConversations();
      renderThread();
    }
  } else if (ev.type === 'time_update') {
    // Source adopted a fresh epoch - re-pin the clock anchor against
    // the current browser wall-clock moment so the popover ticker and
    // any new received_ms calculation use the post-sync mapping.
    if (typeof ev.unix_ms === 'number' && state.clockAnchor) {
      state.clockAnchor.deviceUnixMs     = Number(ev.unix_ms);
      state.clockAnchor.calibrated       = !!ev.calibrated;
      state.clockAnchor.source           = ev.source || state.clockAnchor.source;
      state.clockAnchor.lastCalibratedMs = Number(ev.last_calibrated_ms || ev.now_ms || 0);
      if (typeof ev.now_ms === 'number') state.clockAnchor.deviceNowMs = Number(ev.now_ms);
      state.clockAnchor.localWallMs      = Date.now();
      state.clockAnchor.localPerfMs      = performance.now();
    }
  } else if (ev.type === 'message_progress' || ev.type === 'message_complete') {
    if (!state.transfers) state.transfers = new Map();
    const key = ev.link_hash;
    if (!key) return;
    // The terminal message_complete event carries peer="" - the device
    // doesn't have the peer hash at outbox-status callback time, only the
    // link hash. So look up the peer we already cached from the earlier
    // progress events keyed by link_hash, BEFORE deleting the entry.
    const existing = state.transfers[key];
    const peer = ev.peer || (existing && existing.peer) || null;
    if (ev.finished) {
      delete state.transfers[key];
    } else {
      // Inbound Resource progress arrives with peer="" - the Reticulum
      // responder side doesn't know who initiated the link until the
      // LXMF wire payload finishes decrypting on resource-complete
      // (LXMF doesn't send LINKIDENTIFY). Store the entry anyway with
      // peer=null so the top-bar progress strip + a synthetic
      // "Incoming attachment…" row in the conv list can reflect what's
      // happening; the real conversation row picks it up once the
      // `incoming` event arrives with the source_hash and this entry
      // is cleared by message_complete.
      state.transfers[key] = {
        peer,                       // may be null for inbound pre-decrypt
        incoming: !!ev.incoming,
        bytes_done: ev.bytes_done || 0,
        bytes_total: ev.bytes_total || 0,
      };
    }
    renderConversations();
    if (peer && state.openPeer === peer) renderThread();
    renderTopbarProgress();
  }
}

async function refreshState() {
  try {
    const r = await state.transport.getState(state.identityId);
    state.self = r.identity;
    // Clear by deleting own keys (NOT reassigning to {}) so Alpine's
    // reactive proxy on `state` keeps observing the same object - a
    // reassignment would orphan the proxy and silently break templates.
    for (const k of Object.keys(state.announces)) delete state.announces[k];
    for (const a of (r.announces || [])) {
      a.seenAt = Date.now() - (a.age_ms || 0);
      state.announces[a.dest] = a;
    }
    for (const k of Object.keys(state.paths)) delete state.paths[k];
    for (const p of (r.paths || [])) {
      p.seenAt = Date.now() - (p.age_ms || 0);
      state.paths[p.dest] = p;
    }
    // The clock anchor is set from the WS `hello` frame (see
    // dispatchEvent). /state no longer carries `clock` - the WS path
    // is canonical now. If a refresh fires before the WS opens, the
    // anchor stays null and relative-time labels fall back to
    // "earlier" until the first hello arrives.
    for (const k of Object.keys(state.conversations)) delete state.conversations[k];
    for (const c of (r.conversations || [])) {
      const conv = {
        peer: c.peer,
        msgs: [],
        last_ts:           c.last_ts,
        last_boot_epoch:   c.last_boot_epoch   || 0,
        last_received_ms:  c.last_received_ms  || 0,
        last_body:         c.last_body,
        last_in:           c.last_in,
      };
      for (const m of (c.messages || [])) conv.msgs.push(m);
      state.conversations[c.peer] = conv;
    }
    renderSelf();
    renderConversations();
    renderAnnounces();
    renderPaths();
    if (state.openPeer) renderThread();
  } catch (e) {
    console.warn('refreshState failed:', e);
  }
}

let _unsubscribe = null;
function startEventStream() {
  if (_unsubscribe) _unsubscribe();
  _unsubscribe = state.transport.subscribe(state.identityId, dispatchEvent);
}

