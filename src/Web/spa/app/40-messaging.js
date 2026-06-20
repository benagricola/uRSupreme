// ============================== RENDER ==============================
// Legacy DOM-cache + state-preservation helpers. Both are obsolete now
// that conv-list / thread / settings lists are rendered by Alpine -
// Alpine's template diffing preserves scroll / focus / details
// open-state natively. Kept and exported because a few legacy
// render-via-rebuild paths (settings tabs, popovers) still call them
// until the Wave 5 / 6 migrations land.
const _convRowEls = new Map();  // unused; kept to satisfy any stale ref

function withPreservedState(container, fn, opts = {}) {
  const detailsKey = opts.detailsKey || ((d, i) => 'idx:' + i);
  const scrollTop  = container.scrollTop;
  const scrollLeft = container.scrollLeft;
  // Snapshot <details> open/closed states by stable key.
  const openByKey = {};
  const detailEls = container.querySelectorAll('details');
  detailEls.forEach((d, i) => { openByKey[detailsKey(d, i)] = d.open; });
  // Snapshot focused element + selection (if inside container).
  let focusInfo = null;
  const ae = document.activeElement;
  if (ae && container.contains(ae)) {
    focusInfo = { id: ae.id, tag: ae.tagName, value: null, start: null, end: null };
    if (ae.tagName === 'INPUT' || ae.tagName === 'TEXTAREA') {
      try {
        focusInfo.start = ae.selectionStart;
        focusInfo.end   = ae.selectionEnd;
      } catch (_) {}
    }
  }

  fn();

  // Restore <details> open states by matching the new ones to old keys.
  container.querySelectorAll('details').forEach((d, i) => {
    const k = detailsKey(d, i);
    if (k in openByKey) d.open = openByKey[k];
  });
  // Restore focus + selection, if the target still exists.
  if (focusInfo && focusInfo.id) {
    const target = document.getElementById(focusInfo.id);
    if (target) {
      target.focus();
      if (focusInfo.start !== null && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA')) {
        try { target.setSelectionRange(focusInfo.start, focusInfo.end); } catch (_) {}
      }
    }
  }
  // Restore scroll position (the container's clientHeight may have
  // changed - clamp to new scrollHeight so we don't overshoot).
  container.scrollTop  = Math.min(scrollTop,  container.scrollHeight  - container.clientHeight);
  container.scrollLeft = Math.min(scrollLeft, container.scrollWidth   - container.clientWidth);
}

// renderSelf() used to imperatively rewrite the topbar identity bits;
// those elements are now bound declaratively (#self-idc, #self-name,
// #self-addr) and Alpine repaints automatically when state.self changes.
// Kept as a no-op so refreshState() / login flow callsites stay valid.
function renderSelf() {}

// Module-scope helpers that the conv-list template (Alpine) reads via
// `window.foo(...)`. They also stayed referenced from any still-legacy
// renderXxx paths until those are deleted in Wave 7.

// Sorted conversations ordered by (boot_epoch, received_ms) - newest
// activity first. ts is a fallback for legacy records.
function convListData() {
  return Object.values(state.conversations).sort((a, b) => {
    const ba = a.last_boot_epoch || 0, bb = b.last_boot_epoch || 0;
    if (ba !== bb) return bb - ba;
    const ra = a.last_received_ms || 0, rb = b.last_received_ms || 0;
    if (ra !== rb) return rb - ra;
    return (b.last_ts || 0) - (a.last_ts || 0);
  });
}

// Discovered = announces for peers we've never chatted with.
function discoveredListData() {
  const chatted = new Set(Object.keys(state.conversations));
  return Object.values(state.announces)
    .filter(a => !chatted.has(a.dest))
    .sort((a, b) => (b.received_ms || 0) - (a.received_ms || 0));
}

// Synthetic "Incoming" rows for inbound transfers whose sender isn't
// decrypted yet (peer="" in state.transfers).
function pendingInboundList() {
  if (!state.transfers || Object.keys(state.transfers).length === 0) return [];
  return Object.values(state.transfers).filter(t => t.incoming && !t.peer);
}

// Outbound attachment transfers in flight - rendered as an "Outgoing"
// section at the top of the contact list, mirroring pendingInboundList's
// "Incoming" so a send is as visible as a receive even when the recipient's
// row is scrolled out of view. Outbound transfers always have a known peer
// (unlike undecrypted inbound), so the row shows the recipient.
function pendingOutboundList() {
  if (!state.transfers || Object.keys(state.transfers).length === 0) return [];
  return Object.values(state.transfers).filter(t => !t.incoming && t.peer);
}

// "last seen N ago" / "online now" - coarsens with age.
function announceSeenString(rec) {
  if (!rec || !rec.seenAt) return '';
  const ageMs = Math.max(0, Date.now() - rec.seenAt);
  if (ageMs < 60_000) return 'online now';
  const m = Math.floor(ageMs / 60_000);
  if (m < 60) return 'last seen ' + m + 'm ago';
  const h = Math.floor(m / 60);
  if (h < 24) return 'last seen ' + h + 'h ago';
  const d = Math.floor(h / 24);
  return 'last seen ' + d + 'd ago';
}

// Discovered-section age label uses a different format (no "last seen"
// prefix, finer resolution at small times).
function discoveredAgeLabel(a) {
  if (!a.seenAt) return '';
  const ageMs = Math.max(0, Date.now() - a.seenAt);
  if (ageMs < 60_000) return 'online now';
  const ageS = Math.floor(ageMs / 1000);
  return 'last seen ' + (ageS < 3600 ? Math.floor(ageS/60) + 'm'
                       : ageS < 86400 ? Math.floor(ageS/3600) + 'h'
                       : Math.floor(ageS/86400) + 'd') + ' ago';
}

// Is this peer online (heard a real announce in the last 60s)?
function isPeerOnline(peer) {
  const a = state.announces[peer];
  return !!(a && a.seenAt && (Date.now() - a.seenAt) < 60_000);
}

// Find the active transfer for a given peer (outbound takes priority).
function activeTransferFor(peer) {
  if (!state.transfers || Object.keys(state.transfers).length === 0) return null;
  let outgoing = null, incoming = null;
  for (const t of Object.values(state.transfers)) {
    if (t.peer !== peer) continue;
    if (t.incoming) incoming = t; else outgoing = t;
  }
  return outgoing || incoming;
}

// Inbound-only variant, used by the conversation rows: outbound transfers
// surface in the dedicated "Outgoing" section at the top of the list
// (mirroring "Incoming"), so the recipient's row shows its normal preview
// rather than doubling up the same progress bar.
function activeInboundTransferFor(peer) {
  if (!state.transfers || Object.keys(state.transfers).length === 0) return null;
  for (const t of Object.values(state.transfers)) {
    if (t.peer === peer && t.incoming) return t;
  }
  return null;
}

// Type word for the contact-list preview of a captionless attachment message
// (image / audio / file with no text body). The icon itself is rendered as an
// inline SVG in the conv-row markup - no emoji.
function convAttachmentLabel(atts) {
  if (!Array.isArray(atts) || atts.length === 0) return '';
  if (atts.length > 1) return atts.length + ' attachments';
  const t = atts[0].tag;
  if (t === FIELD_IMAGE) return 'Image';
  if (t === FIELD_AUDIO) return 'Audio';
  return 'Attachment';
}

// Live-transfer subtext for a conv row: direction arrow + bytes + percent.
// The idle preview (direction arrow + body, or attachment icon + label) is
// rendered declaratively in the conv-row markup so the peer-controlled body
// stays x-text-escaped and the attachment indicator can be a real icon.
function convSubText(c) {
  const xfer = activeInboundTransferFor(c.peer);
  if (!xfer) return '';
  const arrow = xfer.incoming ? '↓ ' : '↑ ';
  const pct   = xfer.bytes_total ? Math.floor(100 * xfer.bytes_done / xfer.bytes_total) : 0;
  return arrow + formatBytes(xfer.bytes_done) + ' / ' + formatBytes(xfer.bytes_total) + ' (' + pct + '%)';
}

// Percentage helper - caps at [2, 100] so a fresh-start 0% bar still
// shows a hairline.
function transferPct(t) {
  if (!t || !t.bytes_total) return 0;
  return Math.max(2, Math.min(100, Math.floor(100 * t.bytes_done / t.bytes_total)));
}

// The in-flight transfer for a specific outbound message bubble, or null.
// The device keys message_progress on the message's record hash, which is
// the same value the bubble carries as packet_hash - so the bar binds to
// exactly the message being sent and vanishes when its entry is cleared.
function transferForMsg(m) {
  if (m.in || !m.packet_hash || !state.transfers) return null;
  return state.transfers[m.packet_hash] || null;
}

// Caption for the global top-bar progress strip. The strip is a single
// aggregate bar shown only where the contact list isn't on screen (mobile
// thread view), so it can't convey direction/identity the way a list row
// does. This names what it represents - and because several transfers can
// run at once in both directions under one bar, it lists them (up to two,
// then "+N more"), plus any labelled background task the bar also folds in.
function transfersCaption() {
  const out = [];
  const tfs = state.transfers ? Object.values(state.transfers) : [];
  for (const t of tfs) {
    const dir = t.incoming ? '↓ ' : '↑ ';
    const who = t.incoming ? 'Incoming attachment' : ('To ' + nameFor(t.peer));
    const pct = t.bytes_total ? ' ' + Math.floor(100 * t.bytes_done / t.bytes_total) + '%' : '';
    out.push(dir + who + pct);
  }
  for (const b of (state.bgTasks ? Object.values(state.bgTasks) : [])) {
    out.push(b.label || 'Working…');
  }
  if (out.length === 0) return '';
  if (out.length <= 2) return out.join('  ·  ');
  return out.slice(0, 2).join('  ·  ') + '  · +' + (out.length - 2) + ' more';
}

// Synthetic-row subtext: "12 KB / 30 KB · 40%"
function incomingSubText(t) {
  const pct = t.bytes_total ? Math.floor(100 * t.bytes_done / t.bytes_total) : 0;
  return formatBytes(t.bytes_done) + ' / ' + formatBytes(t.bytes_total) + ' · ' + pct + '%';
}

function outgoingSubText(t) {
  const pct = t.bytes_total ? Math.floor(100 * t.bytes_done / t.bytes_total) : 0;
  return formatBytes(t.bytes_done) + ' / ' + formatBytes(t.bytes_total) + ' · ' + pct + '%';
}

function renderConversations() {
  // Conv-list is now rendered by Alpine declaratively. Touch the
  // store so reactive subscribers re-evaluate - this is the bridge
  // for callers (WS dispatcher, etc.) that still call renderXxx().
  // No-op once Wave 7 cleans up callers.
  if (window.Alpine && state) {
    // Reading a reactive field is enough; Alpine sets up tracking on
    // first access. Writing isn't needed because mutations elsewhere
    // already propagate.
    void Object.keys(state.conversations).length;
  }
}


// ============== THREAD HELPERS (Alpine template support) ==============
// The conversation currently open. null when state.openPeer is unset
// (templates show empty-state). Returns the live store reference so
// `currentConv().msgs.push(...)` would actually mutate the store -
// not that any template needs to do that.
function currentConv() {
  const p = state && state.openPeer;
  return p ? (state.conversations[p] || null) : null;
}

function bubbleClass(m) {
  return 'bubble ' + (m.in ? 'in' : 'out')
       + (m.sig_ok === false && m.in ? ' bad' : '');
}

// Metadata footer of a bubble - wall-clock time, outbound status,
// optional stamp verdict, optional bad-signature warning. Empty string
// when nothing to show (the template renders that as no footer).
function msgMeta(m) {
  const parts = [];
  if (m.ts) parts.push(new Date(m.ts * 1000).toLocaleTimeString([], { hour:'2-digit', minute:'2-digit' }));
  if (!m.in && m.status) {
    // While a Resource transfer is in flight for this message, the status
    // word is the live "sending N%" (the thin bar under this line shows the
    // same) - the raw lifecycle status (queued/sent) would otherwise read
    // as stale next to an actively-moving bar.
    const xfer = transferForMsg(m);
    parts.push(xfer                              ? 'sending ' + transferPct(xfer) + '%'
             : m.status === 'generating_stamp'   ? 'generating stamp…'
             : m.status === 'finding_route'      ? 'finding route'
             : m.status);
  }
  // Stamp verdict - present only when a stamp policy applied to this
  // message (the device omits the field otherwise). stamp_value is the
  // effort the sender attached; it only exists for a valid stamp.
  if (m.stamp_ok === true) {
    parts.push('stamp ✓' + (typeof m.stamp_value === 'number' ? ' ' + m.stamp_value : ''));
  } else if (m.stamp_ok === false && m.in) {
    parts.push('⚠ no valid stamp');
  }
  if (m.in && m.sig_ok === false) parts.push('⚠ bad sig');
  return parts.join(' · ');
}

// ============== ATTACHMENT-CHIP HELPERS (Alpine bindings) ==============
// The bubble template's <template x-for="a in m.attachments"> calls
// these to decide which chip variant to render and (for inline-media
// modes) to source the blob URL from the cache. Loader entry point:
// loadAttachmentBlob(a) below, which sets state.attachmentBlobs[a.filename]
// = 'loading' / blobUrl / { failed: true, error }.

// Is this attachment renderable at all? Outbound attachments with no
// on-disk filename AND no display_name AND no size are skipped - they
// carried no actionable content. Mirrors attachmentNode's old check.
function attachmentVisible(a) {
  return !!a && (!!a.filename || !!a.display_name || (a.size > 0));
}

// Name + size label (text only - the type icon is a CSS class from
// attachmentIconClass on the chip element). Display name comes from
// the sender (Sideband convention); blank → fallback to type word.
function attachmentLabel(a) {
  const typeWord = a.tag === 5 ? 'file' : a.tag === 6 ? 'image' : a.tag === 7 ? 'audio' : 'attachment';
  const name = (a.display_name || '').trim();
  return (name || typeWord) + ' (' + formatBytes(a.size || 0) + ')';
}

// CSS-mask icon class for an attachment's type (see styles.css
// .ico-*::before).
function attachmentIconClass(a) {
  return a.tag === 6 ? 'ico-image' : a.tag === 7 ? 'ico-mic' : 'ico-file';
}

// Picks the variant template (the <template x-if> in the bubble
// markup) that should render for this attachment. Variants:
//   'sd_unreachable'      - blob lives on SD that's been ejected
//   'inline-image'        - auto-rendered image (auto-download pref or cached)
//   'inline-audio'        - auto-rendered audio player
//   'click-to-load'       - image/audio chip awaiting a click to fetch
//   'click-to-download'   - generic-file chip (no inline preview)
//   'static'              - info-only chip (no on-disk blob exists)
function attachmentMode(a) {
  // A persisted on-disk blob (hash filename we can fetch back), OR a
  // locally-cached preview blob - an optimistic just-sent image previews
  // from the bytes we still hold, before the device has persisted it under
  // a hash filename.
  const hasBlob = /^[0-9a-f]+_[0-9a-f]+_\d+\.bin$/.test(a.filename || '')
    || (!!a.filename && a.filename in state.attachmentBlobs);
  if (hasBlob && a.backend === 'sd' && state.sdPresent === false) return 'sd_unreachable';
  // A GPX renders as a map track (fetched on demand), not a file chip.
  if (isGpxAttachment(a)) {
    if (a.backend === 'sd' && state.sdPresent === false) return 'sd_unreachable';
    return 'gpx';
  }
  if (hasBlob && (a.tag === 6 || a.tag === 7)) {
    const cached = a.filename in state.attachmentBlobs;
    if (state.prefs.autoDownloadAttachments || cached) {
      return a.tag === 6 ? 'inline-image' : 'inline-audio';
    }
    return 'click-to-load';
  }
  if (hasBlob) return 'click-to-download';
  return 'static';
}

// Auto-loader: kicks off a blob fetch when the inline template
// mounts and there's no cache entry yet. Called from x-init on each
// inline-image / inline-audio variant.
async function loadAttachmentBlob(a) {
  if (!a || !a.filename) return;
  if (a.filename in state.attachmentBlobs) return;  // already loading / done / failed
  state.attachmentBlobs[a.filename] = 'loading';
  try {
    const { blob } = await state.transport.fetchAttachment(state.identityId, a.filename);
    let typed = blob;
    // Audio in particular needs the right MIME so Chrome/Safari pick
    // a decoder. Re-wrap with the sniffed type.
    if (a.tag === 7 && typeof blob.arrayBuffer === 'function') {
      const head = new Uint8Array(await blob.slice(0, 16).arrayBuffer());
      typed = new Blob([blob], { type: sniffAudioMime(head) });
    }
    state.attachmentBlobs[a.filename] = URL.createObjectURL(typed);
  } catch (e) {
    state.attachmentBlobs[a.filename] = { failed: true, error: e.message || String(e) };
  }
}

// User clicked Retry on a failed inline preview. Clear the cache
// entry and let the bubble re-mount kick off a fresh fetch.
function retryAttachment(a) {
  if (a && a.filename) delete state.attachmentBlobs[a.filename];
  // The inline-* template's x-init fires again on next reactive eval
  // (Alpine remounts the x-if branch); calling loadAttachmentBlob
  // here too guarantees it kicks off even if the template branch
  // hasn't been re-evaluated yet.
  loadAttachmentBlob(a);
}

// Is the cache entry a {failed: true, error} record?
function isBlobFailed(v) { return v != null && typeof v === 'object' && v.failed === true; }

// "Stickiness" auto-scroll: if the user was at/near the bottom of the
// thread before a new message arrived, snap to the new bottom. If they
// were scrolled up reading history, leave them alone. Called from an
// x-effect on the thread-msgs container so it re-evaluates whenever
// the message list changes.
function maybeScrollThreadToBottom() {
  const el = document.getElementById('thread-msgs');
  if (!el || el.hidden) return;
  const NEAR_BOTTOM_PX = 80;
  const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - NEAR_BOTTOM_PX;
  // Defer to next frame so the template has finished applying its
  // template diff before we measure scrollHeight.
  requestAnimationFrame(() => {
    if (atBottom) el.scrollTop = el.scrollHeight;
  });
}

// Thread rendering is declarative now - the header / message list /
// compose row are all bound to $store.s via x-show / x-text / x-for.
// renderThread() is kept as a no-op so callers like selectConversation()
// and the WS dispatcher remain unchanged; Wave 7 deletes them.
function renderThread() {}


// Bytes formatter shared by progress UI and attachment chips.
function formatBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / (1024 * 1024)).toFixed(2) + ' MB';
}
// HTML-rendering variant. Same value formatting as formatBytes, but
// wraps the unit (B / KB / MB) in <span class="unit"> so the popover
// CSS dims and shrinks it relative to the digits. Caller must use
// innerHTML, not textContent.
function formatBytesVU(n) {
  if (n < 1024) return vu(n, 'B');
  if (n < 1024 * 1024) return vu((n / 1024).toFixed(1), 'KB');
  return vu((n / (1024 * 1024)).toFixed(2), 'MB');
}

// Sniff an audio file's MIME from its first few bytes so the browser
// picks the right decoder. Returns `audio/*` strings the major
// browsers know how to play. Defaults to audio/ogg (Opus is the
// most common LXMF voice-note format).
function sniffAudioMime(bytes) {
  if (bytes.length < 4) return 'audio/ogg';
  // "OggS" - Ogg container (Vorbis or Opus)
  if (bytes[0] === 0x4F && bytes[1] === 0x67 && bytes[2] === 0x67 && bytes[3] === 0x53) return 'audio/ogg';
  // "RIFF" + "WAVE" - WAV
  if (bytes[0] === 0x52 && bytes[1] === 0x49 && bytes[2] === 0x46 && bytes[3] === 0x46
      && bytes.length >= 12
      && bytes[8] === 0x57 && bytes[9] === 0x41 && bytes[10] === 0x56 && bytes[11] === 0x45) return 'audio/wav';
  // "ID3" tag or MPEG audio sync 0xFFFB / 0xFFFA / 0xFFE3 etc.
  if (bytes[0] === 0x49 && bytes[1] === 0x44 && bytes[2] === 0x33) return 'audio/mpeg';
  if (bytes[0] === 0xFF && (bytes[1] & 0xE0) === 0xE0) return 'audio/mpeg';
  // "fLaC" - FLAC
  if (bytes[0] === 0x66 && bytes[1] === 0x4C && bytes[2] === 0x61 && bytes[3] === 0x43) return 'audio/flac';
  // ftyp box + "M4A " - AAC in MP4
  if (bytes.length >= 12
      && bytes[4] === 0x66 && bytes[5] === 0x74 && bytes[6] === 0x79 && bytes[7] === 0x70) return 'audio/mp4';
  return 'audio/ogg';
}


// Fetch the attachment blob from the device. Image (tag 6) / audio
// (tag 7): cache the blob URL in state.attachmentBlobs - the bubble
// template observes that and re-evaluates attachmentMode() to swap
// from chip → inline preview automatically.
// Everything else (tag 5 / unknown): trigger a browser Save-As.
async function downloadAttachment(identityId, meta, chipEl /*unused - kept for backward compat*/ ) {
  if (!meta || !meta.filename) return;
  if (state.attachmentBlobs[meta.filename] === 'loading') return;
  state.attachmentBlobs[meta.filename] = 'loading';
  try {
    const { blob } = await state.transport.fetchAttachment(identityId, meta.filename);
    if (meta.tag === 6) {
      state.attachmentBlobs[meta.filename] = URL.createObjectURL(blob);
    } else if (meta.tag === 7) {
      const head = new Uint8Array(await blob.slice(0, 16).arrayBuffer());
      const mime = sniffAudioMime(head);
      const typed = new Blob([await blob.arrayBuffer()], { type: mime });
      state.attachmentBlobs[meta.filename] = URL.createObjectURL(typed);
    } else {
      // File / unknown: trigger Save-As via the static offscreen
      // #dl-link anchor in markup. Set href + download, click, revoke.
      const url = URL.createObjectURL(blob);
      const a = $('dl-link');
      a.href = url;
      a.download = meta.display_name || meta.filename;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 30000);
      // Don't cache non-media in attachmentBlobs - there's no inline
      // preview, so caching just wastes memory. Clear the placeholder.
      delete state.attachmentBlobs[meta.filename];
      toast('Saved ' + (meta.display_name || meta.filename), 'success');
    }
  } catch (e) {
    state.attachmentBlobs[meta.filename] = { failed: true, error: e.message || String(e) };
    toast('Attachment fetch failed: ' + (e.message || e), 'error');
  }
}

// Ordering key for messages: LXMF ts (sender wall-clock) primary,
// (boot_epoch, received_ms) tiebreak.
//
// Why ts first: now that the device's RTC is reliable, the LXMF
// timestamp on each message is a real UTC wall-clock value from the
// sender. That matches how the user thinks about chat order
// ("messages I sent at 10:01 come before the reply at 10:02"),
// across reboots and across peer clock skew within a conversation.
//
// Why tiebreak on (boot_epoch, received_ms): two messages can share
// a ts (1 s resolution, or sender's clock drifting backwards). The
// receive-side tuple is monotonic within a boot, and boot_epoch
// disambiguates across reboots - so the tiebreak orders by the
// order WE received them, which preserves causal sense for the
// receiver's view.
function msgCmp(a, b) {
  const ta = a.ts || 0, tb = b.ts || 0;
  if (ta !== tb) return ta - tb;
  const ba = a.boot_epoch || 0, bb = b.boot_epoch || 0;
  if (ba !== bb) return ba - bb;
  return (a.received_ms || 0) - (b.received_ms || 0);
}

function upsertMessage(m) {
  const peer = m.peer || m.peer_hex;
  if (!peer) return;
  let c = state.conversations[peer];
  if (!c) {
    c = { peer, msgs: [], last_ts: 0, last_boot_epoch: 0, last_received_ms: 0, last_body: '', last_in: false, last_attachments: [] };
    state.conversations[peer] = c;
  }
  // Dedup by seq + direction (inbox and outbox have independent seq
  // counters). When we already have this message - e.g. an optimistic
  // outbox record inserted by sendMessage before the device's
  // authoritative copy lands via /state refresh - merge the incoming
  // fields in rather than discarding them. Optimistic records may have
  // boot_epoch/received_ms = 0 if the clockAnchor wasn't set yet at
  // send time, which would mis-sort them to the top of the thread.
  const existing = c.msgs.find(x => x.seq === m.seq && x.in === m.in);
  if (existing) {
    let dirty = false;
    if (m.boot_epoch && m.boot_epoch > (existing.boot_epoch || 0)) {
      existing.boot_epoch = m.boot_epoch; dirty = true;
    }
    if (m.received_ms && m.received_ms > (existing.received_ms || 0)) {
      existing.received_ms = m.received_ms; dirty = true;
    }
    if (m.ts && (!existing.ts || m.ts > existing.ts)) {
      existing.ts = m.ts;
    }
    if (typeof m.status === 'string' && m.status !== existing.status) {
      existing.status = m.status;
    }
    // Fill in a packet_hash the existing record lacked (e.g. an
    // optimistic send whose /send response carried none) so the later
    // outbox_status lifecycle pill can match this bubble by hash.
    if (m.packet_hash && !existing.packet_hash) {
      existing.packet_hash = m.packet_hash;
    }
    // Adopt a stamp verdict the existing record lacked - e.g. a stamped
    // send whose authoritative outbox record lands after dispatch.
    if (typeof m.stamp_ok === 'boolean' && typeof existing.stamp_ok !== 'boolean') {
      existing.stamp_ok = m.stamp_ok;
      if (typeof m.stamp_value === 'number') existing.stamp_value = m.stamp_value;
    }
    // Adopt telemetry the existing record lacked (e.g. the
    // authoritative copy of an optimistic send).
    if (m.tel && !existing.tel) existing.tel = true;
    if (m.tele && !existing.tele) existing.tele = m.tele;
    // Adopt the incoming attachments when they're more complete than what
    // the bubble already carries: either more of them, or the authoritative
    // persisted (hash-filename) versions replacing an optimistic record's
    // user-filename placeholders. Without the latter, an optimistically-sent
    // image keeps its un-fetchable "image.jpg" name and won't preview from
    // the device until a full page reload pulls the persisted record.
    const looksPersisted = arr => Array.isArray(arr) && arr.length > 0
      && arr.every(x => /^[0-9a-f]+_[0-9a-f]+_\d+\.bin$/.test(x.filename || ''));
    if (Array.isArray(m.attachments) && m.attachments.length > 0
        && (m.attachments.length > (existing.attachments || []).length
            || (looksPersisted(m.attachments) && !looksPersisted(existing.attachments)))) {
      existing.attachments = m.attachments;
    }
    if (dirty) c.msgs.sort(msgCmp);
  } else {
    c.msgs.push(m);
    c.msgs.sort(msgCmp);
  }
  const last = c.msgs[c.msgs.length - 1];
  c.last_ts          = last.ts;
  c.last_boot_epoch  = last.boot_epoch || 0;
  c.last_received_ms = last.received_ms || 0;
  c.last_body        = last.body || '';
  c.last_in          = last.in;
  c.last_attachments = Array.isArray(last.attachments) ? last.attachments : [];
}

// Announces / Paths / Contacts are all rendered declaratively by Alpine
// templates against the corresponding $store.s.* fields. These functions
// stayed only to keep their callsites valid; Wave 7 deletes them.
function renderAnnounces() {}
function renderPaths()    {}
function renderContacts() {}

// ============================== INTERACTIONS ==============================
async function selectConversation(peer) {
  if (state.openPeer !== peer && state.pendingAttachments.length > 0) {
    // Dropping staged attachments quietly - they belonged to the
    // previous chat. Surface a toast so the user notices.
    toast(state.pendingAttachments.length + ' attachment(s) dropped from previous chat.', 'info');
    state.pendingAttachments = [];
    renderAttachTray();
  }
  if (state.openPeer !== peer) {
    // A telemetry attach is aimed at one recipient; never carry it
    // into another chat.
    state.forms.compose.telemetry = null;
    state.popovers.telemetry = false;
  }
  state.openPeer = peer;
  // Pick up any active live share with this peer for the strip.
  refreshTelemetryShares();
  // Make sure a record exists even if empty. The conv-row "active"
  // highlight is now driven by Alpine's :class binding against
  // $store.s.openPeer; no manual DOM toggling needed.
  if (!state.conversations[peer]) {
    state.conversations[peer] = { peer, msgs: [], last_ts: 0 };
  }
}

// True while the send button should show a spinner + reject clicks: either a
// send is in flight, or a pending attachment is still being prepared (bytes
// read / image encoded) and isn't ready to go yet.
function sendBtnBusy() {
  return !!state.busyButtons['btn-send']
      || (state.pendingAttachments || []).some(a => a.processing);
}

async function sendMessage() {
  const text = (state.forms.compose.text || '').trim();
  if (!state.openPeer) return;
  // Text, files or telemetry - any one makes a sendable message
  // (a telemetry-only send works like an image-only one).
  if (!text && state.pendingAttachments.length === 0 && !telemetryAttachActive()) return;
  // The textarea's :maxlength counts UTF-16 code units but the device
  // cap (and the 413 it returns) is UTF-8 bytes, so multibyte text can
  // pass the input limit and still be over-cap. Check real bytes here
  // instead of letting the server bounce it after the fact. The toast
  // stays non-technical: the numbers are bytes, which read as
  // characters for plain Latin text.
  const textBytes = new TextEncoder().encode(text).length;
  if (textBytes > state.ui.composeMaxLength) {
    toast('Message too long. ' + textBytes + '/' + state.ui.composeMaxLength,
          'error', 6000);
    return;
  }
  // Re-entry / not-ready guard: the button is :disabled while busy, but guard
  // here too (Enter-to-send bypasses the disabled attribute) so we never
  // double-send or send an attachment whose bytes aren't prepared yet.
  if (sendBtnBusy()) return;
  // Snapshot the target peer and the staged attachments before any await,
  // then detach the attachments from the compose tray. This whole function
  // is async (attachment upload + the /send round-trip can take a second or
  // two), and the user can switch chats meanwhile. Re-reading
  // state.openPeer after an await would file the optimistic bubble into the
  // newly-opened chat (so it wrongly appears in two conversations), and
  // leaving the staging in the shared state.pendingAttachments would let the
  // chat switch drop it out from under this send (and toast a false
  // "dropped from previous chat"). Once you hit send the attachments are no
  // longer "being composed", so they leave the tray now; everything below
  // works off these snapshots, not the live state.
  const peer    = state.openPeer;
  const pending = state.pendingAttachments;
  // Telemetry rides the same snapshot rules as attachments: detach
  // from the compose state now so a chat switch mid-send can't aim it
  // at the wrong peer.
  const telemetry = telemetryAttachActive() ? state.forms.compose.telemetry : null;
  state.forms.compose.telemetry = null;
  state.popovers.telemetry = false;
  state.pendingAttachments = [];
  renderAttachTray();
  state.busyButtons['btn-send'] = true;
  try {
    // Upload each pending attachment's bytes ahead of the JSON /send call.
    // Skip ones that already carry a staging_id from a prior partial-send
    // attempt.
    for (const a of pending) {
      if (a.staging_id) continue;
      a.uploading = true;
      try {
        const r = await state.transport.uploadAttachment(
          state.identityId, a.bytes, a.filename, a.mime,
          (loaded, totalBytes) => {
            a.queued = false;
            a.uploaded_bytes = loaded;
            a.uploaded_total = totalBytes;
          },
          () => { a.queued = true; });
        a.staging_id     = r.staging_id;
        a.staging_backend = r.backend;
      } finally {
        a.uploading = false;
        a.queued = false;
      }
    }
    const attachments = pending.map(a => {
      const e = { tag: a.tag, staging_id: a.staging_id,
                  filename: a.filename, mime: a.mime };
      if (a.audio_mode !== undefined) e.audio_mode = a.audio_mode;
      if (a.ext) e.ext = a.ext;
      return e;
    });
    // /send returns the full server-authoritative shape of the new
    // outbox record: seq, status, ts, boot_epoch, received_ms,
    // packet_hash, and the persisted attachments (with hash-based
    // filenames). Plug those directly into the optimistic record so
    // it has identifiers matching what the firmware will broadcast
    // via subsequent outbox_status events.
    const r = await state.transport.send(state.identityId, peer, '', text, attachments, telemetry);
    // Prefer server-side attachments (hash filenames, persisted
    // backend, server-canonical display_name) so the bubble can
    // render an inline preview immediately. Fall back to local
    // metadata only if the server didn't persist any (e.g. the
    // identity has persist_outbound_attachments off).
    const optimisticAttachments = (Array.isArray(r.attachments) && r.attachments.length > 0)
      ? r.attachments.map(a => ({
          tag: a.tag, size: a.size, filename: a.filename,
          display_name: a.display_name, mime: a.mime, backend: a.backend,
        }))
      : pending.map(a => ({
          tag: a.tag, size: a.bytes.length, filename: a.filename, mime: a.mime,
        }));
    // Pre-cache a local preview blob for image attachments, keyed by the
    // optimistic record's filename, so the bubble shows the image the moment
    // it's sent, before the device has persisted it under a hash filename we
    // could fetch back. attachmentMode() treats a cached blob as previewable.
    optimisticAttachments.forEach((oa, i) => {
      const src = pending[i];
      if (oa && oa.tag === FIELD_IMAGE && oa.filename && src && src.bytes
          && !(oa.filename in state.attachmentBlobs)) {
        try {
          state.attachmentBlobs[oa.filename] = URL.createObjectURL(
            new Blob([src.bytes], { type: oa.mime || src.mime || 'application/octet-stream' }));
        } catch (e) { /* preview is best-effort; fall back to the fetched copy */ }
      }
    });
    upsertMessage({
      peer: peer,
      seq: r.queued_seq,
      // `??` only catches null/undefined; a finding-route record can carry
      // ts=0, which would sort the bubble to the top of the thread (out of
      // view) instead of the bottom. Treat 0/missing as "now".
      ts: (r.ts && r.ts > 0) ? r.ts : (Date.now() / 1000),
      boot_epoch:  r.boot_epoch  || 0,
      received_ms: r.received_ms || 0,
      title: '',
      body: text,
      in: false,
      sig_ok: true,
      status: r.status || 'sent',
      packet_hash: r.packet_hash || '',
      attachments: optimisticAttachments,
      ...(r.tel ? { tel: true } : {}),
      ...(r.tele ? { tele: r.tele } : {}),
    });
    // A telemetry send may have started/replaced/ended a live share.
    if (telemetry) refreshTelemetryShares();
    // Clear the compose text only if the user is still on the chat we sent
    // from; if they switched, the new chat owns the compose box now.
    if (state.openPeer === peer) {
      state.forms.compose.text = '';
      state.ui.composeCounterShown = false;
    }
    renderThread();
    renderConversations();
  } catch (e) {
    toast('Send failed: ' + (e.message || e), 'error');
    // Re-stage the attachments so the user can retry, unless they've moved
    // to another chat (don't clobber its compose) or already staged
    // something new here.
    if (state.openPeer === peer && state.pendingAttachments.length === 0) {
      state.pendingAttachments = pending;
      renderAttachTray();
    }
    if (telemetry && state.openPeer === peer && !state.forms.compose.telemetry) {
      state.forms.compose.telemetry = telemetry;
    }
  } finally {
    state.busyButtons['btn-send'] = false;
  }
}

// Manual retry of a Failed outbox message - calls POST .../outbox/{seq}/retry
// on the device, which resets the auto-retry budget and re-schedules
// the send. Flip the message back to "queued" optimistically so the
// bubble updates immediately; the device's outbox-status SSE event
// will eventually confirm Delivered / Failed.
async function retryFailedMessage(msg) {
  if (!msg || msg.in) return;
  const seq = msg.seq;
  if (!Number.isFinite(seq)) return;
  const key = 'retry-' + seq;
  state.busyButtons[key] = true;
  try {
    await state.transport.retryOutbox(state.identityId, seq);
    msg.status = 'queued';
    renderThread();
    renderConversations();
    toast('Retry scheduled', 'info');
  } catch (e) {
    const msgText = (e && e.message) ? e.message : String(e);
    toast('Retry failed: ' + msgText, 'error');
  } finally {
    state.busyButtons[key] = false;
  }
}

// ============================== ATTACHMENTS ==============================
// LXMF fields-dict tag numbers (canonical, see LXMF::FIELD_* in
// LXMFTypes.h). Per-attachment cap is the device-reported outbound
// staging max - PSRAM (~4 MB) or SD card free space, whichever
// backend the device chose. Falls back to a conservative 64 KB when
// /api/info hasn't reported caps yet.
const FIELD_FILE_ATTACHMENTS = 0x05;
const FIELD_IMAGE            = 0x06;
const FIELD_AUDIO            = 0x07;
const ATTACH_FALLBACK_BYTES  = 64 * 1024;
function attachMaxBytes() {
  // The attachment cap is a device property (it changes only on SD
  // insert/eject), sourced over plain HTTP from /api/storage/config and
  // refreshed on demand when the composer adds an attachment. It is NOT taken
  // from the WebSocket: a stateful channel that can drop frames under a
  // connection burst is the wrong place for a hard limit. ATTACH_FALLBACK_BYTES
  // applies only before the first storage-config fetch has resolved.
  const sc = state._storageConfig;
  return (sc && Number.isFinite(sc.effective_max_send_bytes) && sc.effective_max_send_bytes > 0)
       ? sc.effective_max_send_bytes : ATTACH_FALLBACK_BYTES;
}
function attachBackendLabel() {
  const c = state.outboundCaps;
  if (!c) return null;
  if (c.backend === 'sd') return 'SD card';
  if (c.backend === 'psram') return 'PSRAM';
  return null;
}

// Resize an image File to a target max dimension, returning a JPEG
// Blob. Quality 0.78 keeps small/medium variants well under the cap
// while still looking acceptable for chat-app use.
async function resizeImage(file, maxDim) {
  const bmp = await createImageBitmap(file);
  let w = bmp.width, h = bmp.height;
  if (maxDim && Math.max(w, h) > maxDim) {
    const k = maxDim / Math.max(w, h);
    w = Math.round(w * k); h = Math.round(h * k);
  }
  // Reuse the static offscreen #resize-canvas. Resetting width/height
  // also clears the bitmap data so the previous resize doesn't bleed
  // into this one.
  const canvas = $('resize-canvas');
  canvas.width = w; canvas.height = h;
  canvas.getContext('2d').drawImage(bmp, 0, 0, w, h);
  bmp.close();
  return await new Promise(res => canvas.toBlob(res, 'image/jpeg', 0.78));
}

async function blobToBytes(blob) {
  const buf = await blob.arrayBuffer();
  return new Uint8Array(buf);
}

// Read an image blob's natural pixel dimensions without rendering it.
// Used by the resize picker to choose a thumbnail display size that
// matches the smallest variant's actual pixel count.
async function blobImageDimensions(blob) {
  const bmp = await createImageBitmap(blob);
  const w = bmp.width, h = bmp.height;
  bmp.close();
  return { w, h };
}

// Returns just the duration string (e.g. "≈2.4 s") or null when the
// device couldn't compute one (no path, local destination, etc).
// Callers prefix "ETA: " themselves and pick their own unknown-state
// text - keeps display copy consistent everywhere.
function formatEta(ms) {
  if (ms == null) return null;
  if (ms < 1000) return '~' + ms + ' ms';
  const s = ms / 1000;
  if (s < 60) return '≈' + s.toFixed(1) + ' s';
  return '≈' + Math.round(s / 60) + ' min';
}

// The attach button's @click in the markup opens the file picker
// directly via $refs.attachFile.click(); image files take a detour
// through the resize modal (handled by onChangeAttachFile), everything
// else stages immediately. No imperative wrapper needed here.


// ----- audio capture -----
// getUserMedia is blocked on plain HTTP except localhost - browsers
// require a "secure context". When the user accesses the device at
// http://192.168.x.x/ the call will fail; we surface a clear toast
// rather than a cryptic permission error.
function pickMediaRecorderMime() {
  // Most browsers support at least one of these; Opus-in-Ogg is
  // smallest for voice but Chrome only emits Opus-in-WebM. Either
  // sniffer handles on the receive side (#94).
  const candidates = [
    'audio/ogg;codecs=opus',
    'audio/webm;codecs=opus',
    'audio/webm',
    'audio/mp4',
  ];
  for (const m of candidates) {
    if (typeof MediaRecorder !== 'undefined' && MediaRecorder.isTypeSupported(m)) return m;
  }
  return '';
}

async function startRecording() {
  if (!state.openPeer) {
    toast('Open a chat first.', 'warn');
    return;
  }
  if (state.recorder) return;  // already recording
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    toast('Microphone is not available. Try a different browser, or HTTPS.', 'error', 6000);
    return;
  }
  let stream;
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: true });
  } catch (e) {
    // The classic case here is the secure-context block on
    // http://192.168.x.x - pin that error explicitly so the user
    // knows what to do.
    if (!window.isSecureContext) {
      toast('Mic blocked: browsers only allow recording over HTTPS or on localhost. Try installing the SPA as a PWA, or connect to the device via localhost.', 'error', 9000);
    } else {
      toast('Mic permission denied: ' + (e.message || e), 'error', 6000);
    }
    return;
  }
  const mime = pickMediaRecorderMime();
  let rec;
  try {
    rec = new MediaRecorder(stream, mime
        ? { mimeType: mime, audioBitsPerSecond: 24000 }   // ~3 KB/s, room for ~16 s under the 48 KB cap
        : { audioBitsPerSecond: 24000 });
  } catch (e) {
    stream.getTracks().forEach(t => t.stop());
    toast('Could not start recorder: ' + (e.message || e), 'error');
    return;
  }
  state.recorder = rec;
  state.recorderChunks = [];
  state.recorderStartMs = Date.now();
  state.recorderStopReason = '';
  rec.ondataavailable = (ev) => { if (ev.data && ev.data.size > 0) state.recorderChunks.push(ev.data); };
  rec.onstop = async () => {
    // Free the mic immediately.
    stream.getTracks().forEach(t => t.stop());
    const blob = new Blob(state.recorderChunks, { type: mime || 'audio/webm' });
    state.recorder = null;
    state.recorderChunks = [];
    state.ui.micRecording = false;
    renderAttachTray();
    if (state.recorderStopReason === 'cancel' || blob.size === 0) return;
    const cap = attachMaxBytes();
    if (blob.size > cap) {
      toast('Recording too large (' + formatBytes(blob.size)
            + '). Cap is ' + formatBytes(cap) + '.', 'error', 6000);
      return;
    }
    const bytes = await blobToBytes(blob);
    const ext = (mime.includes('ogg') ? 'ogg'
               : mime.includes('mp4') ? 'm4a' : 'webm');
    await stagePendingAttachment({
      tag:      FIELD_AUDIO,
      bytes,
      filename: 'voice-' + new Date().toISOString().replace(/[:.]/g, '-') + '.' + ext,
      mime:     mime || 'audio/webm',
    });
  };
  rec.start();
  state.ui.micRecording = true;
  renderAttachTray();   // surfaces the live "Recording 0:03 [Stop] [Cancel]" chip
}

function stopRecording(save) {
  if (!state.recorder) return;
  state.recorderStopReason = save ? 'save' : 'cancel';
  state.recorder.stop();
}

function onClickMic() {
  if (state.recorder) stopRecording(true);
  else                startRecording();
}
// Bound via @change on the #attach-file file picker. Dispatches by mime:
// images go through the in-browser resize picker; everything else is
// staged at native size up to the device-reported attachment cap.
async function onChangeAttachFile(ev) {
  const file = ev.target.files && ev.target.files[0];
  if (!file) return;
  // Refresh the cap in the background (it changes only on SD insert/eject) -
  // never block the attach on it. On the SRAM-tight device an HTTP request can
  // stall for seconds, and awaiting it here froze the composer outright. This
  // attach validates against the last-known cap (loaded at boot by
  // populateStorageConfig); the refresh updates it for the next attach.
  refreshStorageCaps().catch(() => {});
  if (file.type.startsWith('image/')) {
    await openImageResizePicker(file);
  } else {
    await stageNonImageAttachment(file);
  }
  // Reset the input so picking the same file twice in a row still fires
  // change; otherwise the browser dedupes and the second pick is silent.
  ev.target.value = '';
}

async function stageNonImageAttachment(file) {
  const cap = attachMaxBytes();
  if (file.size > cap) {
    toast('File is ' + formatBytes(file.size) + '. Cap is '
          + formatBytes(cap) + ' per attachment.', 'error', 6000);
    return;
  }
  const tag = file.type.startsWith('audio/') ? FIELD_AUDIO : FIELD_FILE_ATTACHMENTS;
  // Stage the chip instantly; read the file bytes in the background.
  await stagePendingAttachment({
    tag, bytesPromise: blobToBytes(file),
    filename: file.name, mime: file.type || 'application/octet-stream',
  });
}

// Full-screen image viewer.
//
// Three interaction zones with distinct behaviours:
//   - Image, when fit-to-viewport AND natural size > viewport:
//       cursor zoom-in, click → switches to 1:1, recentred.
//   - Image, when zoomed 1:1: cursor zoom-out, click → back to fit.
//     While zoomed: mouse-drag pans the overflowing image (touch
//     scroll already pans natively via overflow:auto).
//   - Image, when natural already fits: cursor default, click is
//     a no-op (the user can't zoom into something already at 1:1).
//   - Backdrop (anywhere inside the modal that ISN'T the image
//     itself, the close button, or the save button): click closes.
//   - Close X (top right, always above the image) + Escape: close.
//
// Conditional zoom (only when there's more detail to see than the
// fit-to-viewport pass shows) rather than click-anywhere-closes, so
// desktop users can inspect detail without leaving the modal.
function openImageModal(url, meta) {
  const modal = $('modal-image');
  const img   = $('mi-img');
  const wrap  = $('mi-wrap');
  const save  = $('mi-save');
  const close = $('mi-close');
  // Reset to fit-mode on each open.
  img.src = url;
  img.style.maxWidth  = '100vw';
  img.style.maxHeight = '100dvh';
  img.style.width  = '';
  img.style.height = '';
  img.style.cursor = 'default';      // overridden on load if zoom is available
  // Save-as filename: prefer sender display_name, fall back to hash.
  const dl = (meta && meta.display_name) || (meta && meta.filename) || 'image';
  save.setAttribute('download', dl);
  save.href = url;
  state.modals.image = true;

  let zoomed = false;          // true: image rendered at natural 1:1
  let zoomAvailable = false;   // set on load; image natural > viewport
  let drag = null;             // {startX, startY, startSL, startST} while pan-dragging

  const computeZoomAvailable = () => {
    // Compare the image's natural pixels to the wrap's viewport.
    // wrap is overflow:auto so its clientWidth/Height is the
    // visible area after any scrollbar reservation.
    return img.naturalWidth  > wrap.clientWidth
        || img.naturalHeight > wrap.clientHeight;
  };
  const applyFit = () => {
    zoomed = false;
    img.style.maxWidth  = '100vw';
    img.style.maxHeight = '100dvh';
    img.style.width  = '';
    img.style.height = '';
    img.style.cursor = zoomAvailable ? 'zoom-in' : 'default';
  };
  const applyZoom = () => {
    zoomed = true;
    img.style.maxWidth  = 'none';
    img.style.maxHeight = 'none';
    img.style.width  = img.naturalWidth + 'px';
    img.style.height = img.naturalHeight + 'px';
    img.style.cursor = 'zoom-out';
    // Recentre the scroll viewport on the image's geometric centre.
    // After style change the browser lays out synchronously; scrollTo
    // operates on the new bounds.
    requestAnimationFrame(() => {
      wrap.scrollLeft = (img.naturalWidth  - wrap.clientWidth)  / 2;
      wrap.scrollTop  = (img.naturalHeight - wrap.clientHeight) / 2;
    });
  };
  const onImgLoad = () => {
    zoomAvailable = computeZoomAvailable();
    img.style.cursor = zoomAvailable ? 'zoom-in' : 'default';
  };
  if (img.complete && img.naturalWidth > 0) {
    onImgLoad();
  } else {
    img.addEventListener('load', onImgLoad, { once: true });
  }

  const onImgClick = (e) => {
    e.stopPropagation();         // don't bubble to onBackdrop and close
    if (drag) return;            // a drag just completed - ignore click
    if (!zoomAvailable) return;  // image already fits naturally, no zoom
    if (zoomed) applyFit(); else applyZoom();
  };

  // Mouse drag-to-pan while zoomed. Cursor flips to grabbing for
  // visual feedback. Touch pan is already free via overflow:auto.
  const onMouseDown = (e) => {
    if (!zoomed) return;
    if (e.button !== 0) return;  // only primary button
    drag = {
      startX: e.clientX, startY: e.clientY,
      startSL: wrap.scrollLeft, startST: wrap.scrollTop,
      moved: false,
    };
    img.style.cursor = 'grabbing';
    e.preventDefault();
  };
  const onMouseMove = (e) => {
    if (!drag) return;
    const dx = e.clientX - drag.startX;
    const dy = e.clientY - drag.startY;
    if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true;
    wrap.scrollLeft = drag.startSL - dx;
    wrap.scrollTop  = drag.startST - dy;
  };
  const onMouseUp = () => {
    if (!drag) return;
    const wasMoved = drag.moved;
    drag = null;
    img.style.cursor = zoomed ? 'zoom-out' : 'zoom-in';
    // If the user actually dragged (vs a click that registered tiny
    // movement), suppress the immediate click handler.
    if (wasMoved) {
      img.addEventListener('click', swallowOnce, { once: true, capture: true });
    }
  };
  const swallowOnce = (e) => { e.stopPropagation(); e.preventDefault(); };

  const onBackdrop = (e) => {
    // Backdrop = anywhere inside the modal that isn't the image, the
    // close button, or the save button.
    if (e.target === img) return;
    if (close.contains(e.target)) return;
    if (save.contains(e.target))  return;
    cleanup();
  };
  const onKey = (e) => { if (e.key === 'Escape') cleanup(); };
  const cleanup = () => {
    state.modals.image = false;
    img.removeEventListener('click', onImgClick);
    img.removeEventListener('mousedown', onMouseDown);
    window.removeEventListener('mousemove', onMouseMove);
    window.removeEventListener('mouseup',   onMouseUp);
    modal.removeEventListener('click', onBackdrop);
    close.removeEventListener('click', cleanup);
    document.removeEventListener('keydown', onKey);
    img.src = '';   // release reference; cached blob URL stays in state.attachmentBlobs
  };
  img.addEventListener('click', onImgClick);
  img.addEventListener('mousedown', onMouseDown);
  window.addEventListener('mousemove', onMouseMove);
  window.addEventListener('mouseup',   onMouseUp);
  modal.addEventListener('click', onBackdrop);
  close.addEventListener('click', cleanup);
  document.addEventListener('keydown', onKey);
}

async function openImageResizePicker(file) {
  const cap = attachMaxBytes();
  const backendLabel = attachBackendLabel();
  const sourceLine = file.name + ' · ' + formatBytes(file.size)
        + (backendLabel ? ('  ·  staging: ' + backendLabel + ' (' + formatBytes(cap) + ')') : '');

  // Open the modal immediately in a "preparing" state, then encode the size
  // variants in the background. "Resize" here means downscaling the image's
  // pixel dimensions (which proportionally shrinks the encoded JPEG);
  // "original" sends the user's file bytes as-is. Each client-side re-encode
  // takes a beat on a phone, so doing all three up-front froze the picker until
  // they finished - now each variant appears as it's ready.
  Object.assign(state.imageResize, {
    sourceLine, cap, thumbBox: 200, prepared: [], chosen: null,
    etaByKey: {}, _file: file, preparing: true,
  });
  state.modals.imageResize = true;

  const candidates = [
    { key: 'small',    label: 'Small',    maxDim: 320 },
    { key: 'medium',   label: 'Medium',   maxDim: 800 },
    { key: 'original', label: 'Original', maxDim: 0   },
  ];
  for (const c of candidates) {
    // Bail if the user closed the picker (or started a different attach) while
    // we were encoding - revoke any URLs created for this aborted run.
    if (!state.modals.imageResize || state.imageResize._file !== file) {
      for (const p of state.imageResize.prepared) URL.revokeObjectURL(p.objectUrl);
      return;
    }
    const blob = c.key === 'original' ? file : await resizeImage(file, c.maxDim);
    const dims = await blobImageDimensions(blob);
    const variant = { ...c, blob, size: blob.size, dims,
                      objectUrl: URL.createObjectURL(blob) };
    state.imageResize.prepared.push(variant);
    // Show every thumbnail at the small variant's natural dimensions (capped at
    // 200 px) so the options compare at the same on-screen scale.
    if (c.key === 'small') {
      state.imageResize.thumbBox = Math.min(200, Math.max(dims.w, dims.h));
    }
    // Default selection: largest prepared-so-far that still fits the cap.
    const fit = [...state.imageResize.prepared].reverse().find(p => p.size <= cap);
    state.imageResize.chosen = fit || state.imageResize.prepared[0];
  }
  state.imageResize.preparing = false;

  // Fetch per-variant ETAs in the background; the template observes
  // etaByKey via x-text and shows them as they arrive.
  (async () => {
    const peer = state.openPeer;
    if (!peer) return;
    for (const p of state.imageResize.prepared) {
      if (p.size > cap) continue;
      try {
        const r = await state.transport.pathEstimate(peer, p.size + 120);
        let line = '';
        if (r.kind === 'local')        line = 'ETA: instant (same device)';
        else if (r.kind === 'unknown') line = 'ETA: unknown route';
        else {
          const e = formatEta(r.eta_ms);
          line = e ? ('ETA: ' + e) : 'ETA: pending path estimate';
        }
        state.imageResize.etaByKey[p.key] = line;
      } catch (_) { /* leave blank */ }
    }
  })();
}

// Card style binding - outlines + background match selection state,
// dim + not-allowed for over-cap options.
function imageResizeCardStyle(p) {
  const ir = state.imageResize;
  const tooBig   = p.size > ir.cap;
  const isChosen = ir.chosen && p.key === ir.chosen.key && !tooBig;
  return 'display:flex;gap:12px;padding:8px;border-radius:6px;'
       + 'border:2px solid ' + (isChosen ? 'var(--accent)' : 'var(--bg-elev-2)') + ';'
       + 'background:' + (isChosen ? 'rgba(255,255,255,0.04)' : 'transparent') + ';'
       + 'cursor:' + (tooBig ? 'not-allowed' : 'pointer') + ';'
       + 'opacity:' + (tooBig ? 0.5 : 1) + ';'
       + 'align-items:flex-start';
}

// Selection click - accept only when the variant fits the cap.
function imageResizePick(p) {
  if (p.size > state.imageResize.cap) return;
  state.imageResize.chosen = p;
}

// Cancel button + backdrop click handler.
function imageResizeCancel() {
  for (const p of (state.imageResize.prepared || [])) URL.revokeObjectURL(p.objectUrl);
  state.imageResize.prepared = [];
  state.imageResize.chosen = null;
  state.imageResize._file = null;
  state.imageResize.preparing = false;
  state.modals.imageResize = false;
}

// Attach button - stage the chosen variant and close.
async function imageResizeAttach(_btn) {
  const ir = state.imageResize;
  const c = ir.chosen;
  if (!c) return;
  const file = ir._file;
  const blob = c.blob;
  const filename = (file.name || 'image').replace(/\.[^.]+$/, '')
                  + (c.key === 'original' ? '' : '_' + c.key)
                  + '.jpg';
  const mime = c.key === 'original' ? (file.type || 'image/jpeg') : 'image/jpeg';
  // Close (revokes the thumbnail object URLs) BEFORE staging so the modal
  // disappears promptly; the chosen blob is still readable. Read its bytes in
  // the background so the tray chip appears instantly.
  imageResizeCancel();
  await stagePendingAttachment({
    tag: FIELD_IMAGE, bytesPromise: blobToBytes(blob), filename, mime,
  });
}

async function stagePendingAttachment(att) {
  // Enforce the server's per-message attachment cap up front so the
  // user can't queue up a send that's guaranteed to be rejected.
  const limits = (state.info && state.info.limits) || {};
  const maxAtt = limits.max_attachments || 10;
  if (state.pendingAttachments.length >= maxAtt) {
    toast(`Max ${maxAtt} attachments per message`, 'warn');
    return;
  }
  const maxName = limits.max_attachment_name_bytes || 256;
  const maxMime = limits.max_attachment_mime_bytes || 128;
  if (att.filename && att.filename.length > maxName) {
    att.filename = att.filename.slice(0, maxName);
  }
  if (att.mime && att.mime.length > maxMime) {
    att.mime = att.mime.slice(0, maxMime);
  }
  // Show the chip the instant the user attaches. For callers that pass an async
  // `bytesPromise`, the bytes resolve in the background; the path-ETA estimate
  // (an HTTP round-trip to the SRAM-tight device) always runs in the background.
  // `processing` keeps the send button spinning + disabled until the bytes land.
  // Keep all caller fields (tag, ext, audio_mode, …) but never store the
  // transient bytesPromise on the chip.
  const { bytesPromise, bytes, ...rest } = att;
  const arr = state.pendingAttachments;
  arr.push({
    ...rest,
    bytes: bytes || null,
    eta_ms: null, kind: null,
    processing: !bytes,
  });
  const entry = arr[arr.length - 1];   // reactive proxy
  (async () => {
    try {
      if (!entry.bytes && bytesPromise) entry.bytes = await bytesPromise;
    } catch (e) {
      toast('Attachment failed: ' + (e.message || e), 'error');
      const i = state.pendingAttachments.indexOf(entry);
      if (i >= 0) state.pendingAttachments.splice(i, 1);
      return;
    } finally {
      entry.processing = false;
    }
    if (state.openPeer && entry.bytes) {
      try {
        const r = await state.transport.pathEstimate(state.openPeer, entry.bytes.length + 120);
        entry.eta_ms = r.eta_ms;
        entry.kind   = r.kind;
      } catch (_) {}
    }
  })();
}

// Composed ETA / upload-progress / "no route" string for an attachment
// chip. Called from the x-text on each chip's .eta span.
function attachEtaText(a) {
  if (a.processing) return 'preparing…';
  if (a.queued) return 'waiting…';
  if (a.uploading) {
    const denom = a.uploaded_total || a.bytes.length;
    const pct = denom > 0
      ? Math.max(0, Math.min(100, Math.round(100 * (a.uploaded_bytes || 0) / denom)))
      : 0;
    return 'uploading ' + pct + '%';
  }
  if (a.staging_id) return 'uploaded';
  if (a.kind === 'local')   return 'instant';
  if (a.kind === 'unknown') return 'no route';
  return formatEta(a.eta_ms) || 'sizing…';
}

// Reactive data for the #attach-tray below the compose textarea.
// Visibility tracks pending attachments OR active recording. While
// recording, a 250ms ticker updates recordElapsedText.
function attachTrayData() {
  return {
    nowMs: Date.now(),
    _tick: null,
    init() {
      // Lazy elapsed-time tick - runs only while recording. Watching
      // $store.s.recorder lets us start/stop the interval reactively.
      this.$watch(() => !!this.$store.s.recorder, (rec) => {
        if (rec) {
          if (!this._tick) this._tick = setInterval(() => { this.nowMs = Date.now(); }, 250);
        } else if (this._tick) {
          clearInterval(this._tick); this._tick = null;
        }
      });
    },
    get visible()   { return this.recording || (this.$store.s.pendingAttachments && this.$store.s.pendingAttachments.length > 0) || telemetryAttachActive(); },
    get recording() { return !!this.$store.s.recorder; },
    get recordElapsedText() {
      const ms = this.nowMs - (this.$store.s.recorderStartMs || this.nowMs);
      const s  = Math.floor(ms / 1000);
      const mm = Math.floor(s / 60);
      const ss = (s % 60).toString().padStart(2, '0');
      return mm + ':' + ss + ' (recording)';
    },
  };
}

// Backwards-compat shim - pendingAttachments / recorder are reactive
// now, so callers no longer need to re-render. Stub so existing call
// sites don't trip.
function renderAttachTray() { /* no-op */ }

// Shared dismiss predicate for topbar popovers. Returns true when the
// click should close the popover - i.e. it landed outside both the
// popover content AND any button that should KEEP the popover open.
//
// "Keep open" list:
//   - the popover's own anchor button (handles toggle-to-close itself)
//   - the announce icon (momentary action; users want the popover to
//     remain so they can confirm the announce went out without losing
//     context)
//
// Sibling popover anchors (e.g. btn-system while popover-status is
// open) are NOT in the keep-open list - clicking them legitimately
// wants to swap which popover is visible.
function popoverDismissibleClick(ev, popoverId, anchorBtnId) {
  const pop    = document.getElementById(popoverId);
  const anchor = document.getElementById(anchorBtnId);
  const ann    = document.getElementById('btn-announce-top');
  if (pop    && pop.contains(ev.target))    return false;
  if (anchor && (ev.target === anchor || anchor.contains(ev.target))) return false;
  if (ann    && (ev.target === ann    || ann.contains(ev.target)))    return false;
  return true;
}

async function forceAnnounce() {
  state.busyButtons['btn-announce-top'] = true;
  try {
    await state.transport.announceNow(state.identityId);
    toast('Announce sent', 'success');
    // Flip the dot to yellow ("sent this boot") for the case where
    // auto-announce is off; refreshAnnounceCountdown picks this up.
    state.announcedThisBoot = true;
    // Optimistically reset the countdown to a full interval - server
    // does the same on its end after a successful send_announce.
    const ms = Number(state.self && state.self.announce_interval_ms) || 0;
    seedAnnounceCountdown(ms, ms);
  } catch (e) {
    toast('Announce failed: ' + (e.message || e), 'error');
  } finally {
    state.busyButtons['btn-announce-top'] = false;
  }
}

// Apply firmware-published trust-boundary caps to the compose UI. The
// firmware exports its enforced limits via /api/info.limits; mirror
// them onto input attributes so the browser blocks input past the
// cap (matching the firmware's accept/reject behaviour) and surface
// a counter to the user.
function applyServerLimits(limits) {
  if (limits.max_body_bytes) {
    state.ui.composeMaxLength = limits.max_body_bytes;
  }
  // attachment count / name / mime are enforced inside
  // stagePendingAttachment() at queue time.
}

