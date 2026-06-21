// Device API paths, generated at build time from src/Web/api_routes.def
// (the single source of truth). Param routes are functions taking one
// argument per path segment: API.CONVERSATION(peer), API.OUTBOX_RETRY(seq).
// Never write an /api path literal in this file; the parity check fails
// the build on one.
const API = {
  __API_ROUTES__
};
'use strict';

// ============================== ALPINE STORES ==============================
// Bridge the legacy `state` object to Alpine.
//
// alpine:init fires once, before any x-data block evaluates. We:
//   1. Replace `state` with an Alpine.reactive() proxy over the same
//      initial values. Because `state` is declared with `let`, every
//      closure that reads `state.foo` automatically sees the new
//      reactive binding (let captures the binding, not the value).
//   2. Register the same reactive object under Alpine.store('s') so
//      templates can use `$store.s.foo` directly. The store and
//      `state` are the same proxy; mutating either path triggers
//      reactivity.
//   3. Register a separate `net` store for connection state - kept
//      out of the main state object because the WS code mutates it
//      from below the Alpine boundary (timer callbacks etc.) and
//      having it isolated makes the network-down condition trivial
//      to test in templates.
document.addEventListener('alpine:init', () => {
  // Seed view to whatever was already shown at boot (the imperative
  // bootstrap path may have flipped one before Alpine init runs).
  // Re-evaluated as state.view changes drive all top-level x-show.
  state.view = state.view || (() => {
    for (const v of ['view-loading', 'view-bootstrap', 'view-identities', 'view-create', 'view-login', 'view-app']) {
      const el = document.getElementById(v);
      if (el && !el.hidden) return v;
    }
    return 'view-loading';
  })();
  state = Alpine.reactive(state);
  window.state = state;            // expose for console debugging
  setInterval(() => { state.ui.nowS = Math.floor(Date.now() / 1000); }, 1000);
  Alpine.store('s',   state);
  Alpine.store('net', { connected: true });
  Alpine.store('toasts', {
    items: [],
    _seq: 1,
    push(message, kind = 'info', durationMs) {
      if (!message) return;
      if (durationMs == null) {
        durationMs = (kind === 'error') ? 7000 : (kind === 'success') ? 3000 : 4000;
      }
      // Split "Title: body" into separate lines - same heuristic as
      // the old imperative path.
      let title = message, body = '';
      const idx = message.indexOf(':');
      if (idx > 0 && idx < 32 && message.length > idx + 2) {
        title = message.slice(0, idx);
        body  = message.slice(idx + 1).trim();
      }
      const id = this._seq++;
      this.items.push({ id, kind, title, body, dismissing: false });
      setTimeout(() => this.dismiss(id), durationMs);
    },
    dismiss(id) {
      const t = this.items.find(x => x.id === id);
      if (!t || t.dismissing) return;
      // Set the flag → CSS .dismissing animates out → remove after
      // the 200 ms transition. Matches the legacy timing.
      t.dismissing = true;
      setTimeout(() => {
        const i = this.items.findIndex(x => x.id === id);
        if (i >= 0) this.items.splice(i, 1);
      }, 200);
    },
  });
});

// ============================== STATE ==============================
const LS = {
  TOKEN:    'urlxmf.token',
  IDENTITY: 'urlxmf.identity',
  TRANSPORT:'urlxmf.transport',
  CONTACTS: 'urlxmf.contacts',
  PREFS:    'urlxmf.prefs',
};
const DEFAULT_PREFS = {
  enterSends: true,           // true: Enter sends, Shift+Enter newline. false: opposite.
  autoDownloadAttachments: false,  // when off, inbound images/audio render
                                   // as click-to-load chips instead of
                                   // auto-fetching from the device.
};
function loadPrefs() {
  try { return Object.assign({}, DEFAULT_PREFS, JSON.parse(localStorage.getItem(LS.PREFS) || '{}')); }
  catch { return Object.assign({}, DEFAULT_PREFS); }
}
function savePrefs(p) {
  state.prefs = Object.assign({}, state.prefs, p);
  localStorage.setItem(LS.PREFS, JSON.stringify(state.prefs));
}
// `let`, not `const`, so the alpine:init bridge below can promote it
// to an Alpine.reactive() wrapper. Closures see the latest binding
// (let semantics), so all existing `state.foo` reads / writes start
// flowing through the reactive proxy automatically once Alpine boots.
let state = {
  // Top-level view shown to the user. One of view-loading /
  // view-bootstrap / view-identities / view-create / view-login /
  // view-app. Drives x-show on each top-level page div. Mutated by
  // show(viewId). Starts on view-loading so no real screen flashes
  // before boot()'s first /api/info resolves and routes to the
  // correct view.
  view: 'view-loading',
  // Per-modal visibility flags. Each maps 1:1 to a `<div
  // class="modal-bg">` and is wired via x-show in markup.
  // Stacked modals (image / id-code / confirm on top of settings)
  // each have their own flag so multiple can be open at once.
  modals: {
    newConv:        false,
    imageResize:    false,
    confirm:        false,
    wifiAdd:        false,
    tcpAdd:         false,
    image:          false,
    convRetention:  false,
    idCode:         false,
    settings:       false,
  },
  // The full-screen map view (top-bar icon / bubble preview open it).
  mapView: { open: false, gpx: null, loading: false },
  // Map source, mirrored from /api/map/config. mode is off|sd; the vector
  // layers on the card (world base + detail areas) are listed separately.
  mapTiles: { mode: 'sd', sdPresent: false, layers: [], defaultZoom: 16 },
  // Polygon draw-to-download state for the map view.
  mapDraw: { on: false, pts: [], name: '' },
  // Device-side map download job, mirrored from /api/map/download while a
  // download runs. phase is idle|running|done|error|cancelled.
  mapDl: { phase: 'idle', active: false, written: 0, total: -1, url: '', dest: '', error: '' },
  // Device-side region extract from the planet, mirrored from
  // /api/map/extract. phase is idle|scanning|writing|done|error|cancelled.
  mapExt: { phase: 'idle', active: false, tiles: 0, bytesDone: 0, bytesTotal: 0, dest: '', error: '' },
  // Per-popover visibility flags. Same pattern as modals.
  popovers: {
    status:    false,
    system:    false,
    power:     false,
    emoji:     false,
    telemetry: false,
  },
  // Active tab in the Settings modal. Drives :class="{ active: ... }"
  // on every #settings-tabs > .tab and #modal-settings .tab-panel.
  settingsTab: 'identity',
  // TCP-clients list as fetched from /api/transport/tcp_clients. null
  // means "loading" (initial state). Empty array = loaded + none.
  tcpClients: null,
  tcpClientsCap:  0,
  tcpClientsLive: 0,
  // Saved WiFi networks. null = loading.
  wifiSaved: null,
  // Row-menu popover state. null = closed; { items, x, y } when open.
  // items: [{ label, fn, destructive? }, ...]
  rowMenu: null,
  // Active live-telemetry shares for the session identity. grants =
  // peers we answer; feeds = peers we poll (with latest readings).
  telemetryShares: { grants: [], feeds: [], fetchedAt: 0 },
  // Time-source rows in priority order. Each: { key, label, desc,
  // enabled, interval_s, hasInterval, expanded }.
  timeSources: [],
  // GPS is a normal sensor: its enable + location interval live in
  // state.system.sensors.gps (like the other sensors); its clock-sync
  // "Refresh" lives in the time-sources list. No dedicated store object.
  // Singleton info-tooltip state. null = closed. { html, x, y } open.
  infoTip: null,
  // Image-resize picker state. Populated by openImageResizePicker; the
  // modal's <template x-for> renders prepared variants and chosen
  // tracks the active selection.
  imageResize: {
    sourceLine: '', cap: 0, thumbBox: 200, prepared: [],
    chosen: null, etaByKey: {}, _resolve: null, _file: null,
    preparing: false,
  },
  // Scanned WiFi networks for the bootstrap + add-wifi-modal dropdowns.
  // null while a scan is in flight; an array (possibly empty) once done.
  // 'error' if the last scan failed. Both view templates bind to this.
  wifiScanList: [],
  wifiScanState: 'idle',   // 'idle' | 'scanning' | 'error'
  // Background-task registry. Keyed by an arbitrary string id. Mirrors
  // the legacy bgTasks Map but as a plain object so Alpine reactivity
  // tracks add / remove / update on the topbar progress strip.
  bgTasks: {},
  // Live retention-modal config. Populated by openRetentionModal().
  // Inputs x-model into this; retentionSave() reads it.
  retention: {
    defaultLabel: 'no expiry',
    mode: 'default',
    timeN: 7, timeUnit: '86400',
    countN: 200,
    identityId: '',
    peerHex: '',
    _resolve: null,
  },
  // Live confirm-modal config. Populated by showConfirm(opts) and
  // mutated by confirmOk / confirmCancel. Null when no confirm modal
  // is open. The _resolve fn isn't enumerated reactively but Alpine
  // doesn't care - it just lives on the object.
  confirm: {
    title: 'Confirm', body: '', okLabel: 'OK', destructive: false,
    needsCode: false, needsInput: false,
    inputLabel: '', inputValue: '', inputPlaceholder: '',
    codeValue: '',
    _resolve: null,
  },
  info: null,
  transport: null,
  token: localStorage.getItem(LS.TOKEN) || null,
  identityId: localStorage.getItem(LS.IDENTITY) || null,
  identities: [],
  self: null,                       // current identity object {id, display_name, address, ...}
  // Plain objects (not Maps) so Alpine 3's reactive proxy can observe
  // mutations - Alpine doesn't track Map/Set internals.
  conversations: {},                // peer_hex → {peer, display_name, last_ts, last_body, last_in, msgs:[]}
  openPeer: null,
  announces: {},                    // dest_hex → record
  paths: {},                        // dest_hex → record
  transfers: {},                    // link_hash → { peer, incoming, bytes_done, bytes_total }
  // System snapshot (storage / sensors / battery / outbound_caps / rtc).
  // Populated from the WS `hello` frame and refreshed by `system_update`
  // events every ~30s. Read-only state for popovers + the topbar icons.
  system: null,
  // Per-sensor live-poll state pushed by the device (sensor_live_state):
  // {environment:{live,ttl_ms}, magnetometer, imu, gps:{powered,mode}}.
  sensorLive: {},
  contacts: JSON.parse(localStorage.getItem(LS.CONTACTS) || '{}'),
  prefs: loadPrefs(),
  pendingIdCode: false,
  pendingIdCodeAction: null,        // function called with the 6-char code
  announcedThisBoot: false,         // local flag - yellow announce dot when
                                    // auto-announce is off but user has fired
                                    // at least one manual announce since SPA load
  pendingAttachments: [],           // [{ tag, bytes:Uint8Array, filename, mime, eta_ms }]
                                    // staged before send; cleared after send.
  gpxState: {},                     // gpx filename → 'loading'|'ready'|'failed'
  gpxThumb: {},                     // gpx filename → rendered thumbnail data URL
  attachmentBlobs: {},              // filename → object URL. Inbound image
                                    // chips auto-fetch on first render and
                                    // cache the result here so SSE-driven
                                    // rerenders don't refetch.
  recorder: null,                   // MediaRecorder while recording, else null
  recorderChunks: [],               // Blob chunks accumulated during record
  recorderStartMs: 0,
  recorderStopReason: '',           // 'save' | 'cancel'
  sdPresent: null,                  // null = unknown (no probe yet), bool once
                                    // /api/system_status responds. Attachment
                                    // chips with backend=="sd" surface a warn
                                    // pill when this flips false.
  outboundCaps: null,               // { max_bytes, backend, psram_free_bytes,
                                    //   sd_free_bytes?, sd_present } from /api/info.
                                    // Used to gate image-resize options + file
                                    // picker + recorder duration. (#130)
  reconnectStart: 0,
  skipBootstrap: false,             // set true by "skip" button on view-bootstrap so subsequent
                                    // routing doesn't bounce back into bootstrap mode
  // Radio telemetry ring. `samples` is oldest→newest. `cap` is the
  // server-side ring size echoed back in /api/radio/telemetry; we trust
  // it for trimming to keep client + firmware aligned. Each sample is
  // { ts, rssi, noise, util:{own, others}, cw, dcd, lock, rx, tx }.
  // `util.others` is the fraction of recent DCD samples where the
  // modem detected non-self activity - aggregate, not per-peer.
  // `noise` is the measured ambient RSSI when DCD is off; rssi-noise
  // = effective SNR.
  // `smoothing` is a sample-count for a trailing moving average applied
  // at render time. Firmware emits 1Hz raw - 1 leaves the data
  // unsmoothed (sharp announce spikes), 3-5 gives gentler humps.
  // Server-side raw is preserved either way; this is purely a render
  // knob the SPA controls.
  radioTlm: { samples: [], cap: 120, seeded: false, periodMs: 1000, cwMax: 4, smoothing: 3 },
  netTlm:   { samples: [], cap: 120, seeded: false, periodMs: 1000, smoothing: 3 },
  // Live filter typed into #search; conv rows x-show against it.
  searchQuery: '',

  // Identity-code TTL in ms. Defaulted to the firmware's current value
  // (60 s) for the first-render fallback; replaced from /api/info's
  // identity_code_ttl_ms once that lands.
  idCodeTtlMs: 60_000,

  // ============================== FORM STATE ==============================
  // Every user-facing form binds its inputs into one of these sub-objects
  // via x-model. Populate-from-server handlers assign whole objects
  // (Object.assign(state.forms.x, server_values)); submit handlers read
  // straight off state.forms.x; clear-form is Object.assign(state.forms.x,
  // FORM_DEFAULTS.x). No more $('input-id').value = '' across the file.
  forms: {
    bootstrap:       { ssid: '', psk: '', code: '' },
    map:             { mode: 'sd', default_zoom: 16, download_url: '' },
    createIdent:     { name: '', pw1: '', pw2: '', code: '' },
    login:           { pw: '' },
    factoryReset:    { code: '' },           // bottom of identity-picker
    factoryReset2:   { code: '' },
    diag:            { kissOut: false },           // settings → Reset tab
    newConv:         { addr: '', name: '' },
    // compose.telemetry is null until the user opens the telemetry
    // popover; then { location, environment, battery, compass,
    // share_s }. Cleared on send and on chat switch (an attach is
    // aimed at one recipient).
    compose:         { text: '', telemetry: null },
    idCode:          { input: '' },          // physical-presence modal
    contactRename:   { addr: '', name: '' },
    identity:        { displayName: '', announceInterval: '', persistOutbound: false, stampCost: '0', enforceStamps: false, screen: false,
                       telLocation: true, telEnvironment: false, telBattery: false, telCompass: false, telShare: '0', telRate: '60' },
    messenger:       { presets: [] },        // OLED messenger preset editor
    // Collector reporting (Settings -> Telemetry); also feeds the
    // system popover's telemetry status line.
    collector:       { enabled: false, collectors: [], identity: '', interval_s: 900,
                       location: true, compass: false, environment: true, battery: true,
                       diag: false,
                       last_result: 'never', last_error: '', last_sent_epoch: 0 },
    wifiAdd:         { ssid: '', psk: '', code: '' },
    tcpAdd:          { name: '', host: '', port: 4242 },
    radio:           { discoverable: false, preset: '__empty',
                       freq: 0, bw: 0, sf: 0, cr: 0, tx: 0,
                       airtime: 0, airtimeLt: 0 },
    // LoRa interface mode + IFAC. mode/ifac* are the editable inputs;
    // modeDefault / ifacConfigured / ifacConfiguredSize mirror the GET
    // so the UI can show "(default)" and "already configured" hints.
    // ifacNetkey is write-only: GET never returns it, and a blank value
    // on save keeps the existing key.
    loraCfg:         { mode: 'gateway', modeDefault: true,
                       ifacNetname: '', ifacNetkey: '', ifacSize: 0,
                       ifacConfigured: false, ifacConfiguredSize: 0 },
    discovery:       { enabled: false, advertisedName: '', interval: '360', stampCost: '14' },
    prefs:           { enterSends: false, autoDownloadAttachments: false,
                       inboxDefaultRetention: '' },
    storage:         { maxSend: 65536, maxRecv: 65536 },
  },

  // Set of busy-button IDs. withBusy(buttonId, fn) adds the id while fn
  // is in flight; markup uses :disabled="!!$store.s.busyButtons[id]" and
  // :class="{ 'is-busy': $store.s.busyButtons[id] }". Plain object so
  // Alpine sees mutations.
  busyButtons: {},

  // Radio-status badge on the topbar. {state, title} drives :data-state
  // and :title on #btn-status. state ∈ 'unknown'|'on'|'off'|'no-config'.
  radioStatus: { state: 'unknown', title: 'Loading…' },

  // Various live-state surfaces previously written via .textContent =:
  ui: {
    selfName: '',                    // mirror of state.self.display_name
    selfAddr: '',                    // mirror of state.self.address
    loginTitle: 'Login',             // login view header
    rdPresetNote: '',                // dynamic blurb beneath the preset select
    discNameStatus: '',              // "saved" / "saving…" caption
    discStatus: '',                  // master status caption on discovery tab
    discIdentityHash: '-',           // hash printed in the Discovery tab
    storageStatus: '',               // status caption on storage sliders
    storageMaxSendLabel: '',         // KB readout next to the send slider
    storageMaxRecvLabel: '',         // KB readout next to the recv slider
    tmCurrent: '-',                  // time-master current value
    tmSource:  '',                   // time-master source label
    settingsTitle: 'Settings',       // h3 above the tab bar in #modal-settings
    tpEndpoint:    '',               // transport endpoint URL in App tab
    announceState: 'none',           // data-state on #btn-announce-top: none|sent|auto
    announceLabelText: '',           // countdown label text (empty => label hidden)
    announceTitle: 'Announce me',    // title attr on #btn-announce-top
    micRecording:  false,            // adds .recording class to #btn-mic
    showCloseSettings: true,         // x-show on the X button in #modal-settings
    sysClockNow: '',
    sysTeleStatus: '',
    // Global 1 Hz heartbeat. Bindings that show ageing text (live
    // strips, bubble telemetry ages) read this so they re-render each
    // second without their own timers.
    nowS: Math.floor(Date.now() / 1000),                 // wall-clock display in system popover
    sysClockSource: '',              // "Source: X" / "Last calibrated …"
    sysGpsAge: '',                   // sensor-age strings in system popover
    sysBmeAge: '',
    sysMagAge: '',
    sysImuAge: '',
    rbStatus:  '',                   // reboot banner text
    composeCounter: '',              // composer character/byte counter
    composeCounterShown: false,      // toggles composer counter visibility
    composeCounterDanger: false,     // colour the counter red near the cap
    composeMaxLength: 4096,          // maxlength on the composer textarea
    idAddress: '',                   // address line in Settings → Identity
    idNameStatus: '',                // saved/saving caption next to name input
    idCodeModalHint: '',             // per-call context line in #modal-id-code
    idCodeModalTitle: 'Identity code required',  // h3 in #modal-id-code
    inboxConfigStatus: '',           // saved/error caption under retention select
    cnTcpStatus: '',                 // saved/error caption under tcp-add modal
    cnLoraCfgStatus: '',             // saved/error/reboot caption under LoRa mode+IFAC save
    storageMaxSendMax: 1048576,      // live max bytes for the send slider
    storageMaxRecvMax: 1048576,      // live max bytes for the recv slider
    showRadioSection: true,          // Connectivity tab - toggled by
    showTcpSection:   true,          //   applyTransportCapabilities()
    showBtSection:    true,          //   based on /api/info → transports
    btnStatus: { state: 'unknown', title: 'Loading…' },  // top-bar radio button
    cnDotRadio: 'hidden',            // connectivity dots (hidden|ok|warn|err)
    cnDotWifi:  'hidden',
    cnDotTcp:   'hidden',
    cnDotBle:   'hidden',
    cnDotSerial:'hidden',
  },
};

// Shared copy for the identity-code hint, displayed everywhere a hex
// code is required. Each <p class="id-code-hint"> binds
// `x-html="idCodeHintHtml()"`, so the duration tracks state.idCodeTtlMs
// reactively (sourced via /api/info → identity_code_ttl_ms). Falls back
// to 60 s before /api/info has arrived (matches the firmware default).
function idCodeTtlText() {
  const ms = (state && state.idCodeTtlMs) || 60_000;
  const s  = Math.round(ms / 1000);
  if (s < 60)        return s + ' seconds';
  if (s % 60 === 0)  return (s / 60) + ' minutes';
  return s + ' seconds';
}
function idCodeHintHtml() {
  return '<strong>Identity code:</strong> a 6-character hex code that proves you\'re physically holding the device, required for any action sensitive enough that doing it remotely should not be possible. To generate one: long-press the device\'s BOOT button (~1 s), release, then short-press within 2 s; the code appears on the OLED for ' + idCodeTtlText() + '.';
}

// ============================== UTIL ==============================
function $(id) { return document.getElementById(id); }
function show(viewId) {
  // Alpine drives the view switching via x-show against $store.s.view;
  // legacy callers still call show('view-foo') and it works because
  // state.view is reactive.
  state.view = viewId;
}
function relTime(tsSec) {
  if (!tsSec) return '';
  const dMs = Date.now() - tsSec * 1000;
  if (dMs < 60_000)         return Math.max(0, Math.floor(dMs/1000)) + 's';
  if (dMs < 3600_000)       return Math.floor(dMs/60_000) + 'm';
  if (dMs < 86400_000)      return Math.floor(dMs/3_600_000) + 'h';
  const d = new Date(tsSec * 1000);
  return d.getDate() + '/' + (d.getMonth()+1);
}
function shortHex(h) { return h ? (h.length > 8 ? h.slice(0,8) + '…' : h) : ''; }
function nameFor(peerHex) {
  return state.contacts[peerHex] || shortHex(peerHex);
}

// ============================== BUSY ==============================
// Generic async-button wrapper. Any click handler whose action waits
// on the device - POST, multi-step setup, anything that takes more
// than ~100 ms to surface a visible result - should run through this.
// Pattern:
//
//   $('btn-foo').addEventListener('click', () => withBusy($('btn-foo'),
//     async () => {
//       await state.transport.doThing();
//       toast('Done', 'success');
//     },
//     { label: 'Saving…', alsoDisable: ['#in-foo', '#btn-cancel'] }
//   ));
//
// During the async fn the button gets a spinner glyph, its label is
// optionally swapped, and it is disabled along with any peer inputs
// in `alsoDisable`. Re-entry is suppressed via a busy data attribute
// so double-clicks during the in-flight request are no-ops. The
// finally block restores everything - safe even if the async fn
// triggers a view transition or full-page replacement (the original
// button is just detached at that point and the restoration is a
// no-op on dead nodes).
//
// Errors propagate to the caller; the wrapper only owns the visual
// state. Catch + toast inside the async fn (or above the call) as
// usual.
async function withBusy(btn, asyncFn, opts = {}) {
  if (!btn || btn.dataset.busy) return;
  btn.dataset.busy = '1';
  // Buttons whose markup carries :disabled / :class /
  // x-text="busyButtons[id] ? 'Saving…' : 'Save'" bindings get a
  // declarative busy state from this set. Buttons that don't yet have
  // those bindings still get the imperative DOM manipulation below
  // (originalText swap, disabled, .is-busy class) so the visual state
  // works either way.
  const id = btn.id || null;
  if (id) state.busyButtons[id] = true;
  const originalText = btn.textContent;
  if (opts.label) btn.textContent = opts.label;
  btn.classList.add('is-busy');
  btn.disabled = true;
  const peers = (opts.alsoDisable || [])
    .map(sel => (typeof sel === 'string') ? document.querySelector(sel) : sel)
    .filter(Boolean);
  for (const el of peers) el.disabled = true;
  // Top-bar progress strip - opt-in via opts.bgTaskLabel so we only
  // signal genuine long-running tasks (reboot, factory reset, big
  // saves), not every <100 ms click.
  let bgId = null;
  if (opts.bgTaskLabel) {
    bgId = 'busy:' + (id || Math.random().toString(36).slice(2));
    startBgTask(bgId, { label: opts.bgTaskLabel });
  }
  try {
    return await asyncFn();
  } finally {
    btn.disabled = false;
    for (const el of peers) el.disabled = false;
    btn.textContent = originalText;
    btn.classList.remove('is-busy');
    delete btn.dataset.busy;
    if (id) state.busyButtons[id] = false;
    if (bgId) endBgTask(bgId);
  }
}

