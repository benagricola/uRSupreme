// ============================== IDENTITY CODE MODAL ==============================
// opts.codePresent: true if a code is already on the OLED (we got
// here from the identity_code_available WS event), false if the SPA
// initiated this prompt and the user may still need to press the
// device button. Drives the hint text + title so the message
// matches reality.
function openIdCodeModal(opts) {
  const codePresent = !!(opts && opts.codePresent);
  state.forms.idCode.input  = '';
  state.ui.idCodeModalTitle = codePresent
    ? 'Identity code available'
    : 'Identity code required';
  state.ui.idCodeModalHint  = codePresent
    ? 'A code is on the device OLED for ~' + idCodeTtlText() + '. Read the 6 hex chars and type them below.'
    : 'A 6-character code is needed to authorise this action. If one is not already on the device OLED, press the BOOT button (see below) to generate one, then type it below.';
  state.modals.idCode = true;
  // #idc-input has x-effect bound to $store.s.modals.idCode that calls
  // $el.focus() inside $nextTick, so flipping the modal flag above is
  // enough - no imperative focus needed here.
}
// Bound @click on #btn-idc-ok and @keydown.enter on #idc-input.
function onClickIdcOk() {
  const code = state.forms.idCode.input;
  state.modals.idCode = false;
  if (state.pendingIdCodeAction) { state.pendingIdCodeAction(code); state.pendingIdCodeAction = null; }
}
// Dismiss = cancel: the pending action still runs, with an empty code,
// so its no-code branch can roll back any optimistic UI (e.g. snap a
// toggle back off). Without this, dismissing left the requesting
// control looking applied while the server never saw the change.
function onClickIdcCancel() {
  state.modals.idCode = false;
  if (state.pendingIdCodeAction) { state.pendingIdCodeAction(''); state.pendingIdCodeAction = null; }
}

// ============================== WIFI PROVISIONED PANEL ==============================
// Called after /api/wifi/configure responds with sta_ip + hostname
// (the in-place APSTA transition completed). The AP is about to be
// deauthed by the device (~1 s after the response flushes), so this
// page will become unreachable shortly. Replace the body content
// with a terminal "Done!" screen carrying both URLs so the user can
// switch their WiFi back to their home network and reach the device.
function showWifiProvisionedPanel(resp) {
  const ip       = resp && resp.sta_ip   ? resp.sta_ip   : '';
  const hostname = resp && resp.hostname ? resp.hostname : '';
  const stUrl    = resp && resp.sta_url  ? resp.sta_url  : (ip       ? 'http://' + ip       + '/' : '');
  const mdUrl    = resp && resp.mdns_url ? resp.mdns_url : (hostname ? 'http://' + hostname + '.local/' : '');
  document.body.innerHTML =
    '<div class="page"><div class="card">' +
      '<h2>WiFi configured</h2>' +
      '<p class="muted">Your device is on your home network now. Switch your computer back to that network, then visit one of the URLs below.</p>' +
      (stUrl ? '<p><strong>Always works:</strong> <a id="urlIp">' + stUrl + '</a></p>' : '') +
      (mdUrl ? '<p><strong>Friendlier (where mDNS resolves):</strong> <a id="urlMdns">' + mdUrl + '</a></p>' : '') +
      '<p class="muted">This page will lose its softAP connection in a moment; that\'s expected. The device is fine.</p>' +
    '</div></div>';
  if (stUrl) { const a = document.getElementById('urlIp');   if (a) a.href = stUrl; }
  if (mdUrl) { const a = document.getElementById('urlMdns'); if (a) a.href = mdUrl; }
}

// ============================== REBOOT WAITER ==============================
function waitForDeviceAndReload(message) {
  // Replace the page contents with a status div and poll the device
  // for a successful /api/info. Two paths:
  //   1. Same origin (default) - the device kept the same IP, so we
  //      reload the current URL once /api/info responds.
  //   2. Origin switch - we were in softAP and just configured STA.
  //      The device now lives at a different IP, and the SPA's current
  //      origin (the softAP IP) won't resolve once the device drops
  //      its softAP. Use the mDNS hostname captured before reload and
  //      jump to http://<host>.local/ as soon as it answers.
  // Replace the body with a small reboot-in-progress card. innerHTML
  // is fine here because we're nuking the whole SPA state anyway -
  // a fresh page load is incoming.
  document.body.innerHTML =
    '<div class="page"><div class="card"><h2>Rebooting</h2>' +
    '<p class="muted" id="rb-status"></p></div></div>';
  // The replacement body has its own #rb-status element which is no
  // longer Alpine-bound (the SPA's root x-data is gone). Keep the
  // imperative textContent here because the rendering tree just
  // changed under us.
  $('rb-status').textContent = message;
  // Captured before the device reboots. Falsey when not yet known
  // (first /api/info hasn't returned by the time we navigate here);
  // we fall back to same-origin in that case.
  const mdns = (state.info && state.info.mdns_hostname) || null;
  const wasAp = (state.info && state.info.wifi_mode === 'ap');
  const candidates = [];
  // Always try same origin first - covers the case where the device
  // is on a fixed IP or the user is already on the LAN it'll come
  // back on. If we were in softAP and have a hostname, also try
  // http://<host>.local/ in parallel.
  candidates.push({ url: API.INFO, target: location.href });
  if (wasAp && mdns) {
    const lan = (location.protocol === 'https:' ? 'https' : 'http')
              + '://' + mdns + '.local';
    candidates.push({ url: lan + API.INFO, target: lan + '/' });
  }
  const startMs = Date.now();
  let attempts  = 0;
  let stopped   = false;
  const probeOne = async (c) => {
    if (stopped) return;
    try {
      const ctl = new AbortController();
      const tmo = setTimeout(() => ctl.abort(), 1500);
      // Cross-origin candidates (the .local URL) must probe with
      // mode:'no-cors': the firmware sends no Access-Control-Allow-Origin
      // header, so a 'cors' fetch rejects even when the device is up. A
      // no-cors fetch resolves with an opaque response whenever the host
      // answers at all, which is exactly the liveness signal we need;
      // status stays unreadable but a down host still rejects.
      const cross = c.url.startsWith('http');
      const r = await fetch(c.url, { cache: 'no-store', signal: ctl.signal,
                                     mode: cross ? 'no-cors' : 'same-origin' });
      clearTimeout(tmo);
      if (r.ok || r.type === 'opaque') {
        stopped = true;
        setTimeout(() => { location.href = c.target; }, 400);
      }
    } catch { /* still down */ }
  };
  const tick = async () => {
    if (stopped) return;
    attempts++;
    await Promise.all(candidates.map(probeOne));
    if (stopped) return;
    const elapsed = Math.floor((Date.now() - startMs) / 1000);
    const where = (wasAp && mdns)
      ? '(trying both this IP and http://' + mdns + '.local, '
      : '(';
    $('rb-status').textContent = 'Waiting for device to come back… ' + where + elapsed + 's)';
    if (attempts < 60) setTimeout(tick, 1500);
    else $('rb-status').textContent = "Device hasn't come back after 90 s. Check serial.";
  };
  setTimeout(tick, 2500);
}

// ============================== START ==============================
boot();

