// ============================== SYSTEM POPOVER ==============================
// Storage + clock + sensor snapshot. Source data: /api/system_status,
// fetched on each open (cheap aggregation, no SSE needed for now). A
// 1 s ticker keeps the displayed clock live while the popover is open.
const sysPopover = { ticker: null };

// Project device millis() forward from the WS-pinned clock anchor.
// Returns null when the anchor hasn't been set yet (pre-hello). Used
// by ageLine() so popover labels can tick up live between WS pushes.
function deviceMillisNow() {
  const a = state.clockAnchor;
  if (!a || typeof a.deviceNowMs !== 'number') return null;
  return a.deviceNowMs + (performance.now() - (a.localPerfMs || 0));
}

// Wall-clock formatter: date + time with a space between them, not the
// locale-default "date, time" comma. toLocaleString() inserts the comma; we
// format the two parts separately so there's never one.
function fmtWallClock(ms) {
  const d = new Date(ms);
  return d.toLocaleDateString() + ' ' + d.toLocaleTimeString();
}

// Format a numeric value + its unit as HTML, with the unit wrapped
// in <span class="unit"> so the popover CSS can render it slightly
// smaller and dimmer. No whitespace between value and unit - matches
// the "23.4°C" convention rather than the "23.4 °C" one.
function vu(value, unit) {
  return value + '<span class="unit">' + unit + '</span>';
}

// "Last read Xs ago (MODEL)" from a raw device-millis snapshot.
// Re-running this each tick advances the seconds count without a
// refetch. taken_ms == -1 or missing => "never received" → empty
// string so the subline collapses rather than showing a spinner-y
// "Waiting for first read…" that duplicates the section's primary
// line state. The `warming` arg is kept for callers that want the
// distinction (none currently do).
function ageLine(taken_ms, model, warming) {
  if (typeof taken_ms !== 'number' || taken_ms < 0) return '';
  const now = deviceMillisNow();
  if (now === null) return '';
  const age_s = Math.max(0, Math.round((now - taken_ms) / 1000));
  return 'Last read ' + age_s + 's ago (' + model + ')';
}

// Compact "Xs ago" / "Xm ago" / "Xh ago" / "Xd ago" formatter for
// short-duration human-readable ages. Caller has already decided the
// duration is real (i.e. not negative, not "never"). Used by the
// clock popover's last-calibrated line.
function compactAgo(seconds) {
  const s = Math.max(0, Math.round(seconds));
  if (s < 60)    return s + 's ago';
  if (s < 3600)  return Math.round(s / 60)   + 'm ago';
  if (s < 86400) return Math.round(s / 3600) + 'h ago';
  return Math.round(s / 86400) + 'd ago';
}

// Shared three-state sensor pill (Live / Disabled / Warming up).
function sensorStateForPill(enabled, valid) {
  if (!enabled) return { cls: 'pill warn', label: 'Disabled', state: 'disabled' };
  if (!valid)   return { cls: 'pill warn', label: 'Warming up', state: 'warming' };
  return { cls: 'pill ok', label: 'Live', state: 'live' };
}

// Reactive data backing the system popover. Each getter reads from
// $store.s.system (the WS-pushed system snapshot) plus state.clockAnchor.
function systemPopoverData() {
  return {
    // Per-sensor expand/collapse state for the popover's sensor rows.
    // Keys are the sensor names from data-sensor (gps, environment,
    // magnetometer, imu). Click on the header toggles; x-show on the
    // .popover-config div reads from here.
    sectionOpen: {},
    toggleSection(name) { this.sectionOpen[name] = !this.sectionOpen[name]; },
    get _sys()  { return this.$store.s.system || {}; },
    get _fl()   { return (this._sys.storage && this._sys.storage.flash) || {}; },
    get _sd()   { return (this._sys.storage && this._sys.storage.sd)    || {}; },
    get _g()    { return (this._sys.sensors && this._sys.sensors.gps)          || {}; },
    get _env()  { return (this._sys.sensors && this._sys.sensors.environment)  || {}; },
    get _mg()   { return (this._sys.sensors && this._sys.sensors.magnetometer) || {}; },
    get _im()   { return (this._sys.sensors && this._sys.sensors.imu)          || {}; },
    get _a()    { return this.$store.s.clockAnchor || {}; },

    // -------- storage --------
    get storagePillClass() {
      const fl = this._fl;
      if (!fl.total_bytes) return 'pill';
      const pct = Math.round(100 * (fl.free_bytes || 0) / fl.total_bytes);
      if (pct < 10) return 'pill bad';
      if (pct < 25) return 'pill warn';
      return 'pill ok';
    },
    get storagePillLabel() {
      const fl = this._fl;
      if (!fl.total_bytes) return '-';
      const pct = Math.round(100 * (fl.free_bytes || 0) / fl.total_bytes);
      return pct + '% free';
    },
    get storageFlashHTML() {
      const fl = this._fl;
      return fl.total_bytes
        ? formatBytesVU(fl.used_bytes || 0) + ' / ' + formatBytesVU(fl.total_bytes) + ' flash'
        : '';
    },
    get storageSdHTML() {
      const sd = this._sd;
      return (sd.present && sd.total_bytes)
        ? formatBytesVU(sd.used_bytes || 0) + ' / ' + formatBytesVU(sd.total_bytes) + ' SD'
        : 'SD card not present';
    },
    get storageShowMigrate() {
      return !!(this._sd.present && (this._fl.used_bytes || 0) > 0);
    },

    // -------- clock --------
    get clockPillClass() { return this._a.calibrated ? 'pill ok' : 'pill warn'; },
    get clockPillLabel() { return this._a.calibrated ? 'Calibrated' : 'Uncalibrated'; },
    get clockNowText() {
      return this._a.deviceUnixMs ? fmtWallClock(this._a.deviceUnixMs) : '-';
    },
    get clockSourceText() {
      const a = this._a;
      const src    = (a.source || 'unknown').toUpperCase();
      const lastMs = Number(a.lastCalibratedMs || 0);
      const now    = deviceMillisNow();
      if (lastMs > 0 && now !== null) {
        const age_s = Math.max(0, Math.round((now - lastMs) / 1000));
        return 'Last calibrated ' + compactAgo(age_s) + ' (' + src + ')';
      }
      return 'Source: ' + src;
    },

    // -------- GPS --------
    get gpsAvailable() { return !!this._g.available; },
    get gpsPillClass() {
      const g = this._g;
      if (g.valid)                  return 'pill ok';
      if (g.powered === false)      return 'pill';
      return 'pill warn';
    },
    get gpsPillLabel() {
      const g = this._g;
      if (g.valid)                  return 'Live';
      if (g.powered === false)      return 'Sleeping';
      return 'Acquiring';
    },
    get gpsHaveFix() {
      const g = this._g;
      return typeof g.unix_ms === 'number' && g.unix_ms > 0;
    },
    // Satellites/signal line - the grayed extras row. CONSTANT format
    // whether or not there is a fix (the radio popover's delta line
    // taught us: lines must not jump into existence or restructure on
    // the first reading): "used/visible satellites \u00b7 signal N dB",
    // zeros and all. The module name is NOT repeated here - the age
    // line below already carries it.
    // Same shape as the Motion lines: label, values, dimmed unit.
    get gpsExtraHTML() {
      const g = this._g;
      if (!g.available) return '';
      return 'Satellites ' + (g.sats_visible || 0) + ' seen / ' + (g.sats || 0)
           + ' used \u00b7 max ' + vu(g.snr_db || 0, 'dB');
    },
    // RF health - MAX-M10 only, and then always shown: noise floor and
    // receiver gain say something useful even with zero satellites
    // (e.g. high noise explaining why there is no fix), and
    // interference state rides the same stable line.
    get gpsRfText() {
      const g = this._g;
      if (g.noise == null) return '';
      const jam = { unknown: 'unknown', none: 'none', warning: 'detected', critical: 'severe' };
      return 'Noise ' + g.noise + ' \u00b7 gain ' + (g.agc_pct || 0) + '% \u00b7 interference '
             + (jam[g.jamming] || 'unknown');
    },
    get gpsPosText() {
      const g = this._g;
      if (!this.gpsHaveFix) return 'No fix';
      let t = g.latitude.toFixed(5) + ', ' + g.longitude.toFixed(5);
      // Real accuracy estimate (MAX-M10) rides the position value -
      // it qualifies the coordinates, and this line's content varies
      // by nature so it cannot cause layout jumps.
      if (g.hacc_m != null) t += ' \u00b1' + Number(g.hacc_m).toFixed(1) + ' m';
      return t;
    },
    get gpsFixTimeText() {
      return this.gpsHaveFix ? ('GPS fix ' + new Date(this._g.unix_ms).toLocaleString()) : '';
    },
    get gpsAgeText() {
      const g = this._g;
      return ageLine(g.last_byte_ms, g.model || 'GPS');
    },

    // -------- environment / mag / imu --------
    get envAvailable() { return !!this._env.available; },
    get envState() { return sensorStateForPill(this._env.enabled, this._env.valid); },
    get envValuesHTML() {
      if (this.envState.state !== 'live') return '';
      const env = this._env;
      return vu(env.temp_c.toFixed(1), '°C') + ' · ' +
             vu(env.humidity_pct.toFixed(0), '%RH') + ' · ' +
             vu((env.pressure_pa / 100).toFixed(1), 'hPa');
    },
    get envAgeText() {
      const env = this._env;
      return ageLine(env.taken_ms, env.model || 'environment');
    },

    get magAvailable() { return !!this._mg.available; },
    get magState() { return sensorStateForPill(this._mg.enabled, this._mg.valid); },
    get magValuesHTML() {
      if (this.magState.state !== 'live') return '';
      const mg = this._mg;
      return 'Heading ' + vu(mg.heading_deg.toFixed(0), '°') + ' · ' +
             vu(mg.x_uT.toFixed(1) + ' / ' + mg.y_uT.toFixed(1) + ' / ' + mg.z_uT.toFixed(1), 'µT');
    },
    get magAgeText() {
      const mg = this._mg;
      return ageLine(mg.taken_ms, mg.model || 'magnetometer');
    },

    get imuAvailable() { return !!this._im.available; },
    get imuState() { return sensorStateForPill(this._im.enabled, this._im.valid); },
    get imuAccelHTML() {
      if (this.imuState.state !== 'live') return '';
      const im = this._im;
      return 'Accel ' + vu(im.accel_x_g.toFixed(2) + ' / ' + im.accel_y_g.toFixed(2) + ' / ' + im.accel_z_g.toFixed(2), 'g');
    },
    get imuGyroHTML() {
      if (this.imuState.state !== 'live') return '';
      const im = this._im;
      return 'Gyro  ' + vu(im.gyro_x_dps.toFixed(1) + ' / ' + im.gyro_y_dps.toFixed(1) + ' / ' + im.gyro_z_dps.toFixed(1), '°/s') + ' · ' +
             vu(im.temp_c.toFixed(1), '°C');
    },
    get imuAgeText() {
      const im = this._im;
      return ageLine(im.taken_ms, im.model || 'imu');
    },

  };
}

// Human labels for the telemetry sender's last_result slugs.
const TELE_RESULT_LABELS = {
  never: 'On', sent: 'Sent', delivered: 'Delivered', failed: 'Failed',
  finding_route: 'Finding route', config_error: 'Check address',
  no_identity: 'No identity', no_readings: 'No readings',
};

// ---- Collector reports: one model (state.forms.collector) shared by
// the Settings form and the system popover's status line.
function _collectorApply(r) {
  Object.assign(state.forms.collector, {
    enabled: !!r.enabled,
    collectors: Array.isArray(r.collectors) ? r.collectors.slice() : [],
    identity: r.identity || '', interval_s: r.interval_s || 900,
    battery: r.battery !== false, location: r.location !== false,
    environment: r.environment !== false, compass: !!r.compass,
    diag: !!r.diag,
    last_result: r.last_result || 'never',
    last_error:  r.last_error || '',
    last_sent_epoch: r.last_sent_epoch || 0,
  });
}
async function populateCollectorConfig() {
  try { _collectorApply(await state.transport._req(API.TELEMETRY_CONFIG)); }
  catch (e) { /* keep last-known values */ }
}
async function saveCollectorConfig() {
  const c = state.forms.collector;
  const collectors = c.collectors.map(s => (s || '').trim()).filter(Boolean);
  const body = { enabled: c.enabled, collectors, identity: c.identity,
                 interval_s: c.interval_s, battery: c.battery, location: c.location,
                 environment: c.environment, compass: c.compass, diag: c.diag };
  try {
    _collectorApply(await state.transport._req(API.TELEMETRY_CONFIG, { method: 'POST', body }));
  } catch (e) {
    toast('Could not save: ' + (e.message || e), 'error');
    await populateCollectorConfig();
  }
}
// Append a blank row for the user to type into; saved on @change once
// it holds a value. No save here, so an empty row is not yet posted.
function addCollector() {
  state.forms.collector.collectors.push('');
}
function removeCollector(i) {
  state.forms.collector.collectors.splice(i, 1);
  saveCollectorConfig();
}
function collectorSendNow(btn) {
  return withBusy(btn, async () => {
    try {
      const r = await state.transport._req(API.TELEMETRY_SEND, { method: 'POST', body: {} });
      _collectorApply(r);
      if (r.accepted) toast('Readings sent', 'success');
      else toast('Not sent: ' + (r.last_error || r.last_result), 'error');
    } catch (e) { toast('Send failed: ' + (e.message || e), 'error'); }
  });
}
function collectorPillClass() {
  const c = state.forms.collector;
  if (!c.enabled) return 'pill';
  if (c.last_result === 'failed' || c.last_result === 'config_error' ||
      c.last_result === 'no_identity' || c.last_result === 'no_readings') return 'pill bad';
  return 'pill ok';
}
function collectorPillLabel() {
  const c = state.forms.collector;
  if (!c.enabled) return 'Off';
  return TELE_RESULT_LABELS[c.last_result] || c.last_result;
}
// Sensor-style status line: "Last sent 54s ago (f41214)".
function collectorStatusText() {
  const c = state.forms.collector;
  if (!c.enabled) return 'Periodic sensor reports over LXMF. Off.';
  const n = (c.collectors || []).filter(Boolean).length;
  const dest = n === 1 ? ' (' + c.collectors[0].slice(0, 6) + ')'
             : n > 1 ? ' (' + n + ' collectors)' : '';
  if (!c.last_sent_epoch) return 'Not sent yet' + dest;
  const sec = Math.max(0, Math.round(Date.now() / 1000 - c.last_sent_epoch));
  const t = sec < 60 ? sec + 's' : sec < 3600 ? Math.round(sec / 60) + 'm'
          : Math.round(sec / 3600) + 'h';
  return 'Last sent ' + t + ' ago' + dest;
}

// The legacy storage-migrate click handler had no name; expose one so
// the new @click in markup can reach it.
function onClickStorageMigrate(btn) {
  return withBusy(btn, async () => {
    const ok = await showConfirm({
      title: 'Move attachments to SD card?',
      body: 'Frees device flash storage. The files become unreachable if the SD card is later removed.',
      okLabel: 'Move',
    });
    if (!ok) return;
    try {
      const r = await state.transport._req(API.STORAGE_MIGRATE, { method: 'POST', body: {} });
      toast('Moved ' + r.moved + ' / ' + (r.moved + r.skipped + r.failed) + ' attachments to SD', 'ok', 6000);
    } catch (e) { toast('Migration failed: ' + (e.message || e), 'error'); }
  }, { label: 'Moving…' });
}

// Build / refresh the per-sensor enable config rows whenever
// the system snapshot changes - declarative bindings handle the sensor
// values but the config inputs are still imperatively constructed
// inside updateSensorConfigUI. The Alpine x-effect wiring on the
// system popover dispatches this for us.
function refreshSensorConfigUI() {
  if (state.system) updateSensorConfigUI(state.system);
}

function tickSystemClock() {
  // 1 Hz pulse that advances every "Last read Xs ago" label and the
  // live device-clock display against the WS-pinned clock anchor.
  // Writes to state.ui slots; the popover markup binds x-text against
  // them and re-renders for free.
  if (!state.popovers.system) return;
  // Device clock. Tick both the primary wall-clock display and the
  // "Last calibrated Xs ago" subline so the age advances live.
  // Project the live wall-clock from the WS-pinned anchor (state.clockAnchor) -
  // the same source the calibrated-age subline below uses. The old
  // sysPopover.clockAnchor was orphaned by the WS clock-anchor refactor, which
  // left this current-time line blank.
  const a = state.clockAnchor;
  if (a && a.deviceUnixMs > 0) {
    const nowMs = a.deviceUnixMs + (performance.now() - (a.localPerfMs || 0));
    state.ui.sysClockNow = fmtWallClock(nowMs);
  }
  // Telemetry status line ages like the sensor "Last read" lines.
  state.ui.sysTeleStatus = collectorStatusText();
  {
    const anchor = state.clockAnchor || {};
    const lastMs = Number(anchor.lastCalibratedMs || 0);
    const dnow   = deviceMillisNow();
    const src    = (anchor.source || 'unknown').toUpperCase();
    if (lastMs > 0 && dnow !== null) {
      const age_s = Math.max(0, Math.round((dnow - lastMs) / 1000));
      state.ui.sysClockSource = 'Last calibrated ' + compactAgo(age_s) + ' (' + src + ')';
    } else {
      state.ui.sysClockSource = 'Source: ' + src;
    }
  }
  // Per-sensor age labels. Same ageLine helper renderSystemPopover
  // uses on initial open - now at module scope so this can call it.
  const s = (state.system && state.system.sensors) || {};
  const g = s.gps || {};
  if (g.available) {
    // Report the last *fix*, not the last NMEA byte - the L76K keeps emitting
    // sentences (and ticking last_byte_ms) while only acquiring, so a "last
    // read 1s ago" was misleading. last_valid_fix_ms is the device-millis of
    // the last valid fix (-1/absent = never fixed since boot).
    const model = g.model || 'GPS';
    const fixMs = g.last_valid_fix_ms;
    if (typeof fixMs === 'number' && fixMs > 0) {
      const now = deviceMillisNow();
      state.ui.sysGpsAge = now === null ? ''
        : 'Last fix ' + compactAgo((now - fixMs) / 1000) + ' (' + model + ')';
    } else {
      state.ui.sysGpsAge = 'No fix since boot (' + model + ')';
    }
  }
  const env = s.environment || {};
  if (env.available) {
    state.ui.sysBmeAge = ageLine(env.taken_ms,
        env.model || 'environment', env.enabled && !env.valid);
  }
  const mg = s.magnetometer || {};
  if (mg.available) {
    state.ui.sysMagAge = ageLine(mg.taken_ms,
        mg.model || 'magnetometer', mg.enabled && !mg.valid);
  }
  const im = s.imu || {};
  if (im.available) {
    state.ui.sysImuAge = ageLine(im.taken_ms,
        im.model || 'imu', im.enabled && !im.valid);
  }
}

// While the sensors popover is open, ask the device to stream live
// sensor data (it polls the I2C sensors fast + drains the WS faster
// for as long as we keep asking; closing the popover stops the asks
// and the demand expires).
function requestSensorLive(on = true) {
  if (window.__wsSend) window.__wsSend({ type: 'sensor_live', on: !!on });
}
function startSystemTicker() {
  if (sysPopover.ticker) return;
  requestSensorLive();
  // Populate the clock + "Last calibrated" + per-sensor age lines immediately
  // so they're visible the instant the popover opens, rather than blank until
  // the first 1 Hz tick lands ~1 s later. openSystemPopover() sets
  // state.popovers.system = true before calling us, so tickSystemClock()'s
  // guard passes.
  tickSystemClock();
  sysPopover.ticker = setInterval(() => { tickSystemClock(); requestSensorLive(); }, 1000);
}

function stopSystemTicker() {
  if (sysPopover.ticker) {
    clearInterval(sysPopover.ticker);
    sysPopover.ticker = null;
    // Closing stops the asks; also tell the device to drop the live
    // demand now instead of waiting out the TTL (and so the last frame
    // is on:false, not a racing tick's on:true).
    requestSensorLive(false);
  }
}

async function openSystemPopover() {
  // System snapshot is WS-pushed - render from cache. If the WS hasn't
  // delivered the first frame yet (rare; hello fires immediately on
  // auth), defer until it has.
  if (!state.system) {
    toast('System snapshot still loading…', 'info');
    return;
  }
  // Popover content is reactive - no imperative render call.
  state.popovers.status = false;
  state.popovers.system = true;
  startSystemTicker();
  setTimeout(() => {
    const dismiss = (ev) => {
      if (popoverDismissibleClick(ev, 'popover-system', 'btn-system')) {
        state.popovers.system = false;
        stopSystemTicker();
        document.removeEventListener('click', dismiss);
      }
    };
    document.addEventListener('click', dismiss);
  }, 0);
}

function onClickSystemBtn() {
  if (state.popovers.system) {
    state.popovers.system = false;
    stopSystemTicker();
    return;
  }
  openSystemPopover();
}

// Power popover - anchored to #btn-power. Battery detail data flows
// through reactive getters; the template in #popover-power binds to
// stateClass / stateLabel / valuesHTML / slopeHTML.
function powerPopoverData() {
  return {
    get _b() { return (this.$store.s.system && this.$store.s.system.battery) || {}; },
    get _sum() { return (this.$store.s.lastInfo && this.$store.s.lastInfo.battery) || {}; },
    get stateClass() {
      const s = this._sum;
      if (s.state === 'charging' || s.state === 'charged') return 'pill ok';
      if (s.state === 'discharging' && typeof s.percent === 'number') {
        if (s.percent < 15) return 'pill bad';
        if (s.percent < 30) return 'pill warn';
      }
      return 'pill';
    },
    get stateLabel() {
      const st = this._sum.state;
      return st === 'charging' ? 'Charging'
           : st === 'charged'  ? 'Full'
           : st === 'discharging' ? 'Discharging'
           : 'Unknown';
    },
    get valuesHTML() {
      const b = this._b, s = this._sum;
      const parts = [];
      if (typeof b.voltage_v === 'number') parts.push(vu(b.voltage_v.toFixed(2), 'V'));
      if (typeof s.percent === 'number' && s.percent >= 0) parts.push(vu(s.percent, '%'));
      if (b.vbus_present) parts.push('USB ' + vu((b.vbus_voltage_v || 0).toFixed(2), 'V'));
      return parts.join(' · ');
    },
    get slopeHTML() {
      const b = this._b;
      const parts = [];
      if (typeof b.slope_mv_per_min === 'number') {
        const sign = b.slope_mv_per_min < 0 ? '' : '+';
        const win  = Math.round((b.slope_window_ms || 0) / 60000);
        parts.push('Slope ' + sign + vu(b.slope_mv_per_min.toFixed(1), 'mV/min') + ' (' + win + ' min)');
      }
      if (typeof b.discharge_ma === 'number') parts.push('Draw ' + vu(b.discharge_ma.toFixed(0), 'mA'));
      if (typeof b.pmu_temp_c   === 'number') parts.push('PMU ' + vu(b.pmu_temp_c.toFixed(1), '°C'));
      return parts.join(' · ');
    },
  };
}
let _pwTicker = null;
async function openPowerPopover() {
  if (!state.system) {
    toast('System snapshot still loading…', 'info');
    return;
  }
  // Template re-renders automatically from $store.s.system; no explicit
  // call needed.
  state.popovers.status = false;
  state.popovers.system = false;
  state.popovers.power = true;
  if (!_pwTicker) {
    // The WS `system_update` keeps state.system fresh every 30 s; this
    // ticker is a noop now that the template is reactive. Kept for
    // future force-refresh hooks.
    // No-op tick - template binds reactively to state.system.
    _pwTicker = setInterval(() => {}, 5000);
  }
  setTimeout(() => {
    const dismiss = (ev) => {
      if (popoverDismissibleClick(ev, 'popover-power', 'btn-power')) {
        state.popovers.power = false;
        if (_pwTicker) { clearInterval(_pwTicker); _pwTicker = null; }
        document.removeEventListener('click', dismiss);
      }
    };
    document.addEventListener('click', dismiss);
  }, 0);
}
function onClickPowerBtn() {
  if (state.popovers.power) {
    state.popovers.power = false;
    if (_pwTicker) { clearInterval(_pwTicker); _pwTicker = null; }
    return;
  }
  openPowerPopover();
}
// onClickStorageMigrate handler is declared above; @click in markup
// points at it. Popover content auto-rerenders from the reactive
// $store.s.system snapshot.

// ============================== SYSTEM POPOVER - sensor config ==============================
// Each sensor section (Environment / Compass / Motion) expands to an
// enable toggle plus, where the sensor drives a feature, that feature's
// controls. Environment has "Pressure trend" (a time series: toggle +
// interval, where interval x 48 points = the graph window). Motion has
// "Motion wake" (an interrupt-driven event: toggle only, no interval).
// GPS keeps a location interval (its receiver duty-cycles and cannot be
// read on demand). All save through /api/sensors/config.

// Pressure-trend sample intervals; interval x 48 points = trend window
// (1 min -> ~48 min, 30 min -> ~24 h).
const TREND_INTERVALS = [
  { value: 60,   label: 'Every minute' },
  { value: 300,  label: 'Every 5 min' },
  { value: 600,  label: 'Every 10 min' },
  { value: 1800, label: 'Every 30 min' },
];
// The feature object key for a sensor ('trend' / 'motion_wake'), or null.
function sensorFeatureKey(key) {
  return key === 'environment' ? 'trend' : key === 'imu' ? 'motion_wake' : null;
}
function sensorFeature(key) {
  const s = state.system && state.system.sensors && state.system.sensors[key];
  const fk = sensorFeatureKey(key);
  return (s && fk && s[fk]) ? s[fk] : {};
}
function featureEnabled(key) { return !!sensorFeature(key).enabled; }
// Current trend interval (environment only) snapped to a preset.
function featureIntervalCurrent(key) {
  const cur = Number(sensorFeature(key).interval_s) || TREND_INTERVALS[1].value;
  for (const p of TREND_INTERVALS) if (p.value === cur) return String(p.value);
  let best = TREND_INTERVALS[1].value, bestD = Infinity;
  for (const p of TREND_INTERVALS) {
    const d = Math.abs(Math.log(Math.max(cur, 1)) - Math.log(p.value));
    if (d < bestD) { best = p.value; bestD = d; }
  }
  return String(best);
}

// Send cadence for telemetry-to-collector. Floor matches the
// firmware's MIN_INTERVAL_S clamp.
const TELEMETRY_INTERVALS = [
  { value: 300,   label: 'Every 5 min' },
  { value: 900,   label: 'Every 15 min' },
  { value: 3600,  label: 'Every hour' },
  { value: 21600, label: 'Every 6 hours' },
  { value: 86400, label: 'Every day' },
];
// GPS location-update presets (the receiver power schedule, saved via
// /api/sensors/config like the other sensors):
//   0 .. <300s  -> receiver stays powered (continuous)
//   >= 300s     -> pulsed: full-power acquisition, then sleep between fixes
const GPS_INTERVALS = [
  { value: 0,    label: 'Always on' },
  { value: 300,  label: 'Every 5 min' },
  { value: 900,  label: 'Every 15 min' },
  { value: 1800, label: 'Every 30 min' },
  { value: 3600, label: 'Every hour' },
];
function gpsIntervalSnap(secs) {
  const n = Number(secs) || 0;
  for (const p of GPS_INTERVALS) if (p.value === n) return p.value;
  if (n < 300) return 0;   // below the firmware's pulse threshold = "always on"
  let best = 300, bestDist = Infinity;
  for (const p of GPS_INTERVALS) {
    if (p.value === 0) continue;
    const d = Math.abs(Math.log(Math.max(n, 1)) - Math.log(p.value));
    if (d < bestDist) { best = p.value; bestDist = d; }
  }
  return best;
}
// GPS is a normal sensor now: enable + location interval go through the
// shared onSensorConfigChange (POST /api/sensors/config) like every other
// sensor, and the clock-sync "Refresh" lives in the time-sources list.
// This just snaps the current location interval for the popover select.
function gpsLocationCurrent() {
  const sys = (state.system && state.system.sensors && state.system.sensors.gps) || {};
  return String(gpsIntervalSnap(Math.round((sys.interval_ms || 0) / 1000)));
}

// @change handler for the inputs in each sensor-config row, addressed by
// data-role: the enable toggle is always present; GPS adds an interval
// select; environment/imu add a feature toggle (and environment a feature
// interval). One change fires one POST carrying the whole row's state.
async function onSensorConfigChange(key, sourceEl) {
  const section = sourceEl.closest('[data-sensor]');
  if (!section) return;
  const cb      = section.querySelector('.popover-config [data-role=enable]');
  if (!cb) return;
  const gpsSel  = section.querySelector('.popover-config [data-role=gps-interval]');
  const featCb  = section.querySelector('.popover-config [data-role=feature-enable]');
  const featSel = section.querySelector('.popover-config [data-role=feature-interval]');
  const featKey = sensorFeatureKey(key);
  const body = { sensor: key, enabled: cb.checked };
  if (gpsSel) body.interval_s = parseInt(gpsSel.value, 10);
  if (featCb && featKey) {
    body[featKey] = { enabled: featCb.checked };
    if (featSel) body[featKey].interval_s = parseInt(featSel.value, 10);
  }
  try {
    await state.transport._req(API.SENSORS_CONFIG, { method: 'POST', body });
    const s = state.system && state.system.sensors && state.system.sensors[key];
    if (s) {
      s.enabled = cb.checked;
      if (gpsSel) s.interval_ms = parseInt(gpsSel.value, 10) * 1000;
      if (featCb && featKey) {
        s[featKey] = Object.assign({}, s[featKey], { enabled: featCb.checked });
        if (featSel) s[featKey].interval_s = parseInt(featSel.value, 10);
      }
    }
  } catch (e) {
    toast('Sensor config save failed: ' + (e.message || e), 'error');
  }
}

// No-op kept for any external callers; the popover sections now bind
// open/close declaratively via the sectionOpen map on the popover's
// Alpine component (header @click toggles, config / .is-open read).
function attachSensorConfigToggle(_section) { /* declarative */ }

// Each sensor section's .popover-config block now renders its
// checkbox + interval select declaratively against state.system.
// updateSensorConfigUI just wires the expander toggle on each
// section header - that's a one-shot setup that survives reactive
// rerenders.
function updateSensorConfigUI(_s) {
  const map = {
    environment:  'sys-section-env',
    magnetometer: 'sys-section-mag',
    imu:          'sys-section-imu',
    gps:          'sys-section-gps',
  };
  for (const secId of Object.values(map)) {
    const section = $(secId);
    if (section) attachSensorConfigToggle(section);
  }
}
function onClickToggleTransport(btn) {
  return withBusy(btn, async () => {
    const cur = !!(state.lastInfo && state.lastInfo.transport && state.lastInfo.transport.enabled);
    try {
      await state.transport.setTransportEnabled(!cur);
      toast(cur ? 'Transport disabled' : 'Transport enabled', 'success');
      await refreshStatus();
    } catch (e) {
      toast('Toggle failed: ' + (e.message || e), 'error');
    }
  });
}
async function onClickForceSoftap(btn) {
  const r = await showConfirm({
    title: 'Switch to softAP?',
    body: 'You will be disconnected; reconnect to the device\'s bootstrap WiFi to continue. The saved network is kept; a reboot will try it again.',
    okLabel: 'Switch to softAP',
    destructive: true,
    needsCode: true,
  });
  if (!r) return;
  await withBusy(btn, async () => {
    try {
      await state.transport._req(API.WIFI_SOFTAP, { method: 'POST', body: { identity_code: r.code } });
      toast('Device switching to softAP. Reconnect to the bootstrap network.', 'info');
      state.popovers.status = false;
    } catch (e) {
      toast('softAP switch failed: ' + (e.message || e), 'error');
    }
  }, { label: 'Switching…' });
}
// No periodic /api/info poll. The radio-status indicator is fetched once on
// app-enter (enterApp -> refreshStatus) and re-fetched after the actions that
// change it (radio save, transport toggle); live system state (sensors,
// battery, storage, radio/network telemetry) streams from the WebSocket
// (system_update / *_telemetry frames), so there's nothing left to poll for.

// ============================== ANNOUNCE COUNTDOWN ==============================
// Topbar megaphone icon pulses when auto-announce is enabled and shows
// the time until the next auto-fire next to it. Updates every second.
// state.nextAnnounceAtMs is the Date.now() epoch ms at which the next
// auto-announce is expected (best-effort estimate from the server's
// `next_announce_in_ms` plus local clock).
function seedAnnounceCountdown(nextInMs, intervalMs) {
  if (state.self) state.self.announce_interval_ms = intervalMs;
  if (intervalMs > 0) {
    state.nextAnnounceAtMs = Date.now() + (nextInMs > 0 ? nextInMs : intervalMs);
  } else {
    state.nextAnnounceAtMs = 0;
  }
  refreshAnnounceCountdown();
}
function refreshAnnounceCountdown() {
  const interval = Number(state.self && state.self.announce_interval_ms) || 0;
  if (interval <= 0) {
    // No auto-announce. Dot is yellow if the user manually fired one
    // this boot, grey otherwise.
    state.ui.announceState = state.announcedThisBoot ? 'sent' : 'none';
    state.ui.announceLabelText = '';
    state.ui.announceTitle = state.announcedThisBoot
      ? 'Announce me. Auto-announce is OFF (manual announce sent this boot)'
      : 'Announce me. Auto-announce is OFF for this identity';
    return;
  }
  // Auto-announce on.
  state.ui.announceState = 'auto';
  const remainMs = Math.max(0, (state.nextAnnounceAtMs || 0) - Date.now());
  state.ui.announceLabelText = formatCountdown(remainMs);
  state.ui.announceTitle = 'Announce me. Auto-announce in ' + formatCountdown(remainMs);
  if (remainMs === 0) {
    // The server should have announced; nudge our local target forward
    // so we don't sit at 0 forever if the server announce is delayed.
    state.nextAnnounceAtMs = Date.now() + interval;
  }
}
function formatCountdown(ms) {
  const s = Math.ceil(ms / 1000);
  if (s < 60) return s + 's';
  const m = Math.floor(s / 60);
  if (m < 60) return m + 'm' + (s % 60 ? ' ' + (s % 60) + 's' : '');
  const h = Math.floor(m / 60);
  return h + 'h' + (m % 60 ? ' ' + (m % 60) + 'm' : '');
}
setInterval(refreshAnnounceCountdown, 1000);
// Re-render the conv list every 30s to keep "last seen" labels fresh.
// Cheap: state.conversations is small and the DOM diff is wholesale.
setInterval(() => {
  if (state.view === 'view-app') renderConversations();
}, 30000);

function persistContacts() { localStorage.setItem(LS.CONTACTS, JSON.stringify(state.contacts)); }

