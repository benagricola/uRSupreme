// ============================== RADIO REGION PRESETS ==============================
// Frequency + bandwidth + SF + CR + TX power per region. The frequency is
// the only strictly region-dependent value (regulatory); the rest are
// community-conventional defaults that interop with the typical Reticulum
// network in that region.  Order: most-likely-needed first.  Add or tweak
// as the community converges on different settings.
//
// TX power is the recommended ceiling; users can lower it freely. The
// hardware may not actually achieve the listed TX power (depends on
// SX1262 vs LR1121, antenna matching, etc.).
// airtime / lt_airtime are the regulatory short-term / long-term duty-cycle
// caps (0-99 %). 0 disables. ETSI g3 (869.4-869.65 MHz) allows 10 %, most
// other EU sub-bands are 1 % or 0.1 %, US/AU 915 are unlimited.
const RADIO_PRESETS = [
  { id: 'eu868',  label: 'EU 869.525 MHz',    freq: 869525000, bw: 125000, sf: 7,  cr: 5, tx: 17,
    airtime: 10, lt_airtime: 10,
    note: 'EU 869.4-869.65 MHz (g3 sub-band). 10 % duty cycle allowed.' },
  { id: 'eu868-long', label: 'EU 869.525 MHz · long-range', freq: 869525000, bw: 125000, sf: 11, cr: 5, tx: 14,
    airtime: 10, lt_airtime: 10,
    note: 'Same g3 sub-band, higher SF for range. 10 % duty cycle.' },
  { id: 'uk869',  label: 'UK 869.525 MHz',    freq: 869525000, bw: 125000, sf: 7,  cr: 5, tx: 17,
    airtime: 10, lt_airtime: 10,
    note: 'UK SRD860 (Ofcom IR 2030). 869.4-869.65 MHz: 10 % duty cycle, 500 mW ERP. Same regulatory regime as EU at this frequency, separate label so UK users can pick a familiar one.' },
  { id: 'uk868-1pc',  label: 'UK 868.3 MHz (1 % duty)', freq: 868300000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'UK 868.0-868.6 MHz general sub-band. 1 % duty cycle, 25 mW ERP. More headroom in TX-power than the g3 sub-band but much tighter on airtime.' },
  { id: 'us915',  label: 'US 915 MHz',        freq: 915000000, bw: 125000, sf: 7,  cr: 5, tx: 20,
    airtime: 0, lt_airtime: 0,
    note: 'US 902-928 MHz ISM. No duty-cycle limits.' },
  { id: 'au915',  label: 'AU 915 MHz',        freq: 915000000, bw: 125000, sf: 7,  cr: 5, tx: 20,
    airtime: 0, lt_airtime: 0,
    note: 'AU 915-928 MHz ISM. No duty-cycle limits.' },
  { id: 'nz864',  label: 'NZ 864 MHz',        freq: 864000000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 0, lt_airtime: 0,
    note: 'NZ 864-868 MHz ISM. No general duty-cycle limit.' },
  { id: 'in866',  label: 'IN 866 MHz',        freq: 866000000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'IN 865-867 MHz. 1 % duty cycle (conservative).' },
  { id: 'as923',  label: 'AS 923 MHz',        freq: 923200000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'AS923 (varies by country, check local rules).' },
  { id: 'jp920',  label: 'JP 920 MHz',        freq: 920900000, bw: 125000, sf: 7,  cr: 5, tx: 13,
    airtime: 10, lt_airtime: 10,
    note: 'JP 920.6-928 MHz (ARIB STD-T108). 10 % duty cycle.' },
  { id: 'kr920',  label: 'KR 920 MHz',        freq: 920900000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'KR 920-925 MHz. 1 % duty cycle (conservative).' },
  { id: 'ru868',  label: 'RU 868.9 MHz',      freq: 868900000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'RU 868.7-869.2 MHz. 1 % duty cycle.' },
  { id: 'cn470',  label: 'CN 470 MHz',        freq: 470000000, bw: 125000, sf: 7,  cr: 5, tx: 14,
    airtime: 1, lt_airtime: 1,
    note: 'CN 470-510 MHz. 1 % duty cycle. Hardware must support 470 band.' },
];
// Options are now rendered declaratively via <template x-for> over
// RADIO_PRESETS inside the #rd-preset markup. No imperative builder
// needed; kept as a no-op so legacy callers don't NPE.
function findMatchingPreset() {
  const f = state.forms.radio;
  return RADIO_PRESETS.find(p =>
    p.freq === f.freq && p.bw === f.bw && p.sf === f.sf && p.cr === f.cr && p.tx === f.tx
    && p.airtime === f.airtime && p.lt_airtime === f.airtimeLt
  );
}
// Called from @change on #rd-preset. If the user picks a region, copy its
// params into state.forms.radio so the (hidden) raw fields stay in sync if
// they later switch to Custom. If they pick Custom, the existing values
// stay - the form just becomes visible. The "saved" notebook is shown
// declaratively via x-text on $store.s.ui.rdPresetNote.
function applyRadioPreset() {
  const id = state.forms.radio.preset;
  if (id === '__empty')  { state.ui.rdPresetNote = ''; return; }
  if (id === '__custom') { state.ui.rdPresetNote = ''; return; }
  const p = RADIO_PRESETS.find(x => x.id === id);
  if (!p) return;
  Object.assign(state.forms.radio, {
    freq: p.freq, bw: p.bw, sf: p.sf, cr: p.cr, tx: p.tx,
    airtime: p.airtime, airtimeLt: p.lt_airtime,
  });
  state.ui.rdPresetNote = p.note;
}

// ============================== SETTINGS MODAL ==============================
// Single tabbed modal hosts every configuration screen. Tabs: identity,
// radio, ui (App), network, danger. firstRun=true locks the modal to the
// radio tab and hides everything else until the radio is saved.

async function populateRadioTab() {
  // Reset form to "no preset chosen"; raw inputs hide automatically via
  // x-show on $store.s.forms.radio.preset === '__custom'.
  Object.assign(state.forms.radio, {
    preset: '__empty',
    freq: 0, bw: 0, sf: 0, cr: 0, tx: 0, airtime: 0, airtimeLt: 0,
  });
  state.ui.rdPresetNote = '';
  try {
    const r = await state.transport.getRadio();
    if (r.have_conf) {
      Object.assign(state.forms.radio, {
        freq: r.frequency || 0,
        bw:   r.bandwidth || 0,
        sf:   r.spreading_factor || 0,
        cr:   r.coding_rate || 0,
        tx:   r.tx_power || 0,
        airtime:   r.airtime_limit_pct ?? 0,
        airtimeLt: r.longterm_airtime_limit_pct ?? 0,
      });
      // If the loaded values match a known preset, select it (which keeps
      // the raw fields hidden); otherwise drop to Custom so the user can
      // see exactly what's loaded.
      const match = findMatchingPreset();
      state.forms.radio.preset = match ? match.id : '__custom';
      state.ui.rdPresetNote = match ? match.note : '';
    }
  } catch (e) {
    toast('Could not read radio state: ' + (e.message || e), 'error');
  }
  try {
    const d = await state.transport._req(API.LORA_DISCOVERABLE);
    state.forms.radio.discoverable = !!d.discoverable;
  } catch { /* leave toggle in last state */ }
  await populateLoraConfig();
  // Header dot reflects the radio's live state, not the discoverable
  // flag - "is the LoRa interface up and able to send/receive?", not
  // "are we advertising it?".
  const radio = (state.lastInfo && state.lastInfo.radio) || {};
  setConnectivityDot('cn-dot-radio',
    radio.online ? 'ok' :
    radio.have_conf ? 'err' : 'warn');
}

// ============== INFO ICON / CLICK TOOLTIP ==============
// Generic explainer popover. Click an `.info-icon` button to set
// state.infoTip = { html, x, y } - the declarative .info-tooltip
// element in body markup picks up the position + content.
function _closeInfoTooltip() { state.infoTip = null; }
document.addEventListener('click', (ev) => {
  const btn = ev.target.closest('.info-icon');
  if (btn) {
    ev.preventDefault();
    ev.stopPropagation();
    const html = btn.dataset.info || '';
    // Toggle: re-click closes.
    if (state.infoTip && state.infoTip.html === html) {
      _closeInfoTooltip();
      return;
    }
    const r = btn.getBoundingClientRect();
    // Tooltip width/height are unknown until rendered - guess 280×60
    // (matches CSS max-width); the position adjusts on next frame
    // when the actual rect is known via x-effect inside the tooltip
    // ... we skip that and just clamp generously. Browsers honor the
    // first paint position; the user can tolerate a few px drift if
    // the body is unusually long.
    const w = 280, h = 60;
    let left = r.left + r.width / 2 - w / 2;
    let top  = r.bottom + 6;
    if (left + w > window.innerWidth  - 8) left = window.innerWidth  - w - 8;
    if (left < 8) left = 8;
    if (top  + h > window.innerHeight - 8) top  = r.top - h - 6;
    if (top  < 8) top  = 8;
    state.infoTip = { html, x: left, y: top };
    return;
  }
  if (state.infoTip) {
    // Click outside the .info-tooltip closes.
    let n = ev.target;
    while (n && n !== document.body) {
      if (n.classList && n.classList.contains('info-tooltip')) return;
      n = n.parentNode;
    }
    _closeInfoTooltip();
  }
}, true);
document.addEventListener('keydown', (ev) => { if (ev.key === 'Escape') _closeInfoTooltip(); });
window.addEventListener('scroll', _closeInfoTooltip, true);
// Toggle handler for #rd-discoverable. Enabling requires an identity
// code; disabling just POSTs. On error or cancel we flip the form-state
// boolean back, which Alpine reflects through x-model.
function onChangeRdDiscoverable(el) {
  const wantOn = el.checked;
  // The switch shows the SAVED state only: snap the visual back until
  // the server confirms the change. Enabling first needs the identity
  // code, and a dismissed code prompt must leave the toggle off.
  el.checked = state.forms.radio.discoverable;
  const apply = async (code) => {
    const body = { discoverable: wantOn };
    if (code) body.identity_code = code;
    try {
      await state.transport._req(API.LORA_DISCOVERABLE,
        { method: 'POST', body });
      state.forms.radio.discoverable = wantOn;
      toast('LoRa interface ' + (wantOn ? 'is now publicly advertised' : 'is no longer advertised'));
    } catch (e) {
      toast('Could not save: ' + (e.message || e), 'error');
    }
  };
  if (wantOn) {
    state.pendingIdCodeAction = (code) => { if (code) apply(code); };
    openIdCodeModal();
  } else {
    apply();
  }
}

// Load the LoRa interface mode + IFAC state into the form. Called from
// populateRadioTab alongside the discoverable fetch. The netkey is a
// secret and is never returned, so ifacNetkey always starts blank; a
// blank key on save keeps the stored one.
async function populateLoraConfig() {
  try {
    const c = await state.transport._req(API.LORA_CONFIG);
    const ifac = c.ifac || {};
    Object.assign(state.forms.loraCfg, {
      mode:               c.mode || 'gateway',
      modeDefault:        !!c.mode_default,
      ifacNetname:        ifac.netname || '',
      ifacNetkey:         '',
      ifacSize:           Number(ifac.size_bits || 0),
      ifacConfigured:     !!ifac.configured,
      ifacConfiguredSize: Number(ifac.size_bits || 0),
    });
    state.ui.cnLoraCfgStatus = '';
  } catch (e) {
    // Leave whatever defaults the form holds; surface the reason quietly.
    state.ui.cnLoraCfgStatus = 'Could not read mode/IFAC: ' + (e.message || e);
  }
}

// Save mode + IFAC. Mirrors onChangeRdDiscoverable's physical-presence
// flow: enabling IFAC or otherwise touching the interface may need an
// identity code, so we POST first and, if the server rejects with a
// presence error, retry with a code. mode/IFAC changes take effect on
// the next reboot, so the caption + toast say so when the server
// reports reboot_required.
function onClickLoraCfgSave(btn) {
  const f = state.forms.loraCfg;
  const body = { mode: f.mode };
  const size = Number(f.ifacSize || 0);
  const netname = (f.ifacNetname || '').trim();
  if (size > 0 && netname) {
    // IFAC on: send name + size. Only send a key if the user typed one
    // (blank keeps the existing key on edit).
    body.ifac_netname = netname;
    body.ifac_size    = size;
    if (f.ifacNetkey) body.ifac_netkey = f.ifacNetkey;
    // Enabling IFAC fresh requires a key.
    if (!f.ifacConfigured && !f.ifacNetkey) {
      state.ui.cnLoraCfgStatus = 'A network key is required to enable IFAC.';
      return;
    }
  } else {
    // IFAC off / cleared.
    body.ifac_size    = 0;
    body.ifac_netname = '';
  }
  return withBusy(btn, async () => {
    state.ui.cnLoraCfgStatus = '';
    const apply = async (code) => {
      const payload = code ? Object.assign({ identity_code: code }, body) : body;
      const r = await state.transport._req(API.LORA_CONFIG,
        { method: 'POST', body: payload });
      const reboot = !!r.reboot_required;
      const msg = reboot
        ? 'Saved. The change applies after a reboot.'
        : 'Saved.';
      state.ui.cnLoraCfgStatus = msg;
      toast(reboot ? 'LoRa config saved. Reboot the device to apply.' : 'LoRa config saved');
      // Refresh so ifacConfigured / modeDefault reflect the new state
      // and the key field shows "unchanged" next time.
      await populateLoraConfig();
    };
    try {
      await apply();
    } catch (e) {
      // 403 from the presence guard => retry once with an identity code.
      if (e.status === 403) {
        await new Promise((resolve) => {
          state.pendingIdCodeAction = async (code) => {
            if (!code) { resolve(); return; }
            try { await apply(code); }
            catch (e2) { state.ui.cnLoraCfgStatus = 'Save failed: ' + (e2.message || e2); }
            resolve();
          };
          openIdCodeModal();
        });
      } else {
        state.ui.cnLoraCfgStatus = 'Save failed: ' + (e.message || e);
        toast('Could not save LoRa config: ' + (e.message || e), 'error');
      }
    }
  }, { label: 'Saving…' });
}

async function populateIdentityTab() {
  try {
    const me = await state.transport.getIdentity(state.identityId);
    state.self = Object.assign({}, state.self || {}, me);
    const ms = Number(me.announce_interval_ms || 0);
    const exact = ANNOUNCE_INTERVALS.find(i => i.ms === ms);
    Object.assign(state.forms.identity, {
      displayName: me.display_name || '',
      announceInterval: exact
        ? String(exact.ms)
        : String(ANNOUNCE_INTERVALS.reduce((a, b) =>
            Math.abs(b.ms - ms) < Math.abs(a.ms - ms) ? b : a).ms),
      persistOutbound: me.persist_outbound_attachments !== false,
      stampCost: String(me.stamp_cost ?? 0),
      enforceStamps: !!me.enforce_stamps,
      screen: !!me.screen,
      msgFlash: me.msg_flash !== false,
      telLocation:    !me.telemetry || me.telemetry.location !== false,
      telEnvironment: !!(me.telemetry && me.telemetry.environment),
      telBattery:     !!(me.telemetry && me.telemetry.battery),
      telCompass:     !!(me.telemetry && me.telemetry.compass),
      telShare:       String((me.telemetry && me.telemetry.share_s) || 0),
      telRate:        String((me.telemetry && me.telemetry.rate_s) || 60),
    });
    populateMessengerPresets();
    // Remember the server-side last-saved name so onBlurIdName can no-op
    // if the user blurred without actually editing. Stored on the form
    // object (not in state.ui) because it's submit-state, not display.
    state.forms.identity._lastSavedDisplayName = me.display_name || '';
    // Resync the topbar's announce-countdown anchor based on this fetch.
    seedAnnounceCountdown(me.next_announce_in_ms || 0, ms);
  } catch (err) {
    console.warn('failed to fetch identity:', err);
  }
}

// Edge affordances on the scrolling tab strip: show the left/right fade +
// chevron only when the strip can scroll further that way. Bound to the
// strip's scroll event and called on select / open / resize. A 1px slack
// keeps it from flickering at the exact ends; a hidden strip (modal shut)
// reports 0 width and resolves to both edges off.
function updateTabEdges() {
  const bar = document.getElementById('settings-tabs');
  if (!bar) return;
  const max = bar.scrollWidth - bar.clientWidth;
  state.ui.settingsTabEdges.left  = bar.scrollLeft > 1;
  state.ui.settingsTabEdges.right = bar.scrollLeft < max - 1;
}
// Nudge the strip about one near-screenful in `dir` (-1 left, +1 right)
// when an edge chevron is tapped; on touch the strip also swipes natively.
function scrollTabs(dir) {
  const bar = document.getElementById('settings-tabs');
  if (bar) bar.scrollBy({ left: dir * Math.round(bar.clientWidth * 0.7), behavior: 'smooth' });
}
// A viewport resize (e.g. phone rotation while Settings is open) changes the
// strip's overflow, so re-evaluate the edges. No-ops when the modal is shut.
window.addEventListener('resize', updateTabEdges);

// Keep the active settings tab visible in the scrolling tab strip: scroll
// it toward the strip's centre so its neighbours on both sides stay
// reachable. rAF lets the modal finish laying out on first open (a hidden
// strip reports 0 width); the leftmost tab just resolves to scrollLeft 0.
// scrollTo clamps to the scrollable range, so the ends never over-scroll.
function _centerSettingsTab(tabId) {
  requestAnimationFrame(() => {
    const bar = document.getElementById('settings-tabs');
    if (!bar) return;
    const btn = bar.querySelector('.tab[data-tab="' + tabId + '"]');
    if (btn) {
      const target = btn.offsetLeft - (bar.clientWidth - btn.offsetWidth) / 2;
      bar.scrollTo({ left: Math.max(0, target), behavior: 'smooth' });
    }
    updateTabEdges();
  });
}

function selectSettingsTab(tabId) {
  // Visual state is reactive via $store.s.settingsTab + the :class
  // bindings on each #settings-tabs > .tab and #modal-settings
  // .tab-panel. This function still handles the per-tab data load
  // (populateXxxTab) which the legacy code coupled to the tab click.
  state.settingsTab = tabId;
  _centerSettingsTab(tabId);
  // Stop the time-tab ticker if we're leaving it; populateTimeTab
  // restarts it when we re-enter.
  if (tabId !== 'time') stopTimeTicker();
  // Lazy-populate per-tab data so the modal opens snappily.
  if (tabId === 'connectivity') { applyTransportCapabilities(); populateRadioTab(); populateWifiTab(); populateTcpClientsList(); populateDiagTab(); }
  if (tabId === 'discovery')    { populateDiscoveryTab(); }
  if (tabId === 'time')         { populateTimeTab(); }
  if (tabId === 'identity')     { populateIdentityTab(); }
  if (tabId === 'telemetry')    { populateCollectorConfig(); }
  if (tabId === 'power')        { populatePowerTab(); }
  if (tabId === 'propagation')  { populatePropagationTab(); }
  if (tabId === 'map')          { fetchMapConfig(); fetchMapLayers(); refreshMapDownload(); refreshMapExtract(); }
  if (tabId === 'network')      { renderAnnounces(); renderPaths(); }
  if (tabId === 'ui')           {
    state.forms.prefs.enterSends                = !!state.prefs.enterSends;
    state.forms.prefs.autoDownloadAttachments   = !!state.prefs.autoDownloadAttachments;
    renderContacts(); populateInboxConfig(); populateStorageConfig();
  }
}

async function populateWifiTab() {
  populateSavedWifiList();
  // Header dot: green when joined to a network (station mode + connected),
  // grey otherwise (softAP / not connected). Nothing was setting this, so
  // it stayed on the grey default even when WiFi was up.
  const w = (state.lastInfo && state.lastInfo.wifi) || {};
  setConnectivityDot('cn-dot-wifi', (w.mode === 'sta' && w.connected) ? 'ok' : 'idle');
}

function populateDiagTab() {
  state.forms.diag.kissOut = !!(state.lastInfo && state.lastInfo.kiss_serial_output);
}

// Hide/show Connectivity accordion sections based on what the firmware
// was actually built with. The transports capability dict comes from
// /api/info → transports.{lora,tcp_client,tcp_server,udp,bluetooth}.
// Sections for absent transports are hidden so users can't open
// configuration that can't take effect. Visibility is driven through
// reactive state - the <details> elements bind x-show against these.
function applyTransportCapabilities() {
  const t = (state.lastInfo && state.lastInfo.transports) || {};
  state.ui.showRadioSection = t.lora !== false;
  state.ui.showTcpSection   = !!t.tcp_client;
  state.ui.showBtSection    = !!t.bluetooth;
}

// Render the TCP-clients list inside the Connectivity tab. Each row
// shows host:port, an online dot, a discoverable checkbox, and a
// remove button. Online dot mirrors the firmware's per-client
// runtime status from /api/transport/tcp_clients.
async function populateTcpClientsList() {
  const t = (state.lastInfo && state.lastInfo.transports) || {};
  if (!t.tcp_client) return;
  state.tcpClients = null;  // template shows "Loading…"
  try {
    const r = await state.transport._req(API.TCP_CLIENTS);
    state.tcpClients     = r.clients || [];
    state.tcpClientsCap  = r.capacity   ?? 0;
    state.tcpClientsLive = r.live_count ?? 0;
    // grey: none configured · green: all connected · yellow: some but not
    // all · red: configured but none connected. ('idle' has no colour rule,
    // so it falls back to the base grey dot - and stays visible, unlike '').
    let headerCls = 'idle';
    if (state.tcpClients.length > 0) {
      headerCls = state.tcpClientsLive === state.tcpClients.length ? 'ok'
                : state.tcpClientsLive > 0                          ? 'warn'
                : 'err';
    }
    setConnectivityDot('cn-dot-tcp', headerCls);
  } catch (e) {
    state.tcpClients = [];
    toast('Could not load TCP clients: ' + (e.message || e), 'error');
  }
}
// Connectivity-tab status dots. Maps each accordion header's <span
// id="cn-dot-…"> to a key on state.ui (cnDotRadio / cnDotWifi /
// cnDotTcp / cnDotBle / cnDotSerial). Empty string = hidden.
const CN_DOT_KEYS = {
  'cn-dot-radio':  'cnDotRadio',
  'cn-dot-wifi':   'cnDotWifi',
  'cn-dot-tcp':    'cnDotTcp',
  'cn-dot-bt':     'cnDotBle',
  'cn-dot-serial': 'cnDotSerial',
};
function setConnectivityDot(id, cls) {
  const key = CN_DOT_KEYS[id];
  if (!key) return;
  state.ui[key] = cls || '';
}
async function onTcpRemove(client) {
  const ok = await showConfirm({
    title: 'Remove TCP client?',
    body: `Disconnect and forget ${client.name} (${client.host}:${client.port})?`,
    okLabel: 'Remove', destructive: true,
  });
  if (!ok) return;
  try {
    await state.transport._req(
      API.TCP_CLIENT(client.name),
      { method: 'DELETE' });
    populateTcpClientsList();
  } catch (e) {
    toast('Remove failed: ' + (e.message || e), 'error');
  }
}

function openAddTcpModal() {
  Object.assign(state.forms.tcpAdd, { name: '', host: '', port: 4242 });
  state.ui.cnTcpStatus = '';
  state.modals.tcpAdd = true;
}
async function onClickTcpSave(_btn) {
  const f = state.forms.tcpAdd;
  const name = (f.name || '').trim();
  const host = (f.host || '').trim();
  const port = Number(f.port);
  if (!name || !host || !(port > 0 && port < 65536)) {
    state.ui.cnTcpStatus = 'Name, host, and a valid port (1–65535) are required.';
    return;
  }
  // discoverable is never sent: a TCP client is outbound-only, so an
  // announce telling peers "this device is at host:port" would be
  // wrong (peers can't connect that way).
  try {
    await state.transport._req(API.TCP_CLIENTS, { method: 'POST', body: { name, host, port } });
    state.modals.tcpAdd = false;
    populateTcpClientsList();
    toast('TCP client added');
  } catch (e) {
    state.ui.cnTcpStatus = 'Failed: ' + (e.message || e);
  }
}

// ============================== TIME SETTINGS ==============================
// LXMF needs a real wall clock for the message-timestamp field. The
// device tracks several sources (GPS, NTP, Browser, RNS peer) and
// picks the highest-priority enabled one. Hardware RTC seeds the
// clock at boot but isn't a user-visible source. (#111, #113)
//
// User reorders sources by dragging rows. The displayed order IS the
// priority order - index 0 = priority 0 (highest), etc.
const TIME_SOURCE_LABELS = {
  gps:     ['GPS',      'UTC from a GPS fix; works without internet.'],
  ntp:     ['NTP',      'pool.ntp.org sync when WiFi has an internet route.'],
  browser: ['Browser',  'Time the SPA pushed from your phone/laptop clock.'],
  rns:     ['RNS peer', 'Wall clock from an LXMF peer announce. Lowest trust, last-resort.'],
};
// Sources for which a poll interval is meaningful (event-driven
// sources hide the field).
// GPS is configured from its sensor row in the system popover (it's
// the only time source that's also a physical sensor). The Time tab
// keeps the priority/enable knob for GPS but hides the interval
// dropdown to avoid having two homes for the same value.
// GPS's interval here is the clock-sync "Refresh": how often a live fix
// may resync the clock. Location power lives in the sensors popover.
const TIME_SOURCE_HAS_INTERVAL = { gps: true, ntp: true, browser: false, rns: false };

// Live-ticking clock state. Anchor = the moment we last got an
// authoritative reading from the device, captured as (deviceUnixMs,
// browserPerformanceNow). A 1 s interval extrapolates the current
// time from the anchor without further polls.
const timeTicker = { anchor: null, timer: null };

function tickTimeClock() {
  const a = timeTicker.anchor;
  if (!a) return;
  const nowMs = a.deviceUnixMs + (performance.now() - a.localNowMs);
  state.ui.tmCurrent = fmtWallClock(nowMs);
}

function startTimeTicker() {
  if (timeTicker.timer) return;
  timeTicker.timer = setInterval(tickTimeClock, 1000);
}

function stopTimeTicker() {
  if (timeTicker.timer) { clearInterval(timeTicker.timer); timeTicker.timer = null; }
}

async function populateTimeTab() {
  try {
    const t = await state.transport._req(API.TIME);
    renderTimeStatus(t);
    renderTimeSources(t.sources || {});
    startTimeTicker();
  } catch (e) {
    state.ui.tmCurrent = 'unavailable';
    state.ui.tmSource  = e.message || 'failed to fetch';
    stopTimeTicker();
  }
}

function renderTimeStatus(t) {
  if (t && t.calibrated && t.unix_ms) {
    timeTicker.anchor = {
      deviceUnixMs: Number(t.unix_ms),
      localNowMs:   performance.now(),
    };
    state.ui.tmCurrent = fmtWallClock(Number(t.unix_ms));
    state.ui.tmSource  = 'from ' + (t.source || 'unknown');
  } else {
    timeTicker.anchor = null;
    state.ui.tmCurrent = 'uncalibrated';
    state.ui.tmSource  = 'no source has reported yet';
  }
}

function renderTimeSources(sources) {
  // Canonical fallback order when priorities tie. NTP comes first
  // because it's the cheapest, most-reliable source when WiFi has an
  // internet route - see TimeManager::default_config. GPS sits a step
  // behind for offline-only deployments.
  const order = ['ntp', 'gps', 'browser', 'rns'];
  const rows = order.map(key => ({ key, cfg: sources[key] || { enabled: true, priority: 99, interval_s: 0 } }));
  rows.sort((a, b) => {
    const pa = (a.cfg.priority != null) ? a.cfg.priority : 99;
    const pb = (b.cfg.priority != null) ? b.cfg.priority : 99;
    if (pa !== pb) return pa - pb;
    return order.indexOf(a.key) - order.indexOf(b.key);
  });
  // Replace the reactive list in one swap so Alpine repaints the
  // <template x-for> in #tm-sources.
  state.timeSources = rows.map(({ key, cfg }) => {
    const [label, desc] = TIME_SOURCE_LABELS[key];
    const hasInterval   = !!TIME_SOURCE_HAS_INTERVAL[key];
    const interval_s    = hasInterval ? snapToPreset(cfg.interval_s) : 0;
    return { key, label, desc, hasInterval,
             enabled: !!cfg.enabled, interval_s, expanded: false };
  });
}

// Interval-dropdown presets. 0 = "at boot": driver reports once and
// never repolls. Otherwise = seconds between repolls. We snap the
// loaded value to the nearest preset so non-standard configs from
// older firmware degrade sensibly.
const INTERVAL_PRESETS = [
  { value: 0,       label: 'At boot' },
  { value: 3600,    label: 'Every hour' },
  { value: 86400,   label: 'Every day' },
  { value: 604800,  label: 'Every week' },
];
function snapToPreset(secs) {
  if (secs == null) return 0;
  // Snap to whichever preset's log distance is smallest.
  let best = INTERVAL_PRESETS[0], bestDist = Infinity;
  for (const p of INTERVAL_PRESETS) {
    const a = Math.max(secs, 1), b = Math.max(p.value, 1);
    const d = Math.abs(Math.log(a) - Math.log(b));
    if (d < bestDist) { best = p; bestDist = d; }
  }
  return best.value;
}


// Drag-and-drop: minimal HTML5-DnD reorder. Drop above/below the
// hovered row based on midpoint. Save button picks up the resulting
// DOM order on submit and turns it into priorities 0..N-1.
function attachTimeSourceDnD(row) {
  row.addEventListener('dragstart', (ev) => {
    row.style.opacity = '0.4';
    ev.dataTransfer.effectAllowed = 'move';
    ev.dataTransfer.setData('text/source', row.dataset.source);
  });
  row.addEventListener('dragend', () => { row.style.opacity = '1'; });
  row.addEventListener('dragover', (ev) => {
    ev.preventDefault();
    ev.dataTransfer.dropEffect = 'move';
  });
  row.addEventListener('drop', (ev) => {
    ev.preventDefault();
    const src = ev.dataTransfer.getData('text/source');
    const dst = row.dataset.source;
    if (!src || src === dst) return;
    const rect = row.getBoundingClientRect();
    const above = (ev.clientY - rect.top) < (rect.height / 2);
    // Reorder state.timeSources - Alpine x-for repaints from the new
    // array order, so the visual move matches without a manual DOM
    // insertBefore (which Alpine could undo on its next render pass).
    const list = state.timeSources;
    const srcIdx = list.findIndex(r => r.key === src);
    const dstIdx = list.findIndex(r => r.key === dst);
    if (srcIdx < 0 || dstIdx < 0) return;
    const [moved] = list.splice(srcIdx, 1);
    const insertAt = above ? (srcIdx < dstIdx ? dstIdx - 1 : dstIdx)
                           : (srcIdx < dstIdx ? dstIdx     : dstIdx + 1);
    list.splice(insertAt, 0, moved);
  });
}

// Bound via @click on #btn-tm-browser-sync in the Time tab.
function onClickTmBrowserSync(btn) {
  return withBusy(btn, async () => {
    try {
      const r = await state.transport._req(API.TIME,
        { method: 'POST', body: { unix_ms: Date.now() } });
      if (r.adopted) {
        toast('Device clock set from browser', 'success');
      } else if (r.calibrated) {
        toast('Browser time recorded but a higher-priority source is in use (' + r.source + ')', 'info');
      } else {
        toast('Browser sync did not take effect. Check that Browser is enabled in the source list', 'error');
      }
      renderTimeStatus(r);
    } catch (e) {
      toast('Sync failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Syncing…' });
}

// Bound via @click on #btn-tm-save in the Time tab.
function onClickTmSave(btn) {
  return withBusy(btn, async () => {
  // Walk state.timeSources in its current order - that's the user's
  // preferred priority sequence (drag-and-drop mutates the array).
  // Index = priority (0 = highest).
  const body = { sources: {} };
  state.timeSources.forEach((r, idx) => {
    const cfg = { enabled: !!r.enabled, priority: idx };
    if (r.hasInterval) cfg.interval_s = Math.max(0, parseInt(r.interval_s, 10) || 0);
    body.sources[r.key] = cfg;
  });
  try {
    const r = await state.transport._req(API.TIME_SOURCES,
      { method: 'POST', body });
    toast('Time-source config saved', 'success');
    renderTimeStatus(r);
    renderTimeSources(r.sources || {});
  } catch (e) {
    toast('Save failed: ' + (e.message || e), 'error');
  }
  }, { label: 'Saving…' });
}

// Bound @change on #cn-kiss-out in the Connectivity → Serial/diagnostics
// section. The checkbox writes state.forms.diag.kissOut via x-model; we
// then POST and surface a toast, or roll the state back on failure.
async function onChangeKissOut() {
  const enabled = state.forms.diag.kissOut;
  try {
    await state.transport._req(API.SYSTEM_KISS,
        { method: 'POST', body: { enabled } });
    if (state.lastInfo) state.lastInfo.kiss_serial_output = enabled;
    toast(enabled ? 'KISS serial output ON' : 'KISS serial output OFF', 'success');
  } catch (e) {
    state.forms.diag.kissOut = !enabled;  // revert
    toast('Toggle failed: ' + (e.message || e), 'error');
  }
}

async function populateSavedWifiList() {
  state.wifiSaved = null;
  try {
    const r = await state.transport.wifiSavedList();
    state.wifiSaved = r.networks || [];
  } catch (e) {
    state.wifiSaved = [];
    toast('Could not load saved networks: ' + (e.message || e), 'error');
  }
}

async function forgetSavedNetwork(ssid) {
  const r = await showConfirm({
    title: 'Forget "' + ssid + '"?',
    body: 'The device will reboot into softAP so you can join a different network.',
    okLabel: 'Forget',
    destructive: true,
    needsCode: true,
  });
  if (!r) return;
  try {
    await state.transport.wifiForget(ssid, r.code);
    waitForDeviceAndReload('Forgetting "' + ssid + '", rebooting into softAP…');
  } catch (e) {
    toast('Forget failed: ' + (e.message || e), 'error');
  }
}

// firstRun=true: must-configure-radio gate. Hide every non-connectivity
// tab, hide the close button, suppress dismissal handlers.
async function openSettingsModal(initialTab, firstRun) {
  state.modals.settings = true;
  state.settingsFirstRun = !!firstRun;
  // Title + tab visibility
  state.ui.settingsTitle = firstRun ? 'Set up your radio' : 'Settings';
  // Close-button visibility is bound x-show on the markup; reflect
  // firstRun into state so the binding picks it up.
  state.ui.showCloseSettings = !firstRun;
  // Treat the legacy 'radio' tab name as a synonym for 'connectivity'
  // - callers that open the radio gate still work.
  if (initialTab === 'radio') initialTab = 'connectivity';
  // First-run lock: while the device is in its initial radio-setup
  // gate, only the Connectivity tab is reachable. Stored on state so
  // the x-for'd tab markup can bind :hidden.
  state.settingsFirstRun = firstRun;
  // The #rd-intro paragraph binds x-html to $store.s.settingsFirstRun
  // and renders the right copy for first-run vs subsequent edits.
  selectSettingsTab(initialTab || 'identity');
}

function closeSettingsModal() {
  if (state.settingsFirstRun) return;  // locked until radio is saved
  state.modals.settings = false;
  stopTimeTicker();
}

// Tab-bar click → switch panel. Wiring lives on the Alpine
// <template x-for> @click in markup.
// Close button + backdrop click both dismiss (when not first-run) -
// both bound declaratively now (@click on #btn-close-settings + the
// backdrop's @click.self on #modal-settings below in the markup).

// ============================== SETTINGS - auto-announce interval ==============================
// Sensible auto-announce intervals. 0 disables. The min the server
// will accept is 10s - anything below silently rounds up. The default
// is 5 minutes (LXMF_DEFAULT_ANNOUNCE_INTERVAL_MS server-side).
const ANNOUNCE_INTERVALS = [
  { ms: 0,        label: 'Off: manual only' },
  { ms: 60000,    label: 'Every 1 minute' },
  { ms: 300000,   label: 'Every 5 minutes' },
  { ms: 900000,   label: 'Every 15 minutes' },
  { ms: 1800000,  label: 'Every 30 minutes' },
  { ms: 3600000,  label: 'Every hour' },
  { ms: 21600000, label: 'Every 6 hours' },
  { ms: 86400000, label: 'Every 24 hours' },
];
// Options rendered declaratively via <template x-for> over
// ANNOUNCE_INTERVALS inside #id-announce-interval. Kept as a no-op
// for any external callers.
function populateIntervalSelect() { /* declarative */ }

// Settings → Identity tab change handlers, all bound via @change /
// @blur on the markup. They write straight to the server and surface a
// toast on completion - reading the new values back into state on the
// next populateIdentityTab() pass is what keeps form state in sync.
async function onChangeAnnounceInterval() {
  const ms = Number(state.forms.identity.announceInterval);
  try {
    await state.transport.setIdentitySettings(state.identityId, { announce_interval_ms: ms });
    if (state.self) state.self.announce_interval_ms = ms;
    toast(ms === 0 ? 'Auto-announce disabled' : 'Auto-announce updated', 'success');
  } catch (err) {
    toast('Could not update setting: ' + (err.message || err), 'error');
  }
}

// Settings -> Identity: flash the charge LED on a new message for the screen
// identity (per-identity; saved via setIdentitySettings).
async function onChangeMsgFlash() {
  const want = !!state.forms.identity.msgFlash;
  try {
    await state.transport.setIdentitySettings(state.identityId, { msg_flash: want });
    if (state.self) state.self.msg_flash = want;
    toast(want ? 'Message LED flash on' : 'Message LED flash off', 'success');
  } catch (err) {
    toast('Could not update setting: ' + (err.message || err), 'error');
    state.forms.identity.msgFlash = !want;
  }
}

// ---- Settings -> Power tab ----
// The Power panel's selects use static <option>s (string values); the value
// mapping to/from the /api/power numeric fields lives in the two functions
// below. Map an /api/power response onto the UI-facing select/toggle fields.
function _powerApply(r) {
  const f = state.forms.power;
  f.blankSel  = (r.blank_enabled === false) ? 'off' : String(r.blank_timeout_s);
  f.wakeSel   = (r.wake_threshold_mg <= 80)  ? 'high'
              : (r.wake_threshold_mg <= 150) ? 'medium' : 'low';
  f.heartbeat = (r.heartbeat_enabled !== false);
  f.gpsSel    = String(r.gps_motion_retry_s != null ? r.gps_motion_retry_s : 0);
}
async function populatePowerTab() {
  try { _powerApply(await state.transport._req(API.POWER_CONFIG)); }
  catch (e) { /* keep last-known values */ }
}
async function savePowerConfig() {
  const f = state.forms.power;
  const body = {
    heartbeat_enabled:  !!f.heartbeat,
    wake_threshold_mg:  f.wakeSel === 'high' ? 60 : (f.wakeSel === 'low' ? 200 : 120),
    gps_motion_retry_s: parseInt(f.gpsSel, 10) || 0,
  };
  if (f.blankSel === 'off') { body.blank_enabled = false; }
  else { body.blank_enabled = true; body.blank_timeout_s = parseInt(f.blankSel, 10); }
  try {
    _powerApply(await state.transport._req(API.POWER_CONFIG, { method: 'POST', body }));
    toast('Power settings saved', 'success');
  } catch (e) {
    toast('Could not save: ' + (e.message || e), 'error');
    await populatePowerTab();
  }
}

// ---- Settings -> Propagation tab ----
// Maps the /api/propagation response onto the UI form. The node picker shows
// the discovered registry (nodes[]) plus a free-text address field; both edit
// pnHash. sync_interval_s collapses to a "Manual / every N" select.
function _propApply(r) {
  const f = state.forms.propagation;
  f.enabled = !!r.enabled;
  f.pnHash  = r.pn_hash || '';
  f.syncSel = r.sync_interval_s ? String(r.sync_interval_s) : 'manual';
  f.retain  = !!r.retain_on_node;
  f.offline = (r.use_when_offline !== false);
  f.nodes   = Array.isArray(r.nodes) ? r.nodes : [];
  f.sync    = r.sync || {};
}
function propAge(s) {
  if (s == null || s < 0) return '';
  if (s < 60)   return s + 's ago';
  if (s < 3600) return Math.round(s / 60) + 'm ago';
  return Math.round(s / 3600) + 'h ago';
}
function propSyncStatus() {
  const s = (state.forms.propagation && state.forms.propagation.sync) || {};
  if (s.state === 'connecting') return 'Connecting to the node';
  if (s.state === 'syncing')    return 'Syncing';
  if (s.state === 'complete')   return 'Synced ' + (s.last_received || 0) + ' message(s)';
  if (s.state === 'failed')     return 'Sync failed' + (s.last_error ? ': ' + s.last_error : '');
  if (s.last_sync_age_s != null && s.last_sync_age_s >= 0)
    return 'Last sync: ' + (s.last_received || 0) + ' message(s), ' + propAge(s.last_sync_age_s);
  return 'Not synced yet';
}
async function propSyncNow() {
  try {
    await state.transport._req(API.PROPAGATION_SYNC, { method: 'POST' });
    toast('Sync started', 'success');
  } catch (e) {
    toast('Could not sync: ' + (e.message || e), 'error');
  }
  // Poll the status a few times so the line reflects progress then the result.
  for (let i = 0; i < 8; i++) { await new Promise(r => setTimeout(r, 2500)); await populatePropagationTab(); }
}
async function populatePropagationTab() {
  try { _propApply(await state.transport._req(API.PROPAGATION_CONFIG)); }
  catch (e) { /* keep last-known values */ }
}
async function savePropagationConfig() {
  const f = state.forms.propagation;
  const body = {
    enabled:          !!f.enabled,
    pn_hash:          (f.pnHash || '').trim().toLowerCase(),
    sync_interval_s:  f.syncSel === 'manual' ? 0 : (parseInt(f.syncSel, 10) || 0),
    retain_on_node:   !!f.retain,
    use_when_offline: !!f.offline,
  };
  try {
    _propApply(await state.transport._req(API.PROPAGATION_CONFIG, { method: 'POST', body }));
    toast('Propagation settings saved', 'success');
  } catch (e) {
    toast('Could not save: ' + (e.message || e), 'error');
    await populatePropagationTab();
  }
}

// Telemetry-attach defaults (Settings → Identity). Saved as one
// object on any control change; the compose popover seeds from
// state.self.telemetry, so mirror the save there too.
async function onChangeTelemetryDefaults() {
  const f = state.forms.identity;
  const telemetry = {
    location:    !!f.telLocation,
    environment: !!f.telEnvironment,
    battery:     !!f.telBattery,
    compass:     !!f.telCompass,
    share_s:     Number(f.telShare || 0),
    rate_s:      Number(f.telRate || 60),
  };
  try {
    await state.transport.setIdentitySettings(state.identityId, { telemetry });
    if (state.self) state.self.telemetry = telemetry;
    toast('Telemetry defaults updated', 'success');
  } catch (err) {
    toast('Could not update setting: ' + (err.message || err), 'error');
  }
}

// Save the display name on blur (or Enter). No save on every keystroke
// - peers don't see the new label until the next announce anyway, so
// per-keystroke API calls would just churn the meta.json write. The
// input's @keydown.enter blurs the field, which triggers this.
async function onBlurIdName() {
  const desired = (state.forms.identity.displayName || '').trim();
  const last    = state.forms.identity._lastSavedDisplayName || '';
  if (desired === last) return;
  if (!desired) {
    state.forms.identity.displayName = last;  // refuse empty; restore prior
    toast('Display name cannot be empty', 'error');
    return;
  }
  try {
    const resp = await state.transport.setIdentitySettings(
      state.identityId, { display_name: desired });
    const saved = resp.display_name || desired;
    state.forms.identity._lastSavedDisplayName = saved;
    state.forms.identity.displayName           = saved;
    if (state.self) state.self.display_name = saved;
    toast('Display name updated', 'success');
  } catch (err) {
    state.forms.identity.displayName = last;
    toast('Could not update display name: ' + (err.message || err), 'error');
  }
}

async function onChangePersistOutbound() {
  const on = !!state.forms.identity.persistOutbound;
  try {
    await state.transport.setIdentitySettings(state.identityId, { persist_outbound_attachments: on });
    if (state.self) state.self.persist_outbound_attachments = on;
  } catch (err) {
    toast('Could not update setting: ' + (err.message || err), 'error');
  }
}

// Settings → Identity → device screen. Turning ON needs the
// physical-presence code, so the checkbox stays off until the device
// confirms; the id-code modal collects the proof. Turning OFF is a
// plain save. A 409 means another identity holds the screen - the
// server message says where to turn it off.
async function onChangeScreenIdentity() {
  const want = !!state.forms.identity.screen;
  if (!want) {
    // Turning the screen off destroys that account's device messages
    // on the device - destructive, so it confirms first.
    const ok = await showConfirm({
      title: 'Turn off the device screen?',
      body: 'The device messages saved for this account will be cleared.',
      okLabel: 'Turn off',
      destructive: true,
    });
    if (!ok) { state.forms.identity.screen = true; return; }
    try {
      await state.transport.setIdentitySettings(state.identityId, { screen: false });
      if (state.self) state.self.screen = false;
      state.forms.messenger.presets = [];   // wiped on the device too
      toast('Screen off. Device messages were cleared.', 'success');
    } catch (err) {
      state.forms.identity.screen = true;
      toast('Could not update: ' + (err.message || err), 'error');
    }
    return;
  }
  state.forms.identity.screen = false;  // pessimistic until confirmed
  state.pendingIdCodeAction = async (code) => {
    if (!code) return;   // dismissed - the toggle already shows off
    try {
      await state.transport.setIdentitySettings(state.identityId,
        { screen: true, identity_code: code });
      state.forms.identity.screen = true;
      if (state.self) state.self.screen = true;
      await populateMessengerPresets();   // freshly seeded templates
      toast('New messages will show on the device screen', 'success');
    } catch (err) {
      toast('Could not enable: ' + (err.message || err), 'error');
    }
  };
  openIdCodeModal({ codePresent: false });
}

// ---- OLED messenger presets (Settings → Identity, shown while the
// screen toggle is on). The list is edited locally and saved whole -
// it is at most 8 small rows.
function normalizeMessengerPresets(list) {
  return (list || []).map(p => ({
    label: p.label || '', dest: p.dest || '',
    content: p.content || '',
    telemetry: {
      location:    !!(p.telemetry && p.telemetry.location),
      environment: !!(p.telemetry && p.telemetry.environment),
      battery:     !!(p.telemetry && p.telemetry.battery),
      compass:     !!(p.telemetry && p.telemetry.compass),
      share_s:     Number((p.telemetry && p.telemetry.share_s) || 0),
      rate_s:      Number((p.telemetry && p.telemetry.rate_s) || 60),
    },
    _manual: false,   // editor-only: recipient select is on "Enter an address"
  }));
}

// Recipient choices for the device-message editor: saved contacts
// first, then peers with an existing chat, then discovered announces.
// Deduped by address; this device's own identities excluded.
function messengerDestOptions() {
  const own = new Map();
  const seen = new Set((state.identities || []).map(i => i.address));
  const add = (addr, name) => {
    if (!addr || addr.length !== 32 || seen.has(addr)) return;
    seen.add(addr);
    own.set(addr, name ? (name + ' · ' + addr.slice(0, 8)) : (addr.slice(0, 16) + '…'));
  };
  for (const [addr, name] of Object.entries(state.contacts || {})) add(addr, name);
  for (const c of Object.values(state.conversations || {})) add(c.peer, c.display_name || state.contacts[c.peer]);
  for (const a of Object.values(state.announces || {})) add(a.dest, a.display_name || '');
  return [...own.entries()].map(([addr, label]) => ({ addr, label }));
}

function messengerDestKnown(addr) {
  return !!addr && messengerDestOptions().some(o => o.addr === addr);
}

function addMessengerPreset() {
  if (state.forms.messenger.presets.length >= 8) return;
  state.forms.messenger.presets.push({
    label: '', dest: '', content: '',
    telemetry: { location: false, environment: false, battery: false,
                 compass: false, share_s: 0, rate_s: 60 },
  });
}

async function populateMessengerPresets() {
  try {
    const r = await state.transport._req(API.MESSENGER_PRESETS);
    state.forms.messenger.presets = normalizeMessengerPresets(r.presets);
  } catch (e) { /* editor keeps last-known values */ }
}

function saveMessengerPresets(btn) {
  return withBusy(btn, async () => {
    try {
      const r = await state.transport._req(API.MESSENGER_PRESETS,
        { method: 'POST', body: { presets: state.forms.messenger.presets } });
      state.forms.messenger.presets = normalizeMessengerPresets(r.presets);
      toast('Messages saved', 'success');
    } catch (e) {
      toast('Save failed: ' + (e.message || e), 'error');
    }
  });
}

// Settings → Identity → Message stamps. The cost is validated here so a
// typo gets a toast + restore instead of a server round-trip; the new
// requirement reaches peers on the next announce.
async function onChangeStampCost() {
  const raw  = String(state.forms.identity.stampCost ?? '').trim();
  const cost = Number(raw);
  const last = String(state.self && state.self.stamp_cost != null ? state.self.stamp_cost : 0);
  if (raw === '') {
    // Cleared field is ambiguous - restore rather than silently saving
    // "off" (0 spelled out is the explicit way to disable).
    state.forms.identity.stampCost = last;
    return;
  }
  if (!Number.isInteger(cost) || cost < 0 || cost > 254) {
    state.forms.identity.stampCost = last;
    toast('Stamp requirement must be a whole number from 0 (off) to 254', 'error');
    return;
  }
  try {
    await state.transport.setIdentitySettings(state.identityId, { stamp_cost: cost });
    if (state.self) state.self.stamp_cost = cost;
    state.forms.identity.stampCost = String(cost);
    toast(cost === 0 ? 'Message stamps no longer required'
                     : 'Stamp requirement updated. Senders learn it from your next announce.', 'success');
  } catch (err) {
    state.forms.identity.stampCost = last;
    toast('Could not update setting: ' + (err.message || err), 'error');
  }
}

async function onChangeEnforceStamps() {
  const on = !!state.forms.identity.enforceStamps;
  try {
    await state.transport.setIdentitySettings(state.identityId, { enforce_stamps: on });
    if (state.self) state.self.enforce_stamps = on;
  } catch (err) {
    state.forms.identity.enforceStamps = !on;
    toast('Could not update setting: ' + (err.message || err), 'error');
  }
}

// App-tab preference checkboxes - both bound @change="savePrefs()" in
// markup. This wrapper merges the live form values into the persisted
// prefs and triggers a thread re-render for the auto-download flip.
function savePrefs() {
  state.prefs = {
    ...state.prefs,
    enterSends:               !!state.forms.prefs.enterSends,
    autoDownloadAttachments:  !!state.forms.prefs.autoDownloadAttachments,
  };
  localStorage.setItem(LS.PREFS, JSON.stringify(state.prefs));
  if (state.openPeer) renderThread();
}

// Inbox capacity + TTL - populated lazily when the UI tab opens and
// saved on each change. Server applies + persists in one round-trip.
async function populateInboxConfig() {
  state.ui.inboxConfigStatus = '';
  try {
    const cfg = await state.transport._req(API.INBOX_CONFIG);
    const r = cfg.default_retention || { kind: 'none', value: 0 };
    const key = r.kind + ':' + (r.value || 0);
    // Default to 'none:0' if the value isn't one of the offered options.
    const known = new Set([
      'none:0','time:604800','time:2592000','time:7776000','time:31536000',
      'count:50','count:200',
    ]);
    state.forms.prefs.inboxDefaultRetention = known.has(key) ? key : 'none:0';
  } catch (e) {
    state.ui.inboxConfigStatus = 'Could not load: ' + (e.message || e);
  }
}
// Bound via @change on #inbox-default-retention.
async function onChangeInboxDefaultRetention() {
  const raw = state.forms.prefs.inboxDefaultRetention || 'none:0';
  const [kind, vStr] = raw.split(':');
  const body = { default_retention: { kind, value: parseInt(vStr, 10) || 0 } };
  try {
    await state.transport._req(API.INBOX_CONFIG, { method: 'POST', body });
    state.ui.inboxConfigStatus = 'Saved.';
    setTimeout(() => { state.ui.inboxConfigStatus = ''; }, 2000);
  } catch (e) {
    state.ui.inboxConfigStatus = 'Save failed: ' + (e.message || e);
  }
}

// Storage transfer caps - two sliders bound to the device's current
// effective_max_* (clamped to backing-store free space + protocol
// ceiling). The slider's max is the largest cap the device can
// currently accept; the value is the user's saved preference. Both
// sliders are x-model'd to state.forms.storage.{maxSend,maxRecv} and
// their :max binds to state.ui.storageMax*Max; labels are x-text
// against state.ui.storageMax*Label.
function _fmtBytes(n) {
  const mb = (n || 0) / (1024 * 1024);
  return mb >= 100 ? `${Math.round(mb)} MiB`
       : mb >= 1   ? `${mb.toFixed(1)} MiB`
                   : `${Math.round((n || 0) / 1024)} KiB`;
}
function _renderStorageLabels() {
  const cfg = state._storageConfig || {};
  state.ui.storageMaxSendLabel = `${_fmtBytes(state.forms.storage.maxSend)} (device caps at ${_fmtBytes(cfg.effective_max_send_bytes)})`;
  state.ui.storageMaxRecvLabel = `${_fmtBytes(state.forms.storage.maxRecv)} (device caps at ${_fmtBytes(cfg.effective_max_recv_bytes)})`;
}
// Single source of truth for the device's storage limits, fetched over plain
// HTTP. state._storageConfig feeds attachMaxBytes() (the composer's hard cap),
// so callers refresh it on demand - e.g. just before validating an attachment -
// rather than depending on a WS push that can be dropped under load.
async function refreshStorageCaps() {
  state._storageConfig = await state.transport._req(API.STORAGE_CONFIG);
  return state._storageConfig;
}
async function populateStorageConfig() {
  state.ui.storageStatus = '';
  try {
    const cfg = await refreshStorageCaps();
    state.ui.storageMaxSendMax = Math.max(cfg.effective_max_send_bytes || 0, 65536);
    state.ui.storageMaxRecvMax = Math.max(cfg.effective_max_recv_bytes || 0, 65536);
    state.forms.storage.maxSend = Math.min(cfg.user_max_send_bytes    || 0, state.ui.storageMaxSendMax);
    state.forms.storage.maxRecv = Math.min(cfg.user_max_receive_bytes || 0, state.ui.storageMaxRecvMax);
    _renderStorageLabels();
  } catch (e) {
    state.ui.storageStatus = 'Could not load: ' + (e.message || e);
  }
}
async function onChangeStorageSlider() {
  // Re-render the live "N KiB / cap M KiB" labels immediately, then
  // POST the new values. Server response refreshes the sliders' caps.
  _renderStorageLabels();
  const body = {
    user_max_send_bytes:    Number(state.forms.storage.maxSend) || 0,
    user_max_receive_bytes: Number(state.forms.storage.maxRecv) || 0,
  };
  try {
    const cfg = await state.transport._req(API.STORAGE_CONFIG, { method: 'POST', body });
    state._storageConfig = cfg;
    state.ui.storageMaxSendMax = Math.max(cfg.effective_max_send_bytes || 0, 65536);
    state.ui.storageMaxRecvMax = Math.max(cfg.effective_max_recv_bytes || 0, 65536);
    _renderStorageLabels();
    state.ui.storageStatus = 'Saved.';
    setTimeout(() => { state.ui.storageStatus = ''; }, 2000);
  } catch (e) {
    state.ui.storageStatus = 'Save failed: ' + (e.message || e);
  }
}
// Public name kept so external callers (e.g. system_update WS frame
// when the SD card flips state) still re-fit the slider bounds.
function refreshStorageSliders() {
  const cfg = state._storageConfig;
  if (!cfg) return;
  state.ui.storageMaxSendMax = Math.max(cfg.effective_max_send_bytes || 0, 65536);
  state.ui.storageMaxRecvMax = Math.max(cfg.effective_max_recv_bytes || 0, 65536);
  if (state.forms.storage.maxSend > state.ui.storageMaxSendMax) state.forms.storage.maxSend = state.ui.storageMaxSendMax;
  if (state.forms.storage.maxRecv > state.ui.storageMaxRecvMax) state.forms.storage.maxRecv = state.ui.storageMaxRecvMax;
  _renderStorageLabels();
}

// Escape dismisses whichever overlay is open. Order matters when more
// than one is somehow open - close the topmost / most-recently-opened
// first. Popups before transient modals before the big settings modal.
document.addEventListener('keydown', (e) => {
  if (e.key !== 'Escape') return;
  if (state.popovers.emoji) { state.popovers.emoji = false; e.preventDefault(); return; }
  // Map old element-ID list to store keys.
  const escModals = [
    ['idCode',          'modal-id-code'],
    ['newConv',         'modal-new-conv'],
  ];
  for (const [key, _m] of escModals) {
    if (state.modals[key]) { state.modals[key] = false; e.preventDefault(); return; }
  }
  if (state.modals.settings) {
    if (state.settingsFirstRun) return;  // locked
    state.modals.settings = false;
    e.preventDefault();
    return;
  }
});

// Bound via @click on #btn-ct-save in the App tab's contacts card.
function onClickContactSave() {
  const addr = (state.forms.contactRename.addr || '').trim().toLowerCase();
  const name = (state.forms.contactRename.name || '').trim();
  if (!/^[0-9a-f]{32}$/.test(addr) || !name) { toast('Invalid address or empty name', 'warn'); return; }
  state.contacts[addr] = name;
  persistContacts();
  renderContacts();
  renderConversations();
  Object.assign(state.forms.contactRename, { addr: '', name: '' });
}

// Bound via @click on #btn-delete-self in the Reset tab.
async function onClickDeleteSelf(btn) {
  const ok = await showConfirm({
    title: 'Delete this identity?',
    body: 'Inbox, outbox, identity material: all gone for ' +
          (state.self ? state.self.display_name : state.identityId) + '.',
    okLabel: 'Delete identity',
    destructive: true,
  });
  if (!ok) return;
  await withBusy(btn, async () => {
    try {
      await state.transport.deleteIdentity(state.identityId);
      state.token = null; state.identityId = null;
      localStorage.removeItem(LS.TOKEN); localStorage.removeItem(LS.IDENTITY);
      location.reload();
    } catch (e) { toast('Delete failed: ' + (e.message || e), 'error'); }
  }, { label: 'Deleting…' });
}

// Bound via @click on #btn-fr2 in the Reset tab.
async function onClickFr2(btn) {
  const ok = await showConfirm({
    title: 'Factory reset?',
    body: 'Every identity, inbox, outbox, and saved token on this device will be wiped.',
    okLabel: 'Factory reset',
    destructive: true,
  });
  if (!ok) return;
  await withBusy(btn, async () => {
    try {
      await state.transport.factoryReset(state.forms.factoryReset2.code);
      waitForDeviceAndReload('Factory-resetting & rebooting…');
    } catch (e) {
      toast('Reset failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Resetting…', alsoDisable: ['#fr2-code'] });
}

function doLogout(btn) {
  return withBusy(btn, async () => {
    try { await state.transport.logout(); } catch {}
    state.token = null;
    localStorage.removeItem(LS.TOKEN);
    if (_unsubscribe) { _unsubscribe(); _unsubscribe = null; }
    location.reload();
  });
}

// ============================== DISCOVERY SETTINGS ==============================
// Mirrors GET/POST /api/discovery/state. Enabling the master toggle
// puts the device on-air with periodic announces, so the firmware
// gates that direction behind the identity-code physical-presence
// check. The dropdowns auto-save on change; the master toggle saves
// after collecting the identity code (and reverts on cancel).
async function populateDiscoveryTab() {
  state.ui.discStatus = '';
  try {
    const [s, id] = await Promise.all([
      state.transport._req(API.DISCOVERY_STATE),
      state.transport._req(API.DISCOVERY_IDENTITY),
    ]);
    Object.assign(state.forms.discovery, {
      enabled:        !!s.enabled,
      interval:       String(s.default_interval_min ?? 360),
      stampCost:      String(s.default_stamp_cost ?? 0),
      advertisedName: s.advertised_name || '',
    });
    state.forms.discovery._lastSavedAdvertisedName = s.advertised_name || '';
    state.ui.discIdentityHash = id.ready ? id.hash : 'not generated yet';
  } catch (e) {
    state.ui.discStatus = 'Could not load: ' + (e.message || e);
  }
}
async function saveDiscoveryField(body) {
  try {
    const updated = await state.transport._req(API.DISCOVERY_STATE, { method: 'POST', body });
    state.ui.discStatus = 'Saved.';
    setTimeout(() => { state.ui.discStatus = ''; }, 2000);
    return updated;
  } catch (e) {
    state.ui.discStatus = 'Save failed: ' + (e.message || e);
    throw e;
  }
}
// Master toggle (#disc-enabled) - bound @change in markup.
async function onChangeDiscEnabled() {
  const wantOn = state.forms.discovery.enabled;
  if (!wantOn) {
    // Disable path: bearer-only, no prompt. Revert if the save fails.
    try { await saveDiscoveryField({ enabled: false }); }
    catch { state.forms.discovery.enabled = true; }
    return;
  }
  // Enable path: collect identity code, then save. Revert the checkbox
  // if the user cancels or the save fails.
  state.pendingIdCodeAction = async (code) => {
    if (!code) { state.forms.discovery.enabled = false; return; }
    try { await saveDiscoveryField({ enabled: true, identity_code: code }); }
    catch { state.forms.discovery.enabled = false; }
  };
  openIdCodeModal();
}
async function onChangeDiscInterval() {
  await saveDiscoveryField({ default_interval_min: parseInt(state.forms.discovery.interval, 10) || 360 });
}
async function onChangeDiscStampCost() {
  await saveDiscoveryField({ default_stamp_cost: parseInt(state.forms.discovery.stampCost, 10) || 0 });
}
// Advertised name: persist on blur (markup @keydown.enter blurs first).
// Trim whitespace; empty value = "fall back to the interface name" on
// the firmware side. No-op when the value hasn't actually changed.
async function onBlurDiscName() {
  const v    = (state.forms.discovery.advertisedName || '').trim();
  const last = state.forms.discovery._lastSavedAdvertisedName || '';
  if (v === last) return;
  try {
    await state.transport._req(API.DISCOVERY_STATE,
      { method: 'POST', body: { advertised_name: v } });
    state.forms.discovery._lastSavedAdvertisedName = v;
    state.ui.discNameStatus = v ? 'Saved.' : 'Cleared. Falls back to interface name.';
    setTimeout(() => { state.ui.discNameStatus = ''; }, 2000);
  } catch (e) {
    state.ui.discNameStatus = 'Save failed: ' + (e.message || e);
  }
}

