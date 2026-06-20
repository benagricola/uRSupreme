// ============================== BOOT / VIEW FLOW ==============================
async function boot() {
  state.transport = makeTransportFromStorage();
  state.ui.tpEndpoint = state.transport.endpoint || '';
  await state.transport.connect();
  let info;
  try { info = await state.transport.getInfo(); }
  catch (e) { console.warn('getInfo failed:', e); info = null; }
  state.info = info;
  if (info && info.limits) applyServerLimits(info.limits);
  state.identities = (info && info.identities) || [];
  // Snapshot the identity-code TTL before any pre-auth view renders so
  // the hint text on the bootstrap / identity-picker screens shows
  // the right number. refreshStatus() also does this post-login.
  if (info && typeof info.identity_code_ttl_ms === 'number') {
    state.idCodeTtlMs = info.identity_code_ttl_ms;
  }

  if (info && info.bootstrap && !state.skipBootstrap) { return enterBootstrap(); }
  if (info && info.identity_code_pending) { state.pendingIdCode = true; }

  if (state.token && state.identityId) {
    // Validate token by hitting /state.
    try {
      const me = await state.transport.getIdentity(state.identityId);
      state.self = me;
      await refreshState();
      enterApp();
      return;
    } catch (e) {
      // Token bad - fall through to identity picker.
      console.warn('token validation failed:', e);
      state.token = null;
      localStorage.removeItem(LS.TOKEN);
    }
  }
  enterIdentityPicker();
}

function makeTransportFromStorage() {
  const which = localStorage.getItem(LS.TRANSPORT) || 'urhttp';
  if (which === 'urhttp') return new UReticulumHTTPTransport();
  // Future: 'wasm-rs' → new WasmReticulumRsTransport();
  return new UReticulumHTTPTransport();
}

function enterBootstrap() {
  show('view-bootstrap');
  bsRescan();
}
function enterIdentityPicker() {
  show('view-identities');
  // List rendered declaratively from $store.s.identities.
}
function enterCreate() {
  show('view-create');
  Object.assign(state.forms.createIdent, { name: '', pw1: '', pw2: '', code: '' });
}
function enterLogin(identity) {
  show('view-login');
  state.identityId = identity.id;
  state.ui.loginTitle = 'Login: ' + (identity.display_name || identity.id);
  state.forms.login.pw = '';
  // #login-pw has x-effect bound to $store.s.view === 'view-login' that
  // calls $el.focus() inside $nextTick, so flipping the view above is
  // enough - no imperative focus needed here.
}
async function enterApp() {
  show('view-app');
  renderSelf();
  renderConversations();
  renderAnnounces();
  renderPaths();
  renderContacts();
  startEventStream();
  refreshStatus();  // populate the topbar indicator
  // Load the device's storage caps so the composer's attachment limit
  // (attachMaxBytes) reflects the real effective_max_send rather than the
  // 64 KB fallback. Runs on every app-enter, including reload/auto-login
  // (populateStorageConfig only fires when the Settings modal opens).
  refreshStorageCaps().catch(() => {});
  // Map tile availability (SD present? which zooms?) for the location view.
  fetchMapConfig().catch(() => {});
  // First-run radio check: if the device hasn't been configured yet,
  // pop the settings modal to the Radio tab in locked mode so the
  // user can't dismiss it until they've saved. Without a working radio
  // the rest of the UI is just a museum exhibit.
  try {
    const r = await state.transport.getRadio();
    if (!r.have_conf || !r.radio_online) {
      openSettingsModal('radio', true);
    }
  } catch (e) {
    console.warn('getRadio probe failed:', e);
  }
  // Seed the announce countdown from the snapshot we already have.
  if (state.self) {
    seedAnnounceCountdown(state.self.next_announce_in_ms || 0,
                          state.self.announce_interval_ms || 0);
  }
}

// ============================== BOOTSTRAP VIEW WIRING ==============================
async function bsRescan() {
  state.wifiScanState = 'scanning';
  state.wifiScanList  = [];
  try {
    const r = await state.transport.wifiScan();
    state.wifiScanList  = r.networks || [];
    state.wifiScanState = 'idle';
  } catch (_) {
    state.wifiScanState = 'error';
  }
}
// Handlers bound via @click in the markup for the bootstrap view.
function onClickBsSave(btn) {
  return withBusy(btn, async () => {
    const f = state.forms.bootstrap;
    const ssid = f.ssid || '';
    if (!ssid) { toast('Pick a network first', 'warn'); return; }
    const code = (f.code || '').trim();
    if (!code) { toast('Enter the identity code from the device OLED', 'warn'); return; }
    try {
      const r = await state.transport.wifiConfigure(ssid, f.psk, code);
      // Two response shapes:
      //   - { status: 'connected', sta_ip, hostname, sta_url, mdns_url }
      //     - inline transition, no reboot. Device joined the new
      //       network while we were holding the softAP. Show the
      //       URLs; the AP will be deauthed by the device within
      //       a second so this page goes unreachable shortly after.
      //   - { restart: true } - legacy reboot path. Use the polling
      //     screen as before.
      if (r && r.restart) {
        waitForDeviceAndReload('Saving WiFi & rebooting…');
      } else {
        showWifiProvisionedPanel(r);
      }
    } catch (e) {
      toast('Save failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Saving WiFi…', alsoDisable: ['#bs-ssid', '#bs-psk', '#bs-code', '#bs-rescan', '#bs-skip'] });
}
// Skip - let the user proceed into the chat UI without configuring a home
// WiFi network. Useful for standalone / off-grid operation from the softAP
// itself. Routes the same way as a normal boot (identity picker if no
// identities, login if some exist).
async function onClickBsSkip() {
  state.skipBootstrap = true;
  try {
    const info = await state.transport.getInfo();
    state.info = info;
    if (!info.identities || info.identities.length === 0) {
      enterCreate();
    } else {
      enterIdentityPicker();
    }
  } catch (e) {
    toast('Failed to leave bootstrap: ' + (e.message || e), 'error');
  }
}

// ============================== IDENTITY PICKER / CREATE / LOGIN ==============================
async function onClickFactoryReset(btn) {
  const ok = await showConfirm({
    title: 'Factory reset?',
    body: 'This permanently erases every identity, message, and saved token on this device.',
    okLabel: 'Factory reset',
    destructive: true,
  });
  if (!ok) return;
  await withBusy(btn, async () => {
    try {
      await state.transport.factoryReset(state.forms.factoryReset.code);
      waitForDeviceAndReload('Factory-resetting & rebooting…');
    } catch (e) {
      toast('Reset failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Resetting…', alsoDisable: ['#fr-code', '#btn-show-create'] });
}

function onClickCreateGo(btn) {
  const f = state.forms.createIdent;
  if (f.pw1 !== f.pw2)        { toast('Passwords do not match', 'warn'); return; }
  if ((f.pw1 || '').length < 8) { toast('Password must be at least 8 chars', 'warn'); return; }
  return withBusy(btn, async () => {
    try {
      const r = await state.transport.createIdentity(f.name, f.pw1, f.code);
      state.token = r.token;
      state.identityId = r.id;
      localStorage.setItem(LS.TOKEN, r.token);
      localStorage.setItem(LS.IDENTITY, r.id);
      state.identities = ((await state.transport.getInfo()).identities) || [];
      state.self = state.identities.find(x => x.id === r.id) || { id: r.id, address: r.address, display_name: f.name };
      await refreshState();
      enterApp();
    } catch (e) { toast('Create failed: ' + (e.message || e), 'error'); }
  }, { label: 'Creating…', alsoDisable: ['#btn-create-cancel', '#cr-name', '#cr-pw1', '#cr-pw2', '#cr-code'] });
}

function onClickLoginGo(btn) {
  return withBusy(btn, async () => {
    try {
      const r = await state.transport.login(state.identityId, state.forms.login.pw);
      state.token = r.token;
      state.identityId = r.identityId;
      localStorage.setItem(LS.TOKEN, r.token);
      localStorage.setItem(LS.IDENTITY, r.identityId);
      state.self = state.identities.find(x => x.id === r.identityId) || null;
      await refreshState();
      enterApp();
    } catch (e) {
      if (e.status === 401) toast('Wrong password', 'error');
      else toast('Login failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Logging in…', alsoDisable: ['#login-pw', '#btn-login-back'] });
}

// ============================== APP WIRING ==============================
function openNewConvModal() {
  Object.assign(state.forms.newConv, { addr: '', name: '' });
  renderNewConvPicker();
  state.modals.newConv = true;
}

// Build the contact / discovered-peer picker for the new-conversation
// modal. Saved contacts come first, discovered peers (lxmf.delivery
// announces we've heard but haven't chatted to or saved) come second.
// Reactive data for #nc-pick (the New Conversation modal's peer picker).
// Contacts come first; "Discovered" = announces from peers we've not
// chatted with or saved.
function newConvPickerData() {
  return {
    get contacts() {
      return Object.entries(this.$store.s.contacts || {})
        .map(([addr, name]) => ({ addr, name }));
    },
    get discovered() {
      const known = new Set(Object.keys(this.$store.s.contacts || {}));
      for (const p of Object.keys(this.$store.s.conversations || {})) known.add(p);
      return Object.values(this.$store.s.announces || {})
        .filter(a => !known.has(a.dest))
        .sort((a, b) => (b.received_ms || 0) - (a.received_ms || 0));
    },
    heardLabel(a) {
      const s = Math.floor((Date.now() - (a.received_ms || 0)) / 1000);
      return s < 60 ? s + 's' : Math.floor(s/60) + 'm';
    },
  };
}

// Backwards-compat stub for callers that called renderNewConvPicker()
// explicitly. The Alpine template now repaints itself reactively.
function renderNewConvPicker() { /* no-op */ }
function onClickNewConvGo() {
  const addr = (state.forms.newConv.addr || '').trim().toLowerCase();
  if (!/^[0-9a-f]{32}$/.test(addr)) { toast('Address must be 32 hex characters', 'warn'); return; }
  const name = (state.forms.newConv.name || '').trim();
  if (name) { state.contacts[addr] = name; persistContacts(); }
  if (!state.conversations[addr]) state.conversations[addr] = { peer: addr, msgs: [], last_ts: 0 };
  selectConversation(addr);
  state.modals.newConv = false;
}

// Delegated conv-row click handler. Bound once to the container so an
// SSE rerender mid-click can't strip the listener off the row that was
// pressed. Looks for the nearest ancestor carrying a data-peer attr.
$('conv-list').addEventListener('click', (ev) => {
  const row = ev.target.closest('[data-peer]');
  if (!row) return;
  const peer = row.getAttribute('data-peer');
  // Synthetic "Incoming attachment …" rows carry data-peer="" because
  // the sender isn't decrypted yet. Treat them as non-interactive
  // until the LXMF message arrives - clicking would otherwise
  // selectConversation('') and unload whatever chat the user was on.
  if (!peer) return;
  selectConversation(peer);
});

// ============== ROW CONTEXT MENU ==============
// Per-row hamburger menu for retention + clear. Visible always-on as
// the `⋮` icon on landscape mobile + tablet (CSS media query); right-
// click and long-press also open it on any viewport so touch-screen
// laptops + Surface-style devices have a usable entry point even when
// the icon isn't rendered. Discovered rows (data-discovered="1") are
// excluded - they have nothing to retain or clear.
function closeRowMenu() { state.rowMenu = null; }
function openRowMenu(peer, anchor) {
  if (!peer) { state.rowMenu = null; return; }
  const items = [
    { label: 'Rename peer…',    fn: () => renamePeerFlow(peer) },
    { label: 'Retention…',      fn: () => openRetentionModal(state.identityId, peer) },
    { label: 'Clear messages…', fn: () => clearConversationFlow(peer), destructive: true },
  ];
  // Position near the anchor. anchor is either a DOM element (the ⋮
  // button) or a {x, y} for cursor / touch-point. Clamp to viewport.
  // Sizes are estimates - the rendered menu may differ; the clamp
  // tolerates that.
  const rect = (anchor && anchor.getBoundingClientRect)
      ? anchor.getBoundingClientRect()
      : { left: anchor.x, right: anchor.x, top: anchor.y, bottom: anchor.y };
  const menuW = 200, menuH = 130;
  let left = Math.min(rect.right - menuW, window.innerWidth - menuW - 8);
  let top  = rect.bottom + 4;
  if (top + menuH > window.innerHeight - 8) top = Math.max(8, rect.top - menuH - 4);
  if (left < 8) left = 8;
  state.rowMenu = { items, x: left, y: top };
}
// Close on outside click / Esc / scroll. Captured at the document
// level so any tap outside the menu - including taps on a conv-row -
// dismisses it before the row's own click handler runs.
document.addEventListener('mousedown', (ev) => {
  if (!state.rowMenu) return;
  // Walk up to find the row-menu wrapper; if the click isn't inside
  // it, close.
  let n = ev.target;
  while (n && n !== document.body) {
    if (n.classList && n.classList.contains('row-menu')) return;
    n = n.parentNode;
  }
  closeRowMenu();
}, true);
document.addEventListener('keydown', (ev) => {
  if (ev.key === 'Escape') closeRowMenu();
});
window.addEventListener('scroll', closeRowMenu, true);

// Right-click on a conv row opens the menu at the cursor. preventDefault
// so the browser's native context menu doesn't show.
$('conv-list').addEventListener('contextmenu', (ev) => {
  const row = ev.target.closest('[data-peer]');
  if (!row) return;
  if (row.getAttribute('data-discovered') === '1') return;
  const peer = row.getAttribute('data-peer');
  if (!peer) return;
  ev.preventDefault();
  openRowMenu(peer, { x: ev.clientX, y: ev.clientY });
});

// Long-press on touch: 500ms hold opens the menu at the touch point.
// Cancelled on touchmove / touchend / scroll. Only fires for non-
// discovered rows - same rule as right-click.
(() => {
  let timer = null;
  let cancelled = false;
  const cancel = () => { if (timer) { clearTimeout(timer); timer = null; } cancelled = true; };
  $('conv-list').addEventListener('touchstart', (ev) => {
    const row = ev.target.closest('[data-peer]');
    if (!row) return;
    if (row.getAttribute('data-discovered') === '1') return;
    const peer = row.getAttribute('data-peer');
    if (!peer) return;
    const t = ev.touches[0];
    if (!t) return;
    cancelled = false;
    const x = t.clientX, y = t.clientY;
    timer = setTimeout(() => {
      if (cancelled) return;
      // Suppress the click that the touchend will fire - we don't want
      // selectConversation to run alongside the menu open.
      row._suppressNextClick = true;
      openRowMenu(peer, { x, y });
    }, 500);
  }, { passive: true });
  $('conv-list').addEventListener('touchmove',   cancel, { passive: true });
  $('conv-list').addEventListener('touchcancel', cancel, { passive: true });
  $('conv-list').addEventListener('touchend',    cancel, { passive: true });
})();
// Swallow the post-long-press click so it doesn't select the
// conversation while the menu is opening.
$('conv-list').addEventListener('click', (ev) => {
  const row = ev.target.closest('[data-peer]');
  if (row && row._suppressNextClick) {
    row._suppressNextClick = false;
    ev.stopPropagation();
    ev.preventDefault();
  }
}, true);


// Click the chat header name to rename the contact (or assign one for
// the first time). Stores locally to state.contacts, same backing
// store as the Identity → Contacts list in Settings.
// Reusable rename/clear flows so the per-row context menu (landscape
// mobile + tablet) and the thread-header buttons (desktop) share one
// implementation. Both take an explicit peer arg - neither reaches
// for state.openPeer, since the row menu acts on the row's peer which
// may not be the currently-open conversation.
async function renamePeerFlow(peer) {
  if (!peer) return;
  const current = state.contacts[peer] || '';
  const r = await showConfirm({
    title: 'Rename chat',
    body: 'Set the display name shown for this peer. Saved locally on this browser only.',
    okLabel: 'Save',
    needsInput: { label: 'Display name', value: current, placeholder: 'Their name' },
  });
  if (!r) return;
  const name = (r.value || '').trim();
  if (name) state.contacts[peer] = name;
  else      delete state.contacts[peer];   // empty input = clear name
  persistContacts();
  renderContacts();
  renderConversations();
  renderThread();
}
async function clearConversationFlow(peer) {
  if (!peer) return;
  const display = nameFor(peer);
  const ok = await showConfirm({
    title: 'Clear conversation?',
    body: 'Clear all messages with ' + display + ' on this device?\n\nThe peer keeps their copy; only this device\'s inbox + outbox entries for this conversation are removed.',
    okLabel: 'Clear',
    destructive: true,
  });
  if (!ok) return;
  try {
    await state.transport.clearConversation(state.identityId, peer);
    delete state.conversations[peer];
    if (state.openPeer === peer) state.openPeer = null;
    renderConversations();
    renderThread();
    toast('Conversation cleared', 'ok');
  } catch (e) {
    toast('Clear failed: ' + (e.message || e), 'error');
  }
}

// #thread-name now has @click="renamePeerFlow($store.s.openPeer)" in
// the markup directly.
// Format a TTL in seconds as a human-friendly label, choosing the
// coarsest unit that comes out whole (minutes → hours → days). 0 is
// rendered as "no expiry" so it's never ambiguous with "0 seconds".
function ttlLabel(secs) {
  const s = Number(secs) | 0;
  if (s <= 0) return 'no expiry';
  if (s % 86400 === 0) { const n = s / 86400; return n + ' day' + (n === 1 ? '' : 's'); }
  if (s % 3600  === 0) { const n = s / 3600;  return n + ' hour' + (n === 1 ? '' : 's'); }
  if (s % 60    === 0) { const n = s / 60;    return n + ' minute' + (n === 1 ? '' : 's'); }
  return s + ' seconds';
}

// Render a Retention object {kind, value} as a human-readable label
// for the "Use identity default (X)" inline.
function retentionLabel(r) {
  if (!r || r.kind === 'none' || !r.value) return 'no expiry';
  if (r.kind === 'count') {
    return 'last ' + r.value + ' message' + (r.value === 1 ? '' : 's');
  }
  return 'last ' + ttlLabel(r.value);
}

// Open the retention modal for `peerHex` and return when the user
// either cancels or saves. Save POSTs the new override to the device.
async function openRetentionModal(identityId, peerHex) {
  let cfg;
  try {
    cfg = await state.transport.getConversationConfig(identityId, peerHex);
  } catch (e) {
    toast('Could not load retention: ' + (e.message || e), 'error');
    return;
  }
  const defaultR = cfg.default_retention || { kind: 'none', value: 0 };
  const r = cfg.retention;
  // Seed the store from the saved per-peer override (null = default).
  const ret = state.retention;
  ret.defaultLabel = retentionLabel(defaultR);
  ret.identityId   = identityId;
  ret.peerHex      = peerHex;
  ret.timeN        = 7;
  ret.timeUnit     = '86400';
  ret.countN       = 200;
  if (!r) {
    ret.mode = 'default';
  } else if (r.kind === 'none' || !r.value) {
    ret.mode = 'forever';
  } else if (r.kind === 'time') {
    ret.mode = 'time';
    const s = Number(r.value);
    if      (s % 86400 === 0) { ret.timeN = s / 86400; ret.timeUnit = '86400'; }
    else if (s % 3600  === 0) { ret.timeN = s / 3600;  ret.timeUnit = '3600';  }
    else if (s % 60    === 0) { ret.timeN = s / 60;    ret.timeUnit = '60';    }
    else                      { ret.timeN = s;          ret.timeUnit = '60';    }
  } else if (r.kind === 'count') {
    ret.mode   = 'count';
    ret.countN = Number(r.value);
  } else {
    ret.mode = 'default';
  }
  state.modals.convRetention = true;
  return new Promise((resolve) => { ret._resolve = resolve; });
}

function retentionCancel() {
  const r = state.retention;
  if (!r._resolve) return;
  const resolve = r._resolve;
  r._resolve = null;
  state.modals.convRetention = false;
  resolve(false);
}

async function retentionSave(btn) {
  const r = state.retention;
  let body;
  if (r.mode === 'default') body = { retention: null };
  else if (r.mode === 'forever') body = { retention: { kind: 'none', value: 0 } };
  else if (r.mode === 'time') {
    const n = parseInt(r.timeN, 10);
    const unit = parseInt(r.timeUnit, 10);
    if (!Number.isFinite(n) || n <= 0) { toast('Enter a positive number.', 'error'); return; }
    body = { retention: { kind: 'time', value: n * unit } };
  } else if (r.mode === 'count') {
    const n = parseInt(r.countN, 10);
    if (!Number.isFinite(n) || n <= 0) { toast('Enter a positive number.', 'error'); return; }
    body = { retention: { kind: 'count', value: n } };
  } else {
    body = { retention: null };
  }
  try {
    await withBusy(btn, async () => {
      await state.transport.setConversationConfig(r.identityId, r.peerHex, body);
    }, { label: 'Saving…' });
    toast('Retention saved.', 'ok');
    const resolve = r._resolve;
    r._resolve = null;
    state.modals.convRetention = false;
    if (resolve) resolve(true);
  } catch (e) {
    toast('Save failed: ' + (e.message || e), 'error');
  }
}

// btn-conv-retention / btn-clear-conv are now @click-bound in the
// thread-header template (Wave 4).
function onClickRadioSave(btn) {
  return withBusy(btn, async () => {
    try {
      // Single save path - /api/radio now persists airtime caps alongside
      // the band params so a region preset's regulatory limits survive a
      // reboot. Old separate /api/radio/airtime endpoint remains for live
      // tweaks without a reboot but isn't part of the user-driven save.
      const f = state.forms.radio;
      await state.transport.setRadio({
        frequency: f.freq,
        bandwidth: f.bw,
        spreading_factor: f.sf,
        coding_rate: f.cr,
        tx_power: f.tx,
        airtime_limit_pct:          f.airtime,
        longterm_airtime_limit_pct: f.airtimeLt,
      });
      waitForDeviceAndReload('Saving radio config & rebooting…');
    } catch (e) {
      toast('Save failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Saving radio…' });
}
// Connectivity tab: accordion exclusivity - close every other section
// when one opens, so the tab never grows taller than the modal.
for (const det of document.querySelectorAll('#modal-settings .cn-section')) {
  det.addEventListener('toggle', () => {
    if (!det.open) return;
    for (const other of document.querySelectorAll('#modal-settings .cn-section')) {
      if (other !== det) other.open = false;
    }
  });
}

// WiFi: list defaults to saved networks + an "Add a network" button.
// Add opens a modal that runs the scan and the same wifiConfigure
// call the bootstrap softAP flow uses.
function onClickWifiSave(btn) {
  return withBusy(btn, async () => {
    const f = state.forms.wifiAdd;
    if (!f.ssid) { toast('Pick a network first.', 'error'); return; }
    const code = (f.code || '').trim();
    if (!code) { toast('Identity code required to change WiFi.', 'error'); return; }
    try {
      const r = await state.transport.wifiConfigure(f.ssid, f.psk, code);
      state.modals.wifiAdd = false;
      if (r && r.restart) {
        waitForDeviceAndReload('Saving WiFi & rebooting…');
      } else {
        showWifiProvisionedPanel(r);
      }
    } catch (e) {
      toast('WiFi save failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Saving WiFi…', alsoDisable: ['#cn-wifi-ssid', '#cn-wifi-psk', '#cn-wifi-code', '#cn-wifi-scan', '#cn-wifi-cancel'] });
}

async function openAddWifiModal() {
  Object.assign(state.forms.wifiAdd, { ssid: '', psk: '', code: '' });
  state.modals.wifiAdd = true;
  await scanIntoWifiModal();
}

async function scanIntoWifiModal() {
  // Same scan store as the bootstrap view - both <select>s bind to it.
  state.wifiScanState = 'scanning';
  state.wifiScanList  = [];
  try {
    const r = await state.transport.wifiScan();
    state.wifiScanList  = r.networks || [];
    state.wifiScanState = 'idle';
  } catch (_) {
    state.wifiScanState = 'error';
  }
}

// Body cap matches the firmware's LXMF_MAX_BODY_BYTES. The textarea's
// maxlength attribute is the hard stop in browser UI; the counter below
// gives the user a visual heads-up as they approach it.
const COMPOSE_MAX_CHARS = 4096;
// Keydown handler bound via @keydown on the textarea. Branches on the
// enter-sends pref so users on either mental model (chat-app / slack)
// get the behaviour they expect.
function onComposeKeydown(e) {
  if (e.key !== 'Enter') return;
  // enterSends=true: Enter → send, Shift+Enter → newline (default chat-app)
  // enterSends=false: Shift+Enter → send, Enter → newline (mirror Slack/Discord)
  const send = state.prefs.enterSends ? !e.shiftKey : e.shiftKey;
  if (send) { e.preventDefault(); sendMessage(); }
}
// Input handler bound via @input on the textarea. Auto-grows the field
// up to 120 px and updates the counter substate; the counter element
// itself is purely declarative (x-show + x-text on $store.s.ui).
function onComposeInput(e) {
  e.target.style.height = 'auto';
  e.target.style.height = Math.min(120, e.target.scrollHeight) + 'px';
  // Count what the device counts: UTF-8 bytes against the live
  // server-published cap. .length counts UTF-16 code units, which
  // under-reads emoji/CJK and made the counter disagree with the
  // device's 413 verdict.
  const cap = state.ui.composeMaxLength || COMPOSE_MAX_CHARS;
  const len = new TextEncoder().encode(state.forms.compose.text || '').length;
  state.ui.composeCounterShown  = len >= cap * 0.75;
  state.ui.composeCounterDanger = len >= cap * 0.95;
  state.ui.composeCounter       = len + ' / ' + cap;
}
function onClickEmoji(ev) {
  ev.stopPropagation();
  openEmojiPicker(ev.currentTarget, (e) => {
    // Splicing into a textarea needs the live caret positions, which only
    // the DOM has - Alpine state mirrors .value but not selectionStart.
    const ta = $('compose-text');
    const start = ta.selectionStart || ta.value.length;
    const end   = ta.selectionEnd   || ta.value.length;
    const next  = ta.value.slice(0, start) + e + ta.value.slice(end);
    state.forms.compose.text = next;     // x-model wakes up
    requestAnimationFrame(() => {
      ta.focus();
      ta.selectionStart = ta.selectionEnd = start + e.length;
    });
  });
}

// Search field - writes state.searchQuery; conv-row entries x-show
// against convRowMatchesSearch() so filtering is declarative. Per-row
// match keeps the typeahead snappy without rebuilding the list each
// keystroke.
function convRowMatchesSearch(c) {
  const q = (state.searchQuery || '').toLowerCase();
  if (!q) return true;
  const name = (c.display_name || nameFor(c.peer) || '').toLowerCase();
  const last = (c.last_body || '').toLowerCase();
  return name.includes(q) || (c.peer || '').toLowerCase().includes(q) || last.includes(q);
}
function convRowMatchesSearchDiscovered(a) {
  const q = (state.searchQuery || '').toLowerCase();
  if (!q) return true;
  const name = (a.display_name || nameFor(a.dest) || '').toLowerCase();
  return name.includes(q) || (a.dest || '').toLowerCase().includes(q);
}

