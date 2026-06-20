// ============================== STATUS INDICATOR ==============================
// Topbar antenna icon's coloured dot reflects radio + transport state.
// Click opens a popover with live config and a transport toggle.
// Backing data comes from /api/info; we refresh on open and on a slow
// background tick so the indicator isn't perpetually stale.
async function refreshStatus() {
  try {
    // Snapshot the prior info BEFORE we overwrite it so the Alpine
    // "Δ since last poll" getter has stable prev values to compare
    // against the freshly-fetched current values.
    if (state.lastInfo && state.lastInfo.radio && state.lastInfo.radio.stats) {
      const ps = state.lastInfo.radio.stats;
      state.lastInfoStats = {
        ts:        Date.now(),
        uptime_ms: state.lastInfo.uptime_ms || 0,
        rx:        ps.rx_packets || 0,
        tx:        ps.tx_packets || 0,
      };
    }
    const info = await state.transport.getInfo();
    state.lastInfo = info;
    state.info = info;
    // Apply server-published limits to the compose UI. /api/info.limits
    // is the single source of truth; we mirror the values onto input
    // attributes so the browser enforces them without a round-trip,
    // matching the firmware's accept/reject behaviour.
    applyServerLimits(info.limits || {});
    // Cache identity-code TTL so the hint text reflects the firmware's
    // actual compile-time IDENTITY_CODE_TTL_MS (currently 60 s) rather
    // than whatever number the SPA happened to ship with. The hint
    // paragraphs bind `x-html="idCodeHintHtml()"`, so writing the
    // reactive state.idCodeTtlMs is all that's needed - Alpine
    // re-renders every placement automatically.
    if (typeof info.identity_code_ttl_ms === 'number'
        && info.identity_code_ttl_ms !== state.idCodeTtlMs) {
      state.idCodeTtlMs = info.identity_code_ttl_ms;
    }
    applyStatusToButton(info);
    // Battery icon is reactive on state.lastInfo / state.system.
    // Popover content is fully reactive against $store.s.lastInfo -
    // no imperative render call.
  } catch (e) {
    state.radioStatus.state = 'unknown';
    state.radioStatus.title = 'Status unknown: ' + (e.message || e);
  }
  // System snapshot (storage / outbound_caps / battery / sensors) is
  // WS-pushed; re-derive the values the rest of the UI reads from it.
  applySystemDerived();
}

// Project state.system (the WS-pushed snapshot) into the flat fields
// the UI reads: the composer's attachment cap (outboundCaps) and the
// SD-present flag. Called from the `hello` + `system_update` WS frames
// AND the 60 s refreshStatus poll, so the composer reflects the
// device's real staging cap from WS frame zero rather than lagging up
// to a minute behind on the poll (which left attachMaxBytes on its
// 64 KB fallback until the first poll synced it).
function applySystemDerived() {
  const sys = state.system || {};
  state.lastSystemStatus = sys;
  const wasPresent = state.sdPresent;
  state.sdPresent   = !!(sys.storage && sys.storage.sd && sys.storage.sd.present);
  state.outboundCaps = sys.outbound_caps || null;
  if (wasPresent !== null && wasPresent !== state.sdPresent && state.openPeer) {
    renderThread();
  }
}
function applyStatusToButton(info) {
  const r = info && info.radio;
  if (!r)            { state.radioStatus.state = 'unknown'; state.radioStatus.title = 'Radio status unknown'; return; }
  if (!r.have_conf)  { state.radioStatus.state = 'unconf';  state.radioStatus.title = 'Radio not configured. Open Settings → Radio'; return; }
  if (r.online)      { state.radioStatus.state = 'online';
                       state.radioStatus.title = 'Radio online · ' + (r.frequency/1e6).toFixed(3) + ' MHz'; return; }
  state.radioStatus.state = 'offline';
  state.radioStatus.title = 'Radio configured but offline';
}
// Battery icon in the topbar. Hidden when the device hasn't reported
// a battery block at all (older firmware / non-PMU boards). The fill
// rectangle's width is scaled to the percent value; a bolt overlay
// shows when charging. Fill colour conveys remaining charge at a
// glance (green / amber / red) so the icon stays meaningful on mobile
// where the numeric percent is hidden via the @media rule above.
// Reactive data backing the topbar battery icon. Reads from the
// WS-authoritative snapshot (state.system) and falls back to the
// polling snapshot (state.lastInfo) so the icon updates live.
function batteryBtnData() {
  return {
    get _b() {
      const sysB  = (this.$store.s.system   && this.$store.s.system.battery)   || null;
      const infoB = (this.$store.s.lastInfo && this.$store.s.lastInfo.battery) || null;
      return sysB || infoB || {};
    },
    get visible()      { return !!this._b && this._b.state && this._b.state !== 'absent'; },
    get batteryState() { return this._b.state || 'unknown'; },
    get charging()     { return this._b.state === 'charging'; },
    get pct() { return Math.max(0, Math.min(100, Number(this._b.percent) || 0)); },
    get colour() {
      const st = this._b.state;
      if (st === 'charging' || st === 'charged') return '#3fb950';
      const p = this.pct;
      if (p >= 50) return '#3fb950';
      if (p >= 20) return '#d29922';
      return '#f85149';
    },
  };
}
// Reactive data backing the radio/wifi/transport popover. Every field
// is computed from $store.s.lastInfo / $store.s.radioTlm.
function statusPopoverData() {
  return {
    // -------- radio --------
    get _r() { return (this.$store.s.lastInfo && this.$store.s.lastInfo.radio) || {}; },
    get _t() { return (this.$store.s.lastInfo && this.$store.s.lastInfo.transport) || {}; },
    get _s() { return this._r.stats || {}; },
    get _w() { return (this.$store.s.lastInfo && this.$store.s.lastInfo.wifi) || {}; },
    get radioLabel() { return this._r.model || 'Radio'; },
    get radioPillClass() {
      const r = this._r;
      if (!r.have_conf) return 'pill warn';
      return r.online ? 'pill ok' : 'pill bad';
    },
    get radioPillLabel() {
      const r = this._r;
      if (!r.have_conf) return 'Not configured';
      return r.online ? 'Online' : 'Offline';
    },
    get radioCfg1() {
      const r = this._r;
      return r.have_conf
        ? (vu((r.frequency/1e6).toFixed(3), 'MHz') + ' · BW ' + vu((r.bandwidth/1000), 'kHz'))
        : 'No radio config saved yet.';
    },
    get radioCfg2() {
      const r = this._r;
      return r.have_conf
        ? ('SF' + r.spreading_factor + ' · CR4/' + r.coding_rate + ' · ' + vu(r.tx_power, 'dBm'))
        : '';
    },
    get radioStatsLine() {
      const s = this._s;
      if (s.rx_packets === undefined && s.tx_packets === undefined) return '';
      return 'RX ' + (s.rx_packets || 0) + ' pkt · TX ' + (s.tx_packets || 0) + ' pkt';
    },
    get radioAirtimeLine() {
      const s = this._s;
      if (s.airtime_pct === undefined && s.longterm_airtime_pct === undefined) return '';
      const st_now = s.airtime_pct || 0;
      const lt_now = s.longterm_airtime_pct || 0;
      const stStr = s.airtime_limit_pct !== undefined ? (st_now + '%/' + s.airtime_limit_pct + '% s') : (st_now + '% s');
      const ltStr = s.longterm_airtime_limit_pct !== undefined ? (lt_now + '%/' + s.longterm_airtime_limit_pct + '% l') : (lt_now + '% l');
      return 'Airtime: ' + stStr + ' · ' + ltStr;
    },
    get radioRateLine() {
      const prev = this.$store.s.lastInfoStats;
      const info = this.$store.s.lastInfo || {};
      const s = this._s;
      // Placeholder (not '') so the line reserves its height from the moment the
      // popover opens and doesn't shift everything down when the first real delta
      // lands a poll-interval later.
      const PLACEHOLDER = 'Δ -: - RX · - TX';
      if (!prev || !info.uptime_ms || !prev.ts) return PLACEHOLDER;
      const dt_s = (info.uptime_ms - prev.uptime_ms) / 1000;
      if (!(dt_s > 0.5 && dt_s < 600)) return PLACEHOLDER;
      const drx = (s.rx_packets || 0) - (prev.rx || 0);
      const dtx = (s.tx_packets || 0) - (prev.tx || 0);
      return 'Δ ' + dt_s.toFixed(0) + 's: +' + drx + ' RX · +' + dtx + ' TX';
    },
    get radioCsmaLine() {
      // Touch radioTlm.samples.length so changes trigger re-eval.
      void this.$store.s.radioTlm.samples.length;
      return radioCsmaLineText();
    },
    get radioChartHTML() {
      void this.$store.s.radioTlm.samples.length;
      return radioChartHTMLBuild();
    },
    get networkChartHTML() {
      void this.$store.s.netTlm.samples.length;
      return networkChartHTMLBuild();
    },
    get networkRateLine() {
      void this.$store.s.netTlm.samples.length;
      return networkRateLineBuild();
    },
    // -------- wifi --------
    get wifiMode() { return this._w.mode; },
    get wifiPillClass() {
      const w = this._w;
      if (w.mode === 'ap') return 'pill warn';
      if (w.mode === 'sta' && w.connected) return 'pill ok';
      if (w.mode === 'sta') return 'pill bad';
      return 'pill';
    },
    get wifiPillLabel() {
      const w = this._w;
      if (w.mode === 'ap') return 'softAP';
      if (w.mode === 'sta' && w.connected) return 'Connected';
      if (w.mode === 'sta') return 'Disconnected';
      return w.mode || 'off';
    },
    get wifiSsid() { return this._w.ssid || ''; },
    get wifiIpLine() {
      const w = this._w;
      if (w.mode === 'ap' && w.ap_ip) return 'http://' + w.ap_ip + '/';
      if (w.ip && w.ip !== '0.0.0.0') return w.ip;
      return '';
    },
    get wifiAuthLine() {
      const w = this._w;
      if (!w.auth) return '';
      let s = w.auth;
      if (typeof w.rssi_dbm === 'number') s += ' · ' + vu(w.rssi_dbm, 'dBm');
      return s;
    },
    // -------- transport --------
    get transportEnabled() { return !!this._t.enabled; },
  };
}
function openStatusPopover() {
  // Backfill the radio-telemetry ring once per session so the chart
  // shows ~2 min of history right away. Subsequent updates arrive via
  // the WS `radio_telemetry` push and are appended in dispatchEvent.
  // Re-seed each open is wasteful - the local ring is already correct
  // once seeded - so the guard prevents that.
  refreshRadioTelemetry();
  refreshNetworkTelemetry();
  // Chart + CSMA line are reactive - Alpine bindings repaint them
  // automatically as $store.s.radioTlm.samples grows from WS pushes.
  state.popovers.status = true;
  // Close on outside click; defer one tick so this click doesn't fire it.
  // Announce button is excluded - it's a momentary action button users
  // may legitimately want to hit while reading the popover (e.g. open
  // radio status, see online + no recent announce, tap announce, see
  // the popover update without losing context). Other topbar buttons
  // (settings / logout / system / power) are popover-toggles that
  // SHOULD dismiss this one - their own click handlers close us before
  // opening their target.
  setTimeout(() => {
    const dismiss = (ev) => {
      if (popoverDismissibleClick(ev, 'popover-status', 'btn-status')) {
        state.popovers.status = false;
        document.removeEventListener('click', dismiss);
      }
    };
    document.addEventListener('click', dismiss);
  }, 0);
}
// Toggle the status popover. Closes any conflicting popover on the
// way in. Used from @click on the topbar button.
async function onClickStatusBtn() {
  if (state.popovers.status) { state.popovers.status = false; return; }
  state.popovers.system = false; stopSystemTicker();
  // Open immediately with the cached snapshot, then refresh in the background.
  // Awaiting the fetch first meant a stalled HTTP request (common on the
  // SRAM-tight device) left the popover unopenable.
  openStatusPopover();
  refreshStatus().catch(() => {});
}

// ============================== RADIO TELEMETRY ==============================
// One-shot fetch of /api/radio/telemetry to seed the local ring. Idempotent
// - once seeded we trust the WS push to keep it fresh. Re-seeds on
// reconnect (state.radioTlm.seeded is cleared by the _reconnect handler).
async function refreshRadioTelemetry() {
  const rt = state.radioTlm;
  if (rt.seeded) return;
  const tok = state.token; if (!tok) return;
  try {
    const r = await fetch(API.RADIO_TELEMETRY,
                          { headers: { 'Authorization': 'Bearer ' + tok } });
    if (!r.ok) return;
    const j = await r.json();
    rt.samples = Array.isArray(j.samples) ? j.samples : [];
    if (typeof j.capacity === 'number')  rt.cap      = j.capacity;
    if (typeof j.period_ms === 'number') rt.periodMs = j.period_ms;
    if (typeof j.cw_max_band === 'number') rt.cwMax  = j.cw_max_band;
    rt.seeded = true;
  } catch (_) { /* leave seeded=false so we retry next open */ }
}

// One-shot seed of the network (WiFi/transport) telemetry ring, same pattern
// as refreshRadioTelemetry. Re-seeds on reconnect (netTlm.seeded cleared there).
async function refreshNetworkTelemetry() {
  const nt = state.netTlm;
  if (nt.seeded) return;
  const tok = state.token; if (!tok) return;
  try {
    const r = await fetch(API.NETWORK_TELEMETRY,
                          { headers: { 'Authorization': 'Bearer ' + tok } });
    if (!r.ok) return;
    const j = await r.json();
    nt.samples = Array.isArray(j.samples) ? j.samples : [];
    if (typeof j.capacity === 'number')  nt.cap      = j.capacity;
    if (typeof j.period_ms === 'number') nt.periodMs = j.period_ms;
    nt.seeded = true;
  } catch (_) { /* retry next open */ }
}

// Human byte-rate. Sub-1K shows bytes, then KB/s, then MB/s.
function netFmtRate(b) {
  if (!(b > 0)) return '0 B/s';
  if (b >= 1048576) return (b / 1048576).toFixed(1) + ' MB/s';
  if (b >= 1024)    return (b / 1024).toFixed(1) + ' KB/s';
  return (b | 0) + ' B/s';
}

// Current tx/rx rate line above the chart (mirrors the radio rate line).
function networkRateLineBuild() {
  const nt = state.netTlm;
  const s = nt.samples[nt.samples.length - 1] || {};
  return '<span class="swatch swatch-net-tx"></span>tx ' + netFmtRate(s.tx || 0)
       + '  <span class="swatch swatch-net-rx"></span>rx ' + netFmtRate(s.rx || 0);
}

// SVG line chart of WiFi/transport tx & rx byte rate. Reuses the radio
// chart's classes/host sizing; auto-scales the y-axis to a nice round max of
// the (smoothed) peak so low background traffic stays visible without the
// lines jittering each second.
function networkChartHTMLBuild() {
  const nt = state.netTlm;
  const n = nt.samples.length;
  if (n < 2) return '<div class="radio-chart-empty">Awaiting telemetry…</div>';
  const host = document.getElementById('status-net-chart');
  const W = Math.max(120, (host && host.clientWidth) || 240);
  const H = 56, padL = 30, padR = 6, padT = 4, padB = 4;
  const plotW = W - padL - padR, plotH = H - padT - padB;
  const dx = plotW / Math.max(1, (nt.cap - 1));
  const xOf = (i) => padL + (nt.cap - n + i) * dx;
  const sw = Math.max(1, nt.smoothing | 0);
  const smooth = (i, key) => {
    let sum = 0, c = 0;
    for (let j = Math.max(0, i - sw + 1); j <= i; j++) {
      const v = nt.samples[j][key];
      if (typeof v === 'number') { sum += v; c++; }
    }
    return c ? sum / c : 0;
  };
  let peak = 0;
  for (let i = 0; i < n; i++) peak = Math.max(peak, smooth(i, 'tx'), smooth(i, 'rx'));
  // Nice round axis max (1/2/5 × 10^k), floored at 1 KB so an idle link
  // doesn't render a 0-height axis.
  const niceMax = (v) => {
    if (v <= 1024) return 1024;
    const p = Math.pow(10, Math.floor(Math.log10(v)));
    const m = v / p;
    return (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p;
  };
  const yMax = niceMax(peak);
  const yOf = (v) => padT + plotH - (Math.max(0, Math.min(yMax, v)) / yMax) * plotH;
  const path = (key) => {
    let d = '';
    for (let i = 0; i < n; i++) {
      d += (d ? 'L' : 'M') + xOf(i).toFixed(1) + ',' + yOf(smooth(i, key)).toFixed(1) + ' ';
    }
    return d;
  };
  const fmtShort = (b) => b >= 1048576 ? (b / 1048576).toFixed(1) + 'M'
                        : b >= 1024 ? (b / 1024).toFixed(0) + 'K' : (b | 0) + 'B';
  return `
    <svg class="radio-chart-svg" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
      <line class="radio-chart-grid" x1="${padL}" x2="${W - padR}" y1="${yOf(yMax / 2)}" y2="${yOf(yMax / 2)}"/>
      <path class="radio-chart-line net-rx" d="${path('rx')}"/>
      <path class="radio-chart-line net-tx" d="${path('tx')}"/>
      <text x="2" y="${padT + 6}"     font-size="8" fill="#888">${fmtShort(yMax)}</text>
      <text x="2" y="${padT + plotH}" font-size="8" fill="#888">0</text>
    </svg>`;
}

// Render the CSMA state line into #status-radio-csma. Carries the
// contention-window band the firmware is parked in plus the live DCD /
// LOCK flags. Pulled from the most recent radio_telemetry sample so it
// refreshes at 1 Hz while the popover is open. Empty when the
// telemetry ring is still warming up so the row doesn't leave a stale
// figure on screen.
function radioCsmaLineText() {
  const rt = state.radioTlm;
  const last = rt.samples.length ? rt.samples[rt.samples.length - 1] : null;
  if (!last) return '';
  const parts = ['CW band ' + (last.cw ?? '?') + '/' + rt.cwMax];
  if (last.dcd)  parts.push('DCD');
  if (last.lock) parts.push('TX locked');
  return parts.join(' · ');
}

// Render the inline chart into #status-radio-chart. Two y-axes:
//   left  = 0..100 % for own / peer / total channel utilisation
//   right = -120..-40 dBm for RSSI
// Time axis is sample-index - 1 px-per-sample at the popover's width.
// Re-renders in place; safe to call on every WS push while open.
// Build the inline radio-telemetry SVG chart as an HTML string. Called
// from the popover's x-html binding so Alpine reactivity drives the
// update without an imperative innerHTML write.
function radioChartHTMLBuild() {
  const rt = state.radioTlm;
  const n = rt.samples.length;
  if (n < 2) return '<div class="radio-chart-empty">Awaiting telemetry…</div>';
  // Width: use the popover host's clientWidth so the SVG matches.
  // Fall back to 240 if the host hasn't laid out yet (display:none parent).
  const host = document.getElementById('status-radio-chart');
  const W = Math.max(120, (host && host.clientWidth) || 240);
  const H = 56;
  const padL = 22, padR = 22, padT = 4, padB = 4;
  const plotW = W - padL - padR;
  const plotH = H - padT - padB;
  // X: each sample maps to one column.
  const dx = plotW / Math.max(1, (rt.cap - 1));
  const xOf = (i) => padL + (rt.cap - n + i) * dx;
  // Util y: 0 at bottom, snapped-max at top. Auto-scaling so low
  // activity stays visible - when everything is <5%, the lines would
  // otherwise hug the bottom of a fixed 0-100 axis. Snap to a small
  // set of nice round values so the scale "feels" stable across
  // adjacent renders (otherwise the lines jitter as the absolute max
  // shifts each second).
  // Auto-scale max uses the same smoothing window as the rendered
  // lines so the y-axis tracks what's actually drawn (not the raw
  // spike peak that gets averaged away). Computed inline since
  // `path()` below defines its own smoothing logic - refactoring into
  // a shared helper would require passing the smoothing window twice.
  const UTIL_BUCKETS = [5, 10, 25, 50, 100];
  let utilMax = 0;
  const smoothPeak = (key) => {
    const sw = Math.max(1, rt.smoothing | 0);
    for (let i = 0; i < rt.samples.length; i++) {
      let sum = 0, count = 0;
      for (let j = Math.max(0, i - sw + 1); j <= i; j++) {
        const v = (rt.samples[j].util || {})[key];
        if (typeof v === 'number') { sum += v; count++; }
      }
      if (count > 0) {
        const avg = sum / count;
        if (avg > utilMax) utilMax = avg;
      }
    }
  };
  smoothPeak('own');
  smoothPeak('others');
  const yMax = UTIL_BUCKETS.find(b => b >= utilMax) || 100;
  const yUtil = (v) => padT + plotH - (Math.max(0, Math.min(yMax, v)) / yMax) * plotH;
  // RSSI y: tightened to the practical LoRa range. The previous
  // -120/-40 band was wider than anything real RSSI ever reaches
  // (noise floor sits around -95, packets around -50 to -90), so
  // -110/-50 gives a similar amount of pixel per dB on the useful
  // part of the range without sacrificing useful detail.
  const RSSI_HI = -50, RSSI_LO = -110;
  const yRssi = (v) => {
    if (typeof v !== 'number') return padT + plotH;
    const cv = Math.max(RSSI_LO, Math.min(RSSI_HI, v));
    return padT + plotH - ((cv - RSSI_LO) / (RSSI_HI - RSSI_LO)) * plotH;
  };
  // `path` walks the ring building an SVG path-d. `keys` is either a
  // top-level key (e.g. 'rssi') or a dotted path (e.g. 'util.own') to
  // reach into the nested util block.
  const get = (s, key) => {
    if (!key.includes('.')) return s[key];
    let v = s; for (const k of key.split('.')) { if (v == null) return undefined; v = v[k]; }
    return v;
  };
  // Trailing-window moving average applied at render time. Window=1
  // gives the raw 1Hz data (sharp triangle spikes); larger windows
  // smooth proportionally. Index `i`'s value averages samples from
  // [i - window + 1 .. i], skipping non-numeric or sentinel readings
  // within the window so the average only weights real data points.
  const smoothW = Math.max(1, rt.smoothing | 0);
  const sentinel = (k, v) => (k === 'rssi' || k === 'noise') && v < -160;
  const path = (yfn, key) => {
    let d = '';
    for (let i = 0; i < n; i++) {
      let sum = 0, count = 0;
      for (let j = Math.max(0, i - smoothW + 1); j <= i; j++) {
        const v = get(rt.samples[j], key);
        if (typeof v !== 'number') continue;
        if (sentinel(key, v)) continue;
        sum += v; count++;
      }
      if (count === 0) continue;
      const avg = sum / count;
      d += (d ? 'L' : 'M') + xOf(i).toFixed(1) + ',' + yfn(avg).toFixed(1) + ' ';
    }
    return d;
  };
  const last = rt.samples[n - 1] || {};
  const lastU = last.util || {};
  // Grid: a single midline matched to the current util scale. Two
  // lines for the very-low scales would crowd the plot.
  const utilGrid =
    `<line class="radio-chart-grid" x1="${padL}" x2="${W - padR}" y1="${yUtil(yMax / 2)}" y2="${yUtil(yMax / 2)}"/>`;
  // Y-axis tick labels: left = util %, right = RSSI dBm. The util-top
  // label is dynamic - it tracks the auto-scaled yMax so the user
  // knows whether they're looking at a 5% or 100% axis.
  const labels = `
    <text x="2"           y="${padT + 6}"        font-size="8" fill="#888">${yMax}%</text>
    <text x="2"           y="${padT + plotH}"    font-size="8" fill="#888">0%</text>
    <text x="${W - 20}"   y="${padT + 6}"        font-size="8" fill="#888">${RSSI_HI}</text>
    <text x="${W - 20}"   y="${padT + plotH}"    font-size="8" fill="#888">${RSSI_LO}</text>
  `;
  return `
    <svg class="radio-chart-svg" viewBox="0 0 ${W} ${H}" preserveAspectRatio="none">
      ${utilGrid}
      <path class="radio-chart-line noise"  d="${path(yRssi, 'noise')}"/>
      <path class="radio-chart-line others" d="${path(yUtil, 'util.others')}"/>
      <path class="radio-chart-line own"    d="${path(yUtil, 'util.own')}"/>
      <path class="radio-chart-line rssi"   d="${path(yRssi, 'rssi')}"/>
      ${labels}
    </svg>
    <div class="radio-chart-legend">
      <!-- Row 1: left-axis lines (utilisation %). -->
      <div class="radio-chart-legend-row">
        <span><span class="swatch swatch-own" ></span>own ${vu(lastU.own ?? 0, '%')}</span>
        <span title="Fraction of recent DCD samples that detected non-self channel activity. Aggregate: could be one peer, many peers, or LoRa-like interference. DCD doesn't sample while we're TX'ing, so our own emissions are excluded."><span class="swatch swatch-others" ></span>others ${vu(lastU.others ?? 0, '%')}</span>
      </div>
      <!-- Row 2: right-axis lines (dBm). RSSI / noise share a y-scale;
           grouping them on their own row keeps the legend readable on
           narrow popovers. -->
      <div class="radio-chart-legend-row">
        <span><span class="swatch swatch-rssi" ></span>RSSI ${(typeof last.rssi === 'number' && last.rssi > -160) ? vu(last.rssi, 'dBm') : vu('-', 'dBm')}</span>
        <span title="Measured ambient RSSI when no LoRa carrier is detected. The gap to RSSI is effective SNR. Jumps in noise indicate new in-band interferers."><span class="swatch swatch-noise" ></span>noise ${(typeof last.noise === 'number' && last.noise > -160) ? vu(last.noise, 'dBm') : vu('-', 'dBm')}</span>
      </div>
    </div>
  `;
}

