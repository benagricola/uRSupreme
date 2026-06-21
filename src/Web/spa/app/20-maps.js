// ============================== TELEMETRY ATTACH ==============================
// Live-share windows offered for telemetry updates. 0 = one-shot.
// The server clamps anything above 24 h.
const TELEMETRY_SHARE_OPTIONS = [
  { s: 0,     label: 'None' },
  { s: 900,   label: '15 minutes' },
  { s: 3600,  label: '1 hour' },
  { s: 14400, label: '4 hours' },
];
// How often the receiver's device fetches fresh readings during the
// live window. The sender picks this; the receiver's firmware honors
// it (floor 15 s server-side).
const TELEMETRY_RATE_OPTIONS = [
  { s: 30,   label: 'Every 30 seconds' },
  { s: 60,   label: 'Every minute' },
  { s: 300,  label: 'Every 5 minutes' },
  { s: 900,  label: 'Every 15 minutes' },
];
const TELEMETRY_ITEMS = [
  { key: 'location',    ico: 'ico-pin',     label: 'Position' },
  { key: 'compass',     ico: 'ico-compass', label: 'Heading' },
  { key: 'environment', ico: 'ico-thermo',  label: 'Environment' },
  { key: 'battery',     ico: 'ico-battery', label: 'Battery' },
];

// True when the compose box has a telemetry attach with at least one
// item selected. All-off counts as detached (nothing would be sent).
function telemetryAttachActive() {
  const t = state.forms.compose.telemetry;
  return !!(t && (t.location || t.environment || t.battery || t.compass));
}

// Tray-chip: the selected items as icon classes, plus the live window
// as a text suffix.
function telemetryChipIcons() {
  const t = state.forms.compose.telemetry;
  if (!t) return [];
  return TELEMETRY_ITEMS.filter(i => t[i.key]).map(i => i.ico);
}
function telemetryChipSuffix() {
  const t = state.forms.compose.telemetry;
  if (!t || !(t.share_s > 0)) return '';
  const o = TELEMETRY_SHARE_OPTIONS.find(o => o.s === t.share_s);
  return 'live ' + (o ? o.label.toLowerCase() : Math.round(t.share_s / 60) + ' min');
}
function telemetryChipTitle() {
  const t = state.forms.compose.telemetry;
  if (!t) return '';
  return TELEMETRY_ITEMS.filter(i => t[i.key]).map(i => i.label).join(', ');
}

// Pin-button click. First open seeds the selection from the identity's
// saved defaults (Settings → Identity); after that the object persists
// until sent, removed, or the chat switches.
function toggleTelemetryPopover(ev) {
  if (state.popovers.telemetry) { state.popovers.telemetry = false; return; }
  if (!state.forms.compose.telemetry) {
    const d = (state.self && state.self.telemetry) || {};
    state.forms.compose.telemetry = {
      location:    d.location !== false,
      environment: !!d.environment,
      battery:     !!d.battery,
      compass:     !!d.compass,
      share_s:     Number(d.share_s || 0),
      rate_s:      Number(d.rate_s || 60),
    };
  }
  const pop    = $('popup-telemetry');
  const anchor = ev.currentTarget;
  const rect   = anchor.getBoundingClientRect();
  pop.style.left   = Math.max(8, rect.left) + 'px';
  pop.style.bottom = (window.innerHeight - rect.top + 6) + 'px';
  state.popovers.telemetry = true;
  setTimeout(() => {
    const dismiss = (e2) => {
      if (!pop.contains(e2.target) && !anchor.contains(e2.target)) {
        state.popovers.telemetry = false;
        document.removeEventListener('click', dismiss);
      }
    };
    document.addEventListener('click', dismiss);
  }, 0);
}

// Decoded readings (`tele` from the device serializers) as display
// lines for a message bubble: one line per sensor group.
function telemetryLines(m) {
  const t = m.tele;
  if (!t) return [];
  const L = [];
  if (typeof t.lat === 'number') {
    let txt = t.lat.toFixed(5) + ', ' + t.lon.toFixed(5);
    if (typeof t.acc_m === 'number' && t.acc_m > 0) txt += ' ±' + Math.round(t.acc_m) + ' m';
    if (typeof t.speed_kmh === 'number' && t.speed_kmh >= 1) txt += ' · ' + Math.round(t.speed_kmh) + ' km/h';
    L.push({ ico: 'ico-pin', txt, latlon: [t.lat, t.lon] });
    // Distance from the peer's position to ours - only when our own GPS has a
    // fix (the device this app is talking to).
    const sg = state.system && state.system.sensors && state.system.sensors.gps;
    if (sg && sg.valid && typeof sg.latitude === 'number') {
      const km = haversineKm([sg.latitude, sg.longitude], [t.lat, t.lon]);
      const dtxt = km >= 1 ? km.toFixed(km < 10 ? 1 : 0) + ' km' : Math.round(km * 1000) + ' m';
      L.push({ ico: 'ico-activity', txt: dtxt + ' away' });
    }
    // Course over ground (GPS heading) as a cardinal - distinct from the
    // magnetometer heading below (where the device is pointing).
    if (typeof t.bearing_deg === 'number') {
      const deg = Math.round((t.bearing_deg % 360 + 360) % 360);
      L.push({ ico: 'ico-compass', txt: 'Heading ' + deg + '° ' + cardinal(deg) });
    }
  }
  if (typeof t.mag_x === 'number') {
    // Uncompensated heading from the horizontal field components -
    // accurate when the device sits flat.
    const deg = Math.round((Math.atan2(t.mag_y, t.mag_x) * 180 / Math.PI + 360) % 360);
    L.push({ ico: 'ico-compass', txt: deg + '° ' + cardinal(deg) });
  }
  const env = [];
  if (typeof t.temp_c === 'number')  env.push(t.temp_c.toFixed(1) + ' °C');
  if (typeof t.hum_pct === 'number') env.push(Math.round(t.hum_pct) + ' %');
  if (typeof t.mbar === 'number')    env.push(Math.round(t.mbar) + ' mbar');
  if (env.length) L.push({ ico: 'ico-thermo', txt: env.join(' · ') });
  if (typeof t.bat_pct === 'number') {
    L.push({ ico: 'ico-battery', txt: Math.round(t.bat_pct) + '%' + (t.bat_chg ? ' charging' : '') });
  }
  return L;
}

// ---- Location map: modal ------------------------------------------
// Device range-served URL for one vector layer file (token in the query).
function layerUrl(file) {
  return API.MAP_LAYER(file) + '?token=' + encodeURIComponent(state.token || '');
}
// One protomaps-leaflet vector layer for a layer descriptor. maxDataZoom =
// the file's own max zoom, so zooming past it overzooms (coarse data scaled)
// instead of blanking; downloaded areas are clipped to their bounds, the
// world base stays global underneath them.
function _vectorLayer(ly, isWorld) {
  const L = window.L;
  // &v=size busts the browser cache when a layer of the same name is
  // re-downloaded (different size); within a session the URL is stable so
  // tiles are served from cache and the SD isn't re-read on every pan.
  // updateWhenIdle: load tiles when the pan/zoom stops, not continuously,
  // so a slow SD card isn't thrashed fetching tiles you scroll straight past.
  const opts = { url: layerUrl(ly.file) + '&v=' + (ly.size || 0), flavor: 'dark', lang: 'en',
                 updateWhenIdle: true, keepBuffer: 3 };
  if (ly.maxzoom > 0) opts.maxDataZoom = ly.maxzoom;
  if (!isWorld && typeof ly.s === 'number')
    opts.bounds = L.latLngBounds([ly.s, ly.w], [ly.n, ly.e]);
  const lyr = window.protomapsL.leafletLayer(opts);
  // The dark flavor paints each tile's background (#34373d, the water colour)
  // before the land/features draw on top. The world base gets the app bg so a
  // tile still loading from the slow SD blends into the theme instead of
  // flashing a light-grey panel. Detail-area layers stay transparent: an opaque
  // background on a layer stacked above the world would paint its whole bounding
  // tile, hiding the world base everywhere the area's coarse low-zoom tile
  // covers (a permanent blank over Europe/US at the world view).
  lyr.backgroundColor = isWorld ? '#0d1117' : null;
  return lyr;
}

// The map view shows every peer that is sharing a position, with a track
// and heading. Tracks are kept in the browser only (this session), capped.
const MAP_TRACK_MAX = 300;
let _map = null;                 // the single Leaflet map instance
const _mapLayers = {};           // peer hex -> { marker, track }
const _mapTracks = {};           // peer hex -> [{lat, lon, bearing}] live history
let _gpxLayer = null;            // the track drawn when the map is opened from a GPX

// Great-circle bearing from point a to b ([lat, lon]), 0..360.
function bearingBetween(a, b) {
  const r = Math.PI / 180;
  const y = Math.sin((b[1] - a[1]) * r) * Math.cos(b[0] * r);
  const x = Math.cos(a[0] * r) * Math.sin(b[0] * r)
          - Math.sin(a[0] * r) * Math.cos(b[0] * r) * Math.cos((b[1] - a[1]) * r);
  return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
}
// Great-circle distance in km between two [lat, lon] points.
function haversineKm(a, b) {
  const r = Math.PI / 180, R = 6371;
  const dlat = (b[0] - a[0]) * r, dlon = (b[1] - a[1]) * r;
  const s = Math.sin(dlat / 2) ** 2
          + Math.cos(a[0] * r) * Math.cos(b[0] * r) * Math.sin(dlon / 2) ** 2;
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(s)));
}
function _peerName(peer) {
  const c = state.conversations[peer];
  return (c && c.display_name) || (peer ? peer.slice(0, 8) : '?');
}
// Latest known position for a peer: an active live feed first, else the
// most recent message in the conversation that carried a position.
function peerLatestPosition(peer) {
  const f = (state.telemetryShares.feeds || []).find(x => x.peer === peer);
  if (f && f.tele && typeof f.tele.lat === 'number') return { tele: f.tele, live: true };
  const c = state.conversations[peer];
  if (c) for (let i = c.msgs.length - 1; i >= 0; i--) {
    const t = c.msgs[i].tele;
    if (t && typeof t.lat === 'number') return { tele: t, live: false };
  }
  return null;
}
// ---- GPX tracks -----------------------------------------------------------
// A track renders the same whether the device synthesised the GPX from a live
// telemetry feed or a peer sent a real .gpx as an attachment.
function isGpxAttachment(a) {
  if (!a) return false;
  const n = (a.display_name || a.filename || '').toLowerCase();
  return (a.mime || '').toLowerCase().includes('gpx') || n.endsWith('.gpx');
}
function messageGpx(m) {
  return ((m && m.attachments) || []).find(isGpxAttachment) || null;
}
// Parse GPX trkpt/rtept/wpt into [[lat,lon],...]. Namespace-tolerant; anything
// it can't read yields an empty track rather than throwing.
function parseGpx(text) {
  const pts = [];
  try {
    const doc = new DOMParser().parseFromString(text, 'application/xml');
    if (doc.getElementsByTagName('parsererror').length) return pts;
    for (const el of doc.getElementsByTagName('*')) {
      const ln = (el.localName || el.tagName || '').toLowerCase();
      if (ln !== 'trkpt' && ln !== 'rtept' && ln !== 'wpt') continue;
      const lat = parseFloat(el.getAttribute('lat'));
      const lon = parseFloat(el.getAttribute('lon'));
      if (Number.isFinite(lat) && Number.isFinite(lon)) pts.push([lat, lon]);
    }
  } catch (e) { /* malformed GPX -> empty track */ }
  return pts;
}
// Bubble thumbnail: fetch + parse a GPX once, render a static basemap (the
// protomaps Static renderer - no Leaflet instance, so no per-bubble map to
// leak) with the track drawn on top, cache it as a data URL. state.gpxState
// drives the reactive render; state.gpxThumb holds the image.
const GPX_THUMB_W = 240, GPX_THUMB_H = 132, GPX_THUMB_PAD = 22;
// Track line amber, start green, end red - three distinct colours so the end
// marker doesn't blend into the line. Shared by the bubble thumbnail and the
// full map view.
const GPX_TRACK_COLOR = '#f0883e', GPX_START_COLOR = '#3fb950', GPX_END_COLOR = '#f85149';
// Map config + layer list are normally fetched when the map view opens; a
// bubble thumbnail needs them too (to pick the basemap layer), so load once.
let _mapMetaPromise = null;
function ensureMapMeta() {
  if (!_mapMetaPromise) _mapMetaPromise = (async () => {
    await fetchMapConfig();
    if (state.mapTiles.mode === 'sd') await fetchMapLayers();
  })().catch(() => { _mapMetaPromise = null; });
  return _mapMetaPromise;
}
// Parsed points per GPX, in memory. Seeded once from the file; a live feed
// extends it from the position carried in each telemetry_update (no re-fetch).
const _gpxPts = {};
async function loadGpxThumb(a, force) {
  if (!a || !a.filename) return;
  const f = a.filename;
  if (!force && (state.gpxState[f] === 'loading' || state.gpxState[f] === 'ready')) return;
  state.gpxState[f] = 'loading';
  try {
    await ensureMapMeta();
    const { blob } = await state.transport.fetchAttachment(state.identityId, f);
    const pts = parseGpx(await blob.text());
    if (!pts.length) { state.gpxState[f] = 'failed'; return; }
    _gpxPts[f] = pts;
    state.gpxThumb[f] = await _renderGpxThumb(pts);
    state.gpxState[f] = 'ready';
  } catch (e) { state.gpxState[f] = 'failed'; }
}
// A live sample for an open/visible track: append the point the WS event
// already carried (no file download) and redraw - the thumbnail from the
// in-memory points, the open full track by extending its polyline.
async function gpxLiveAppend(filename, lat, lon) {
  const pts = _gpxPts[filename];
  if (!pts) return;                       // not loaded yet; x-init will fetch it
  const last = pts[pts.length - 1];
  if (last && last[0] === lat && last[1] === lon) return;   // dedupe identical
  pts.push([lat, lon]);
  if (pts.length > 2000) pts.splice(0, pts.length - 2000);   // bound RAM
  if (state.gpxState[filename] === 'ready')
    state.gpxThumb[filename] = await _renderGpxThumb(pts);
  if (_gpxLayer && _gpxLayer._urtnFile === filename) _gpxExtendLine(lat, lon);
}
function gpxState(a) { return (a && state.gpxState[a.filename]) || 'idle'; }
// Slippy-map world pixel of a lat/lon at zoom z (matches the protomaps Static
// renderer's projection, so the track overlays the basemap exactly).
function _gpxMerc(lat, lon, z) {
  const s = 256 * Math.pow(2, z);
  const sy = Math.sin(lat * Math.PI / 180);
  return [(lon + 180) / 360 * s, (0.5 - Math.log((1 + sy) / (1 - sy)) / (4 * Math.PI)) * s];
}
function _gpxBbox(pts) {
  let a = 90, b = -90, c = 180, d = -180;
  for (const [la, lo] of pts) { if (la < a) a = la; if (la > b) b = la; if (lo < c) c = lo; if (lo > d) d = lo; }
  return { minLat: a, maxLat: b, minLon: c, maxLon: d, cLat: (a + b) / 2, cLon: (c + d) / 2 };
}
// Fractional zoom that fits the track's bbox into the padded inner box, so the
// start/end markers always sit clear of the edges (integer zoom-to-fit could
// leave the bbox spanning almost the whole width).
function _gpxFitZoom(bb, w, h, pad) {
  const innerW = Math.max(1, w - 2 * pad), innerH = Math.max(1, h - 2 * pad);
  const p1 = _gpxMerc(bb.maxLat, bb.minLon, 0), p2 = _gpxMerc(bb.minLat, bb.maxLon, 0);
  const spanX = Math.max(1e-6, Math.abs(p2[0] - p1[0])), spanY = Math.max(1e-6, Math.abs(p2[1] - p1[1]));
  const z = Math.min(Math.log2(innerW / spanX), Math.log2(innerH / spanY));
  return Math.max(0, Math.min(18, z));
}
// The detail layer whose bounds contain the track centre (highest max zoom),
// else the world base - so the thumbnail shows streets where we have them.
function _gpxBaseLayer(bb) {
  const layers = state.mapTiles.layers || [];
  let best = null;
  for (const ly of layers) {
    if (ly.id === 'world' || typeof ly.s !== 'number') continue;
    if (bb.cLat >= ly.s && bb.cLat <= ly.n && bb.cLon >= ly.w && bb.cLon <= ly.e
        && (!best || (ly.maxzoom || 0) > (best.maxzoom || 0))) best = ly;
  }
  return best || layers.find(l => l.id === 'world') || null;
}
async function _renderGpxThumb(pts) {
  const W = GPX_THUMB_W, H = GPX_THUMB_H, dpr = window.devicePixelRatio || 1;
  const bb = _gpxBbox(pts), z = _gpxFitZoom(bb, W, H, GPX_THUMB_PAD);
  const canvas = document.createElement('canvas');
  canvas.width = W * dpr; canvas.height = H * dpr;
  const ctx = canvas.getContext('2d');
  // Static vector basemap (best layer for the area). Failure leaves the app
  // background, so the track still shows.
  const ly = state.mapTiles.mode === 'sd' ? _gpxBaseLayer(bb) : null;
  if (ly && window.protomapsL && window.protomapsL.Static) {
    try {
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      const src = new window.protomapsL.Static({
        url: layerUrl(ly.file) + '&v=' + (ly.size || 0), flavor: 'dark', lang: 'en',
        maxDataZoom: ly.maxzoom > 0 ? ly.maxzoom : 15 });
      await src.drawContext(ctx, W, H, { x: bb.cLon, y: bb.cLat }, z);
    } catch (e) { /* basemap unavailable - track on the app background */ }
  }
  // Track overlay, projected to match the basemap.
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const cen = _gpxMerc(bb.cLat, bb.cLon, z);
  const px = ([la, lo]) => { const w = _gpxMerc(la, lo, z); return [w[0] - cen[0] + W / 2, w[1] - cen[1] + H / 2]; };
  ctx.beginPath();
  pts.forEach((p, i) => { const q = px(p); i ? ctx.lineTo(q[0], q[1]) : ctx.moveTo(q[0], q[1]); });
  ctx.lineJoin = ctx.lineCap = 'round'; ctx.lineWidth = 2.5; ctx.strokeStyle = GPX_TRACK_COLOR; ctx.stroke();
  const dot = (p, col) => { const q = px(p); ctx.beginPath(); ctx.arc(q[0], q[1], 4, 0, 2 * Math.PI);
    ctx.fillStyle = col; ctx.fill(); ctx.lineWidth = 1.5; ctx.strokeStyle = '#fff'; ctx.stroke(); };
  dot(pts[0], GPX_START_COLOR);
  if (pts.length > 1) dot(pts[pts.length - 1], GPX_END_COLOR);
  return canvas.toDataURL('image/png');
}
// A position marker: a heading arrow when we know the bearing, else a dot.
// Live peers are red, last-known are blue.
function _peerIcon(headingDeg, live) {
  const color = live ? '#f85149' : '#58a6ff';
  let html;
  if (typeof headingDeg === 'number') {
    html = "<svg width='24' height='24' viewBox='0 0 24 24' style='transform:rotate("
         + headingDeg + "deg)'><path d='M12 2 L19 21 L12 16 L5 21 Z' fill='" + color
         + "' stroke='white' stroke-width='1'/></svg>";
  } else {
    html = "<svg width='16' height='16' viewBox='0 0 16 16'><circle cx='8' cy='8' r='6' fill='"
         + color + "' stroke='white' stroke-width='2'/></svg>";
  }
  return window.L.divIcon({ className: 'peer-pin', iconSize: [24, 24], iconAnchor: [12, 12], html });
}
// Popup built as a DOM node so the peer name needs no escaping.
function _peerPopup(peer, latest) {
  const t = latest.tele, el = document.createElement('div');
  const nm = document.createElement('div'); nm.style.fontWeight = '600'; nm.textContent = _peerName(peer);
  const co = document.createElement('div'); co.style.cssText = 'font-family:ui-monospace,monospace;font-size:11px';
  co.textContent = t.lat.toFixed(5) + ', ' + t.lon.toFixed(5);
  el.appendChild(nm); el.appendChild(co);
  const bits = [];
  if (typeof t.acc_m === 'number' && t.acc_m > 0) bits.push('±' + Math.round(t.acc_m) + ' m');
  if (typeof t.alt_m === 'number') bits.push(Math.round(t.alt_m) + ' m alt');
  if (typeof t.speed_kmh === 'number' && t.speed_kmh >= 1) bits.push(Math.round(t.speed_kmh) + ' km/h');
  if (latest.live) bits.push('live');
  if (bits.length) {
    const d = document.createElement('div'); d.className = 'muted';
    d.style.fontSize = '11px'; d.textContent = bits.join(' · '); el.appendChild(d);
  }
  return el;
}
function _mapInit() {
  const L = window.L; if (!L || _map) return;
  _map = L.map('map-canvas', { zoomControl: true, attributionControl: true, maxZoom: 18 });
  _map.setView([20, 0], 2);   // world-ish until markers arrive
}
// Why the map can't show, or '' when it should be fine. Used to warn the
// user on open rather than leaving a silently blank canvas.
function mapUnavailableReason() {
  const mt = state.mapTiles;
  if (mt.mode === 'off') return 'Map is off. Turn it on in Settings, Map.';
  if (!mt.sdPresent)     return 'No SD card detected for maps. Settings, Map.';
  return (mt.layers && mt.layers.length) ? '' : 'No maps on the card yet. Download one in Settings, Map.';
}
let _mapTileErrorShown = false;   // one missing-tile toast per open
// A part of the map not loading (a vector tile fetch failure) fires
// tileerror; warn once per open.
function _onMapTileErr() {
  if (_mapTileErrorShown) return;
  _mapTileErrorShown = true;
  toast('Some of the map could not be loaded.', 'warn');
}
// (Re)build the base layers for the current source. Vector + SD stacks one
// layer per file: the world base, then each downloaded area clipped on top.
// Map-view loading indicator: a spinner pill while base-layer tiles are still
// being fetched (or the GPX is loading), so it's clear the map isn't done.
// Each layer carries its own loading flag (set from Leaflet GridLayer
// 'loading'/'load' events); the indicator is on if any current layer is
// loading. Recomputing over the live layer set (not a counter) means a layer
// removed mid-load can't leave the spinner stuck; a safety timeout is a final
// backstop if a 'load' never arrives.
let _gpxLoading = false, _mapLoadTimer = null;
function _recomputeMapLoading() {
  const on = _gpxLoading || (_map && _map._urtnLayers || []).some(l => l && l._urtnLoading);
  state.mapView.loading = on;
  clearTimeout(_mapLoadTimer);
  if (on) _mapLoadTimer = setTimeout(() => {
    (_map && _map._urtnLayers || []).forEach(l => { if (l) l._urtnLoading = false; });
    _gpxLoading = false; state.mapView.loading = false;
  }, 25000);
}
function _bindTileLoad(lyr) {
  lyr._urtnLoading = true;   // assume loading until its first 'load'
  lyr.on('loading', () => { lyr._urtnLoading = true; _recomputeMapLoading(); });
  lyr.on('load',    () => { lyr._urtnLoading = false; _recomputeMapLoading(); });
}
function _mapApplyBase() {
  if (!_map) return;
  (_map._urtnLayers || []).forEach(l => _map.removeLayer(l));
  _map._urtnLayers = [];
  const mt = state.mapTiles;
  if (mt.mode === 'off') { _recomputeMapLoading(); return; }
  if (!window.protomapsL) return;
  // World base at the bottom, then areas by max zoom ascending so the most
  // detailed area is drawn last (on top): where areas overlap, the finer one
  // wins instead of an arbitrary filesystem order.
  const layers = (mt.layers || []).slice().sort((a, b) => {
    if (a.id === 'world') return -1;
    if (b.id === 'world') return 1;
    return (a.maxzoom || 0) - (b.maxzoom || 0);
  });
  let z = 1;
  for (const ly of layers) {
    const lyr = _vectorLayer(ly, ly.id === 'world');
    lyr.on('tileerror', _onMapTileErr);
    _bindTileLoad(lyr);
    lyr.setZIndex(z++); lyr.addTo(_map);
    _map._urtnLayers.push(lyr);
  }
  _recomputeMapLoading();
}
function _removePeerLayer(peer) {
  const ly = _mapLayers[peer];
  if (!ly) return;
  if (ly.marker) _map.removeLayer(ly.marker);
  if (ly.track) _map.removeLayer(ly.track);
  delete _mapLayers[peer];
}
// Rebuild every peer's marker + track. focusPeer/focusLatLon centre the view;
// otherwise fit to all markers (first open only).
function mapRefresh(focusPeer, focusLatLon, fit) {
  const L = window.L; if (!_map) return;
  // Viewing a single track (opened from a message): show only that track, not
  // the per-peer position markers - they would stack on the track's own
  // start/end (the live-position arrow sits exactly on the last track point).
  if (state.mapView.gpx) { for (const peer of Object.keys(_mapLayers)) _removePeerLayer(peer); return; }
  const seen = new Set();
  const bounds = [];
  for (const peer of Object.keys(state.conversations)) {
    const latest = peerLatestPosition(peer);
    if (!latest) { _removePeerLayer(peer); continue; }
    seen.add(peer);
    const latlng = [latest.tele.lat, latest.tele.lon];
    // Heading: the reported course, else inferred from the last live step.
    const live = _mapTracks[peer] || [];
    let heading = (typeof latest.tele.bearing_deg === 'number') ? latest.tele.bearing_deg : undefined;
    if (heading === undefined && live.length >= 2)
      heading = bearingBetween([live[live.length - 2].lat, live[live.length - 2].lon], latlng);
    let ly = _mapLayers[peer];
    if (!ly) {
      // Markers only - the route lives in the message's GPX track, shown when
      // the map is opened from that message, not stitched onto the global map.
      ly = _mapLayers[peer] = {
        marker: L.marker(latlng, { icon: _peerIcon(heading, latest.live) }).addTo(_map),
      };
      ly.marker.bindPopup(_peerPopup(peer, latest));
    } else {
      ly.marker.setLatLng(latlng).setIcon(_peerIcon(heading, latest.live));
      ly.marker.setPopupContent(_peerPopup(peer, latest));
    }
    bounds.push(latlng);
  }
  for (const peer of Object.keys(_mapLayers)) if (!seen.has(peer)) _removePeerLayer(peer);
  if (focusLatLon) _map.setView(focusLatLon, state.mapTiles.defaultZoom || 15);
  else if (focusPeer && _mapLayers[focusPeer]) _map.setView(_mapLayers[focusPeer].marker.getLatLng(), state.mapTiles.defaultZoom || 15);
  else if (fit && bounds.length) _map.fitBounds(bounds, { padding: [40, 40], maxZoom: 14 });
  _map.invalidateSize();
}
// Draw a GPX track (polyline + start/end dots), fetching + parsing the
// attachment bytes. Replaces any previously drawn GPX track.
async function _renderGpxOnMap(gpx) {
  const L = window.L; if (!_map || !L) return;
  if (_gpxLayer) { _map.removeLayer(_gpxLayer); _gpxLayer = null; }
  if (!gpx) return;
  _gpxLoading = true; _recomputeMapLoading();
  try {
    let text;
    try {
      const { blob } = await state.transport.fetchAttachment(state.identityId, gpx.filename);
      text = await blob.text();
    } catch (e) { toast('Could not load track: ' + (e.message || e), 'error'); return; }
    const pts = parseGpx(text);
    if (!pts.length) { toast('Track has no points', 'warn'); return; }
    _gpxPts[gpx.filename] = pts;
    const grp = L.layerGroup();
    const line = (pts.length >= 2)
      ? L.polyline(pts, { color: GPX_TRACK_COLOR, weight: 3, opacity: 0.9 }).addTo(grp) : null;
    L.circleMarker(pts[0], { radius: 5, color: '#fff', weight: 2, fillColor: GPX_START_COLOR, fillOpacity: 1 }).addTo(grp);
    const end = (pts.length >= 2)
      ? L.circleMarker(pts[pts.length - 1], { radius: 5, color: '#fff', weight: 2, fillColor: GPX_END_COLOR, fillOpacity: 1 }).addTo(grp)
      : null;
    grp.addTo(_map);
    grp._urtnFile = gpx.filename; grp._urtnLine = line; grp._urtnEnd = end;
    _gpxLayer = grp;
    if (pts.length >= 2) _map.fitBounds(L.latLngBounds(pts).pad(0.2));
    else _map.setView(pts[0], state.mapTiles.defaultZoom || 15);
  } finally { _gpxLoading = false; _recomputeMapLoading(); }
}
// Extend the open track's polyline + move its end marker to a live point, with
// no re-fetch and no re-fit (the view stays put as the track grows).
function _gpxExtendLine(lat, lon) {
  const L = window.L; if (!_gpxLayer || !L) return;
  const ll = [lat, lon];
  if (_gpxLayer._urtnLine) _gpxLayer._urtnLine.addLatLng(ll);
  else _gpxLayer._urtnLine = L.polyline(_gpxPts[_gpxLayer._urtnFile] || [ll],
    { color: GPX_TRACK_COLOR, weight: 3, opacity: 0.9 }).addTo(_gpxLayer);   // 1 pt -> line
  if (_gpxLayer._urtnEnd) _gpxLayer._urtnEnd.setLatLng(ll);
  else _gpxLayer._urtnEnd = L.circleMarker(ll,
    { radius: 5, color: '#fff', weight: 2, fillColor: GPX_END_COLOR, fillOpacity: 1 }).addTo(_gpxLayer);
}
// Open the map view. opts: { peer, latlon } to centre on a shared position,
// or { gpx } (an attachment meta) to draw a track.
function openMapView(opts) {
  opts = opts || {};
  state.mapView.open = true;
  state.mapView.gpx = opts.gpx || null;
  _mapTileErrorShown = false;
  setTimeout(async () => {
    _mapInit();
    await fetchMapConfig();
    if (state.mapTiles.mode === 'sd') await fetchMapLayers();
    _mapApplyBase();
    mapRefresh(opts.peer, opts.latlon, !opts.peer && !opts.latlon && !opts.gpx);
    if (opts.gpx) await _renderGpxOnMap(opts.gpx);
    refreshMapExtract();   // resume the progress strip if a download is running
    // A definite config reason (no card / no maps) trumps a per-tile error,
    // so report it and suppress the tileerror toast.
    const reason = mapUnavailableReason();
    if (reason) { _mapTileErrorShown = true; toast(reason, 'warn'); }
  }, 60);
}
function closeMapView() {
  _clearHighlight();
  if (_gpxLayer && _map) { _map.removeLayer(_gpxLayer); _gpxLayer = null; }
  _gpxLoading = false; clearTimeout(_mapLoadTimer); state.mapView.loading = false;
  state.mapView.gpx = null;
  state.mapView.open = false;
}
// Save the currently-open GPX track to disk (reuses the attachment download).
function downloadGpx() {
  if (state.mapView.gpx) downloadAttachment(state.identityId, state.mapView.gpx);
}
// Live feed update: extend the peer's track and refresh the open map.
function mapLive(ev) {
  if (!ev.tele || typeof ev.tele.lat !== 'number') return;
  const arr = _mapTracks[ev.peer] || (_mapTracks[ev.peer] = []);
  arr.push({ lat: ev.tele.lat, lon: ev.tele.lon, bearing: ev.tele.bearing_deg });
  if (arr.length > MAP_TRACK_MAX) arr.splice(0, arr.length - MAP_TRACK_MAX);
  if (state.mapView.open) mapRefresh();
}
// Mirror the persisted /api/map/config into the live tile state + the
// settings form.
function applyMapConfig(r) {
  const mt = state.mapTiles;
  mt.mode        = r.mode || 'sd';
  mt.sdPresent   = !!r.sd_present;
  mt.defaultZoom = r.default_zoom || 16;
  const pf = state.forms.map || {};
  state.forms.map = { mode: mt.mode, default_zoom: mt.defaultZoom,
                      download_url: pf.download_url || '' };
}
async function fetchMapConfig() {
  try { applyMapConfig(await state.transport._req(API.MAP_CONFIG)); }
  catch (e) { /* keep defaults */ }
}
// The downloaded vector layers (world base + detail areas), each with the
// extent/zoom/size read from its pmtiles header. Drives both the manage list
// and the stacked map layers.
async function fetchMapLayers() {
  try {
    const r = await state.transport._req(API.MAP_LAYERS);
    state.mapTiles.sdPresent = !!r.sd_present;
    state.mapTiles.layers = r.layers || [];
  } catch (e) { state.mapTiles.layers = []; }
}
async function saveMapConfig() {
  const f = state.forms.map;
  try {
    applyMapConfig(await state.transport._req(API.MAP_CONFIG, { method: 'POST', body: {
      mode: f.mode, default_zoom: f.default_zoom,
    }}));
    if (f.mode === 'sd') fetchMapLayers();
  } catch (e) { toast('Could not save map settings: ' + (e.message || e), 'error'); }
}
// One-line status under the source picker in settings.
function mapSourceStatus() {
  const mt = state.mapTiles, f = state.forms.map;
  if (f.mode === 'off') return 'Positions show as coordinates only.';
  if (!mt.sdPresent)    return 'No SD card detected. Load a card to use maps.';
  const nareas = (mt.layers || []).filter(l => l.id !== 'world').length;
  const haveWorld = (mt.layers || []).some(l => l.id === 'world');
  if (!(mt.layers || []).length) return 'Card present. Download a map below to start.';
  return (haveWorld ? 'World base' : 'No world base') + (nareas ? (' + ' + nareas + ' detail area' + (nareas > 1 ? 's' : '')) : '') + '.';
}

// --- Device-side map download -------------------------------------
// The device fetches a .pmtiles file from a URL straight to the SD card on
// a background task; the browser polls /api/map/download for progress.
let _mapDlPoll = null;
function _mapDlApply(r) {
  const d = state.mapDl;
  d.phase   = r.phase || 'idle';
  d.active  = !!r.active;
  d.written = r.written || 0;
  d.total   = (typeof r.total === 'number') ? r.total : -1;
  d.url     = r.url || '';
  d.dest    = r.dest || '';
  d.error   = r.error || '';
}
function _mapDlStopPolling() { if (_mapDlPoll) { clearInterval(_mapDlPoll); _mapDlPoll = null; } }
function _mapDlStartPolling() {
  _mapDlStopPolling();
  _mapDlPoll = setInterval(async () => {
    let r; try { r = await state.transport._req(API.MAP_DOWNLOAD); }
    catch (e) { return; }
    const wasActive = state.mapDl.active;
    _mapDlApply(r);
    if (!state.mapDl.active) {
      _mapDlStopPolling();
      if (wasActive) {
        if (state.mapDl.phase === 'done') {
          toast('Map downloaded to the card.', 'info');
          fetchMapConfig().then(() => { if (state.mapView.open) mapRefresh(); });
        } else if (state.mapDl.phase === 'error') {
          toast('Map download failed: ' + (state.mapDl.error || 'unknown error'), 'error');
        } else if (state.mapDl.phase === 'cancelled') {
          toast('Map download cancelled.', 'info');
        }
      }
    }
  }, 1000);
}
// Pull the current job state once (e.g. on opening settings) so a download
// started elsewhere still shows progress here.
async function refreshMapDownload() {
  try { _mapDlApply(await state.transport._req(API.MAP_DOWNLOAD)); }
  catch (e) { return; }
  if (state.mapDl.active) _mapDlStartPolling();
}
async function startMapDownload() {
  const url = (state.forms.map.download_url || '').trim();
  if (!/^https?:\/\//i.test(url)) { toast('Enter a full http:// or https:// link.', 'warn'); return; }
  try {
    _mapDlApply(await state.transport._req(API.MAP_DOWNLOAD, { method: 'POST', body: { url }}));
    _mapDlStartPolling();
  } catch (e) { toast('Could not start download: ' + (e.message || e), 'error'); }
}
async function cancelMapDownload() {
  try { _mapDlApply(await state.transport._req(API.MAP_DOWNLOAD_CANCEL, { method: 'POST' })); }
  catch (e) { toast('Could not cancel: ' + (e.message || e), 'error'); }
}
function mapDlPct() {
  const d = state.mapDl;
  if (d.total > 0) return Math.min(100, Math.round((d.written / d.total) * 100));
  return d.phase === 'done' ? 100 : 0;
}
function _fmtBytes(n) {
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n >= 1024)    return (n / 1024).toFixed(0) + ' KB';
  return n + ' B';
}
function mapDlStatus() {
  const d = state.mapDl;
  if (d.phase === 'running') {
    if (d.total > 0) return _fmtBytes(d.written) + ' of ' + _fmtBytes(d.total) + ' (' + mapDlPct() + '%)';
    return 'Downloaded ' + _fmtBytes(d.written);
  }
  if (d.phase === 'done')      return 'Saved ' + _fmtBytes(d.written) + ' to the card.';
  if (d.phase === 'error')     return 'Failed: ' + (d.error || 'unknown error') + '.';
  if (d.phase === 'cancelled') return 'Cancelled.';
  return '';
}

// --- Device-side region extract from the planet -------------------
// The device range-reads the Protomaps planet for a chosen area and writes
// a small .pmtiles to the card. The browser polls /api/map/extract.
let _mapExtPoll = null;
function _mapExtApply(r) {
  const d = state.mapExt;
  d.phase      = r.phase || 'idle';
  d.active     = !!r.active;
  d.tiles      = r.tiles_total || 0;
  d.bytesDone  = r.bytes_done || 0;
  d.bytesTotal = r.bytes_total || 0;
  d.dest       = r.dest || '';
  d.error      = r.error || '';
}
function _mapExtStop() { if (_mapExtPoll) { clearInterval(_mapExtPoll); _mapExtPoll = null; } }
function _mapExtStartPolling() {
  _mapExtStop();
  _mapExtPoll = setInterval(async () => {
    let r; try { r = await state.transport._req(API.MAP_EXTRACT); } catch (e) { return; }
    const wasActive = state.mapExt.active;
    _mapExtApply(r);
    if (!state.mapExt.active) {
      _mapExtStop();
      if (wasActive) {
        if (state.mapExt.phase === 'done') {
          toast('Map saved to the card.', 'info');
          fetchMapLayers().then(() => { if (state.mapView.open) { _mapApplyBase(); mapRefresh(); } });
        } else if (state.mapExt.phase === 'error') {
          toast('Map download failed: ' + (state.mapExt.error || 'unknown error'), 'error');
        } else if (state.mapExt.phase === 'cancelled') {
          toast('Map download cancelled.', 'info');
        }
      }
    }
  }, 1500);
}
async function refreshMapExtract() {
  try { _mapExtApply(await state.transport._req(API.MAP_EXTRACT)); }
  catch (e) { return; }
  if (state.mapExt.active) _mapExtStartPolling();
}
function _mapJobBusy() { return state.mapExt.active || state.mapDl.active; }
// Start an extract. `body` carries target(world|area), maxzoom, an optional
// name, and either a bbox or a clip polygon. No url -> the device resolves
// the latest planet.
async function _startExtract(body) {
  if (_mapJobBusy()) { toast('A map download is already running.', 'warn'); return; }
  try {
    _mapExtApply(await state.transport._req(API.MAP_EXTRACT, { method: 'POST', body }));
    _mapExtStartPolling();
    toast('Downloading map. This can take a few minutes.', 'info');
  } catch (e) { toast('Could not start: ' + (e.message || e), 'error'); }
}
// Detail zoom proportional to how far in the map is framed (no hard floor, so
// a wide view downloads at a coarser, bounded zoom rather than blowing the cap).
function _detailZoom() {
  return _map ? Math.min(15, Math.max(Math.round(_map.getZoom()) + 3, 7)) : 12;
}
function extractWorld() { _startExtract({ target: 'world', maxzoom: 6, bbox: { w: -180, s: -85, e: 180, n: 85 } }); }
function extractCurrentView() {
  if (!_map) return;
  const b = _map.getBounds();
  _startExtract({ target: 'area', name: (state.mapDraw.name || '').trim(), maxzoom: _detailZoom(),
                  bbox: { w: b.getWest(), s: b.getSouth(), e: b.getEast(), n: b.getNorth() } });
}
async function cancelMapExtract() {
  try { _mapExtApply(await state.transport._req(API.MAP_EXTRACT_CANCEL, { method: 'POST' })); }
  catch (e) { toast('Could not cancel: ' + (e.message || e), 'error'); }
}

// --- Draw-a-polygon area selection (precise, no surrounding sea) -----
// Vertices are draggable markers; the shape (line at 2, filled polygon at 3)
// is redrawn beneath them. Map panning is disabled while drawing so a tap
// (even with a little mouse jitter) reliably drops a vertex instead of
// panning the map; scroll-zoom still works.
let _drawShape = null;
let _drawMarkers = [];
function _drawVertexIcon() {
  return window.L.divIcon({ className: 'draw-vertex', iconSize: [14, 14], iconAnchor: [7, 7] });
}
function _drawClear() {
  if (_map) {
    if (_drawShape) _map.removeLayer(_drawShape);
    _drawMarkers.forEach(m => _map.removeLayer(m));
  }
  _drawShape = null; _drawMarkers = [];
}
function _drawUpdateShape() {
  const L = window.L; if (!_map) return;
  if (_drawShape) { _map.removeLayer(_drawShape); _drawShape = null; }
  const ll = state.mapDraw.pts.map(p => [p[1], p[0]]);   // [lat,lon]
  if (ll.length >= 3)      _drawShape = L.polygon(ll,  { color: '#58a6ff', weight: 2, fillOpacity: 0.15 });
  else if (ll.length >= 2) _drawShape = L.polyline(ll, { color: '#58a6ff', weight: 2 });
  if (_drawShape) { _drawShape.addTo(_map); _drawShape.bringToBack(); }
}
function _drawAddVertex(lat, lon) {
  const L = window.L;
  state.mapDraw.pts.push([lon, lat]);
  const m = L.marker([lat, lon], { draggable: true, icon: _drawVertexIcon(), keyboard: false, zIndexOffset: 1000 });
  m.on('drag dragend', () => {
    const i = _drawMarkers.indexOf(m); if (i < 0) return;
    const p = m.getLatLng();
    state.mapDraw.pts[i] = [p.lng, p.lat];
    _drawUpdateShape();
  });
  m.addTo(_map); _drawMarkers.push(m);
  _drawUpdateShape();
}
function _onDrawClick(e) { _drawAddVertex(e.latlng.lat, e.latlng.lng); }
function startDrawArea() {
  if (!_map) return;
  cancelDrawArea();
  _clearHighlight();
  state.mapDraw.on = true; state.mapDraw.pts = [];
  _map.dragging.disable();
  _map.on('click', _onDrawClick);
}
function cancelDrawArea() {
  state.mapDraw.on = false; state.mapDraw.pts = [];
  if (_map) { _map.off('click', _onDrawClick); _map.dragging.enable(); }
  _drawClear();
}
function finishDrawArea() {
  if (state.mapDraw.pts.length < 3) { toast('Tap at least 3 points to make an area.', 'warn'); return; }
  const polygon = state.mapDraw.pts.slice();
  const name = (state.mapDraw.name || '').trim();
  cancelDrawArea();
  _startExtract({ target: 'area', name, maxzoom: _detailZoom(), polygon });
}

// --- Manage downloaded layers --------------------------------------
async function deleteMapLayer(file) {
  try {
    await state.transport._req(API.MAP_LAYER(file), { method: 'DELETE' });
    await fetchMapLayers();
    if (state.mapView.open) _mapApplyBase();
  } catch (e) { toast('Could not delete: ' + (e.message || e), 'error'); }
}
// Show a downloaded area on the map: fly to it and outline its true shape
// (the drawn polygon from its sidecar, or the bbox rectangle) so the user
// can see exactly what they'd be deleting.
let _highlightLayer = null;
function _clearHighlight() {
  if (_highlightLayer && _map) _map.removeLayer(_highlightLayer);
  _highlightLayer = null;
}
async function flyToLayer(ly) {
  const L = window.L;
  closeSettingsModal();
  openMapView();
  let poly = null;
  if (ly.id !== 'world') {
    try {
      const r = await state.transport._req(API.MAP_LAYER(ly.id + '.json'));
      if (r && Array.isArray(r.polygon) && r.polygon.length >= 3) poly = r.polygon;
    } catch (e) { /* no sidecar -> outline the bbox instead */ }
  }
  setTimeout(() => {
    if (!_map) return;
    _clearHighlight();
    _highlightLayer = (poly)
      ? L.polygon(poly.map(p => [p[1], p[0]]), { color: '#f0a020', weight: 2, fill: false, dashArray: '5' })
      : L.rectangle([[ly.s, ly.w], [ly.n, ly.e]], { color: '#f0a020', weight: 2, fill: false, dashArray: '5' });
    _highlightLayer.addTo(_map);
    _map.fitBounds([[ly.s, ly.w], [ly.n, ly.e]], { padding: [40, 40] });
  }, 320);
}
function layerLabel(ly) {
  if (ly.id === 'world') return 'World base';
  // Hide the auto-generated id (area-<hash>, or the older area_<lat>_<lon>);
  // the extent line below identifies it. A typed name is shown as-is.
  if (/^area-[0-9a-f]{8}$/.test(ly.id) || /^area_-?\d+_-?\d+$/.test(ly.id)) return 'Downloaded area';
  return ly.id;
}
function layerExtent(ly) {
  const c = (a) => (typeof a === 'number' ? a.toFixed(2) : '?');
  return c((ly.s + ly.n) / 2) + ', ' + c((ly.w + ly.e) / 2) + '  z' + ly.minzoom + '-' + ly.maxzoom + '  ' + _fmtBytes(ly.size || 0);
}
function mapExtPct() {
  const d = state.mapExt;
  if (d.bytesTotal > 0) return Math.min(100, Math.round((d.bytesDone / d.bytesTotal) * 100));
  return d.phase === 'done' ? 100 : 0;
}
function mapExtStatus() {
  const d = state.mapExt;
  if (d.phase === 'scanning') return 'Finding map data...';
  if (d.phase === 'writing')  return _fmtBytes(d.bytesDone) + (d.bytesTotal ? ' / ' + _fmtBytes(d.bytesTotal) : '') + ' (' + mapExtPct() + '%)';
  if (d.phase === 'done')     return 'Saved ' + _fmtBytes(d.bytesDone) + ' to the card.';
  if (d.phase === 'error')    return 'Failed: ' + (d.error || 'unknown error') + '.';
  if (d.phase === 'cancelled') return 'Cancelled.';
  return '';
}

// --- Live shares (grants we answer, feeds we poll) -----------------
// Refreshed when a conversation opens, after a send, and patched live
// by WS telemetry_update events. left_s counts down client-side from
// fetchedAt.
async function refreshTelemetryShares() {
  try {
    const r = await state.transport.getTelemetryShares();
    const now = Date.now();
    for (const f of (r.feeds || [])) {
      f.updatedAtMs = f.age_ms != null ? now - f.age_ms : 0;
    }
    state.telemetryShares = {
      grants: r.grants || [],
      feeds:  r.feeds  || [],
      fetchedAt: now,
    };
  } catch (e) { /* strip just stays hidden */ }
}

function _shareLeftS(entry) {
  const elapsed = state.ui.nowS - (state.telemetryShares.fetchedAt || 0) / 1000;
  return Math.max(0, (entry.left_s || 0) - elapsed);
}

// "updated now" / "updated 49s ago" for a live feed's latest readings.
function telemetryFeedUpdatedText() {
  const f = telemetryFeedForOpenPeer();
  if (!f || !f.updatedAtMs) return '';
  const sec = Math.max(0, state.ui.nowS - Math.floor(f.updatedAtMs / 1000));
  if (sec < 3)    return 'updated now';
  if (sec < 60)   return 'updated ' + sec + 's ago';
  if (sec < 3600) return 'updated ' + Math.round(sec / 60) + 'm ago';
  return 'updated ' + Math.round(sec / 3600) + 'h ago';
}

// Age of the readings attached to a message bubble, from the packed
// SID_TIME stamp (the sender's clock).
function telemetryBubbleAge(m) {
  const t = m.tele;
  if (!t || !t.time) return '';
  const sec = state.ui.nowS - t.time;
  if (sec < 0 || sec > 7 * 86400) return '';   // skewed peer clock - say nothing
  if (sec < 3)    return 'updated now';
  if (sec < 60)   return 'updated ' + Math.round(sec) + 's ago';
  if (sec < 3600) return 'updated ' + Math.round(sec / 60) + 'm ago';
  if (sec < 86400) return 'updated ' + Math.round(sec / 3600) + 'h ago';
  return 'updated ' + Math.round(sec / 86400) + 'd ago';
}

// 16-point compass rose for a bearing in degrees.
function cardinal(deg) {
  const pts = ['N','NNE','NE','ENE','E','ESE','SE','SSE',
               'S','SSW','SW','WSW','W','WNW','NW','NNW'];
  return pts[Math.round((((deg % 360) + 360) % 360) / 22.5) % 16];
}

function telemetryGrantForOpenPeer() {
  const g = (state.telemetryShares.grants || []).find(g => g.peer === state.openPeer);
  return (g && _shareLeftS(g) > 0) ? g : null;
}
function telemetryFeedForOpenPeer() {
  const f = (state.telemetryShares.feeds || []).find(f => f.peer === state.openPeer);
  return (f && _shareLeftS(f) > 0) ? f : null;
}

function _leftText(s) {
  if (s >= 3600) return Math.round(s / 3600) + ' h left';
  if (s >= 60)   return Math.round(s / 60) + ' min left';
  return Math.round(s) + ' s left';
}

// Strip pieces. Icons render as CSS classes; only the words are text.
function telemetryGrantIcons() {
  const g = telemetryGrantForOpenPeer();
  if (!g) return [];
  const masks = { location: 1, environment: 2, battery: 4, compass: 8 };
  return TELEMETRY_ITEMS.filter(i => g.items & masks[i.key]).map(i => i.ico);
}
function telemetryFeedLines() {
  const f = telemetryFeedForOpenPeer();
  return (f && f.tele) ? telemetryLines({ tele: f.tele }) : [];
}

async function stopTelemetryShare(role) {
  const peer = state.openPeer;
  if (!peer) return;
  try {
    const r = await state.transport.stopTelemetryShare(peer, role);
    state.telemetryShares = {
      grants: r.grants || [], feeds: r.feeds || [], fetchedAt: Date.now(),
    };
    toast(role === 'grant' ? 'Live updates stopped' : 'Stopped fetching updates', 'info');
  } catch (e) {
    toast('Could not stop: ' + (e.message || e), 'error');
  }
}

