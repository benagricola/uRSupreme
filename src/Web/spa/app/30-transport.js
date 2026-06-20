// ============================== TRANSPORT ABSTRACTION ==============================
// All network I/O the SPA does flows through a Transport instance. The HTTP
// implementation below talks to the uR device's /api/* endpoints; a future
// WasmReticulumRsTransport will implement the same surface against an
// in-browser reticulum-rs WASM build. Render and interaction code must not
// reference fetch / EventSource / /api/* directly.
class Transport {
  async connect()                                 { throw new Error('unimplemented'); }
  async disconnect()                              { throw new Error('unimplemented'); }
  async getInfo()                                 { throw new Error('unimplemented'); }
  async login(identityId, password)               { throw new Error('unimplemented'); }
  async logout()                                  { throw new Error('unimplemented'); }
  isAuthed()                                      { return false; }
  async createIdentity(displayName, pw, code)     { throw new Error('unimplemented'); }
  async deleteIdentity(identityId)                { throw new Error('unimplemented'); }
  async getIdentity(identityId)                   { throw new Error('unimplemented'); }
  async getState(identityId, markers)             { throw new Error('unimplemented'); }
  async send(identityId, destHex, title, content, attachments, telemetry) { throw new Error('unimplemented'); }
  async announceNow(identityId)                   { throw new Error('unimplemented'); }
  async lookupPath(destHex)                       { throw new Error('unimplemented'); }
  subscribe(identityId, markers, onEvent)         { throw new Error('unimplemented'); }
  async getRadio()                                { throw new Error('unimplemented'); }
  async setRadio(cfg)                             { throw new Error('unimplemented'); }
  async resetRadio(code)                          { throw new Error('unimplemented'); }
  async factoryReset(code)                        { throw new Error('unimplemented'); }
  async wifiScan()                                { throw new Error('unimplemented'); }
  async wifiConfigure(ssid, psk, identityCode)    { throw new Error('unimplemented'); }
  // Returns { blob, filename } for the named attachment, or throws on
  // 404 / auth errors. The browser-canonical inbox phase will replace
  // this with a download-once-and-cache flow.
  async fetchAttachment(identityId, filename)     { throw new Error('unimplemented'); }
  get capabilities() {
    return { wifiBootstrap: false, factoryReset: false, radioConfig: false, deviceManaged: false };
  }
}

class UReticulumHTTPTransport extends Transport {
  constructor() {
    super();
    this.endpoint = location.origin;
    this.tokenGetter = () => state.token;
  }
  get capabilities() {
    return { wifiBootstrap: true, factoryReset: true, radioConfig: true, deviceManaged: true };
  }
  async _req(path, opts = {}) {
    const headers = Object.assign({}, opts.headers || {});
    if (opts.body && typeof opts.body !== 'string') {
      headers['Content-Type'] = 'application/json';
      opts.body = JSON.stringify(opts.body);
    }
    const tok = this.tokenGetter();
    if (tok && !opts.unauthed) headers['Authorization'] = 'Bearer ' + tok;
    // Abort a request that stalls past `timeout` ms (default 30 s). The
    // SRAM-tight device can leave a request hanging; without this the socket
    // stays half-open and any awaiting caller never recovers. Slow-by-design
    // calls (large uploads) pass a larger opts.timeout; 0 disables it.
    const timeout = (opts.timeout === undefined) ? 30000 : opts.timeout;
    const fetchOpts = Object.assign({}, opts, { headers });
    delete fetchOpts.timeout;
    let timer = null;
    if (timeout > 0 && typeof AbortController !== 'undefined' && !fetchOpts.signal) {
      const ctrl = new AbortController();
      fetchOpts.signal = ctrl.signal;
      timer = setTimeout(() => ctrl.abort(new Error('request timed out')), timeout);
    }
    let r;
    try {
      r = await fetch(path, fetchOpts);
    } finally {
      if (timer) clearTimeout(timer);
    }
    const txt = await r.text();
    let body; try { body = txt ? JSON.parse(txt) : {}; } catch { body = { _raw: txt }; }
    if (!r.ok) {
      // Prefer the server's human-readable `message` over the
      // machine-readable `error` slug, so the UI shows something useful.
      const msg = body.message || body.error || ('http ' + r.status);
      const err = new Error(msg);
      err.status = r.status; err.body = body; err.error_code = body.error;
      throw err;
    }
    return body;
  }
  async connect() { /* HTTP has no persistent connection */ }
  async disconnect() { /* same */ }
  async getInfo() { return this._req(API.INFO, { unauthed: true }); }
  async login(identityId, password) {
    const r = await this._req(API.AUTH_LOGIN, { method: 'POST', body: { identity_id: identityId, password }, unauthed: true });
    return { token: r.token, identityId: r.identity_id || identityId, expiresIn: r.expires_in_s };
  }
  async logout() { return this._req(API.AUTH_LOGOUT, { method: 'POST' }); }
  isAuthed() { return !!this.tokenGetter(); }
  async createIdentity(displayName, password, code) {
    return this._req(API.IDENTITIES, { method: 'POST',
      body: { display_name: displayName, password, password_confirm: password, identity_code: code },
      unauthed: true });
  }
  // The identity is taken from the bearer token server-side, so these
  // session-scoped endpoints carry no {id} in the path. The identityId
  // arg is retained for call-site clarity (and the WS subscribe path,
  // which still passes identity_id) but no longer appears in the URL.
  async deleteIdentity(identityId) {
    return this._req(API.IDENTITY_DELETE, { method: 'POST' });
  }
  async getIdentity(identityId) {
    return this._req(API.IDENTITY);
  }
  async getState(identityId) {
    return this._req(API.STATE);
  }
  async send(identityId, destHex, title, content, attachments, telemetry) {
    // Wire format (post-#130): attachments reference pre-uploaded staging
    // buffers by id. Bytes must have been pushed to /outbound/upload first.
    const body = { to: destHex, title: title || '', content };
    if (attachments && attachments.length) body.attachments = attachments;
    // Telemetry attach: { location, environment, battery, compass,
    // share_s }. The device packs current readings and, when share_s
    // is set, records a live-share grant for this peer.
    if (telemetry) body.telemetry = telemetry;
    return this._req(API.SEND, { method: 'POST', body });
  }
  // Active live-telemetry shares (grants + feeds) for the session
  // identity, and stopping one direction of one.
  async getTelemetryShares() {
    return this._req(API.TELEMETRY_SHARES);
  }
  async stopTelemetryShare(peerHex, role) {
    return this._req(API.TELEMETRY_SHARES_STOP,
                     { method: 'POST', body: { peer: peerHex, role } });
  }
  // Upload bytes for a single attachment. Resolves to { staging_id, total_bytes, backend }.
  // Uses XHR (not fetch) so we can surface upload progress to the SPA tray.
  async uploadAttachment(identityId, bytes, filename, mime, onProgress, onBusy) {
    // The server allows one upload at a time (409 upload_busy for the
    // rest). Rather than surfacing that as an error, wait and retry so
    // a second send queues behind the first; onBusy lets the caller
    // show a waiting state. Gives up after the deadline and lets the
    // server's message reach the normal failure toast.
    const deadline = Date.now() + 90000;
    for (;;) {
      try {
        return await this._uploadAttachmentOnce(bytes, filename, mime, onProgress);
      } catch (e) {
        const busy = e && e.status === 409 && e.body && e.body.error === 'upload_busy';
        if (!busy || Date.now() >= deadline) throw e;
        if (onBusy) onBusy();
        await new Promise(r => setTimeout(r, 3000));
      }
    }
  }
  async _uploadAttachmentOnce(bytes, filename, mime, onProgress) {
    const total = bytes.length;
    const url = API.ATTACHMENT_UPLOAD;
    const tok = this.tokenGetter();
    const form = new FormData();
    form.append('file', new Blob([bytes], { type: mime || 'application/octet-stream' }), filename || 'blob.bin');
    return await new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', url);
      if (tok) xhr.setRequestHeader('Authorization', 'Bearer ' + tok);
      // Server allocates the staging buffer upfront from this header
      // (URL query params are wiped by the multipart parser before
      // the chunk handler fires).
      xhr.setRequestHeader('X-Total-Length', String(total));
      if (xhr.upload && onProgress) {
        xhr.upload.onprogress = (ev) => {
          if (ev.lengthComputable) onProgress(ev.loaded, ev.total);
        };
      }
      xhr.onload = () => {
        let body = {};
        try { body = xhr.responseText ? JSON.parse(xhr.responseText) : {}; } catch { /* opaque */ }
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(body);
        } else {
          const msg = body.message || body.error || ('http ' + xhr.status);
          const err = new Error(msg); err.status = xhr.status; err.body = body;
          reject(err);
        }
      };
      xhr.onerror = () => reject(new Error('upload network error'));
      xhr.send(form);
    });
  }
  async announceNow(identityId) {
    return this._req(API.ANNOUNCE, { method: 'POST' });
  }
  async retryOutbox(identityId, seq) {
    return this._req(API.OUTBOX_RETRY(String(seq)),
                     { method: 'POST' });
  }
  async lookupPath(destHex) {
    return this._req(API.PATHS_LOOKUP, { method: 'POST', body: { to: destHex } });
  }
  async clearConversation(identityId, peerHex) {
    return this._req(API.CONVERSATION(peerHex),
                     { method: 'DELETE' });
  }
  async getConversationConfig(identityId, peerHex) {
    return this._req(API.CONVERSATION_CONFIG(peerHex),
                     { method: 'GET' });
  }
  async setConversationConfig(identityId, peerHex, body) {
    return this._req(API.CONVERSATION_CONFIG(peerHex),
                     { method: 'POST', body });
  }
  subscribe(identityId, onEvent) {
    // Persistent WebSocket - one TCP connection that lives as long as
    // the SPA tab does, with backoff reconnect on close. Replaces the
    // EventSource short-poll, which churned a fresh TCP connection
    // every 500 ms and was the dominant source of PSRAM allocation
    // pressure on the device.
    let ws = null;
    let stopped = false;
    let reconnectAttempts = 0;
    let reconnectTimer = null;
    let pingTimer = null;
    let authProbeBusy = false;
    // After a few straight failures, find out WHY the socket keeps
    // dying: if the device answers /api/info but rejects our token,
    // this session was ended (another login evicted the single token,
    // or the device was wiped) - reconnecting can never succeed, so
    // drop back to the login screen instead of sitting behind the
    // "Reconnecting" bar forever.
    const probeDeadSession = async () => {
      if (authProbeBusy || stopped) return;
      authProbeBusy = true;
      try {
        const inf = await fetch(API.INFO);
        if (inf.ok) {
          const me = await fetch(API.IDENTITY,
            { headers: { 'Authorization': 'Bearer ' + (this.tokenGetter() || '') } });
          if (me.status === 401) {
            stopped = true;
            state.token = null;
            localStorage.removeItem(LS.TOKEN);
            location.reload();
            return;
          }
        }
      } catch (e) { /* device still down - the backoff keeps retrying */ }
      authProbeBusy = false;
    };
    const wsUrl = () => {
      const proto = (location.protocol === 'https:') ? 'wss:' : 'ws:';
      return proto + '//' + location.host
        + API.WS + '?token=' + encodeURIComponent(this.tokenGetter() || '')
        + '&identity_id=' + encodeURIComponent(identityId);
    };
    const onOpen = () => {
      reconnectAttempts = 0;
      if (state.reconnectStart) {
        state.reconnectStart = 0;
        if (window.Alpine) Alpine.store('net').connected = true;
      }
      // App-level keepalive (every 25 s) so middleboxes that idle
      // out silent WS connections still see traffic.
      if (pingTimer) clearInterval(pingTimer);
      pingTimer = setInterval(() => {
        if (ws && ws.readyState === WebSocket.OPEN) {
          ws.send('{"type":"ping"}');
        }
      }, 25000);
      // Expose a sender so the sensors popover can request live data.
      window.__wsSend = (obj) => {
        if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
      };
    };
    const onClose = () => {
      if (pingTimer) { clearInterval(pingTimer); pingTimer = null; }
      window.__wsSend = null;
      if (stopped) return;
      if (!state.reconnectStart) state.reconnectStart = Date.now();
      // Show "Reconnecting…" if disconnected > 5 s.
      setTimeout(() => {
        if (state.reconnectStart && Date.now() - state.reconnectStart > 5000) {
          if (window.Alpine) Alpine.store('net').connected = false;
        }
      }, 5200);
      // Exponential backoff capped at 30 s. Tell the app to resync
      // any state that may have drifted while disconnected.
      const delay = Math.min(30000, 500 * Math.pow(2, Math.min(reconnectAttempts, 6)));
      reconnectAttempts++;
      if (reconnectAttempts >= 3) probeDeadSession();
      if (onEvent) onEvent({ type: '_reconnect' });
      reconnectTimer = setTimeout(open, delay);
    };
    const open = () => {
      if (stopped) return;
      try { ws = new WebSocket(wsUrl()); }
      catch (e) { onClose(); return; }
      ws.onopen    = onOpen;
      ws.onmessage = (ev) => {
        try { onEvent(JSON.parse(ev.data)); } catch (e) { /* ignore */ }
      };
      ws.onclose   = onClose;
      ws.onerror   = () => { /* close fires after */ };
    };
    open();
    // A backgrounded tab freezes timers (Chrome throttles setTimeout to ~1/min
    // and fully suspends after a few minutes), so a socket dropped while hidden
    // leaves the backoff reconnect timer stalled - the "Reconnecting…" toast
    // then sits there until the throttled timer eventually fires (the ctrl-F5
    // symptom). When the tab becomes visible/focused or the network returns,
    // reconnect immediately: detach the stale socket so its late onclose can't
    // schedule a duplicate, reset the backoff, and open a fresh one now.
    const kick = () => {
      if (stopped || document.hidden) return;
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) return;
      if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
      if (ws) { ws.onclose = null; ws.onerror = null; try { ws.close(); } catch (e) { /* ignore */ } }
      reconnectAttempts = 0;
      open();
    };
    document.addEventListener('visibilitychange', kick);
    window.addEventListener('focus', kick);
    window.addEventListener('online', kick);
    return () => {
      stopped = true;
      document.removeEventListener('visibilitychange', kick);
      window.removeEventListener('focus', kick);
      window.removeEventListener('online', kick);
      if (reconnectTimer) clearTimeout(reconnectTimer);
      if (pingTimer)      clearInterval(pingTimer);
      if (ws) ws.close();
    };
  }
  async getRadio() { return this._req(API.RADIO); }
  async setRadio(cfg) { return this._req(API.RADIO, { method: 'POST', body: cfg }); }
  async setAirtime(cfg) { return this._req(API.RADIO_AIRTIME, { method: 'POST', body: cfg }); }
  async setTransportEnabled(enabled) {
    return this._req(API.SYSTEM_TRANSPORT, { method: 'POST', body: { enabled: !!enabled } });
  }
  async setIdentitySettings(identityId, settings) {
    return this._req(API.IDENTITY_SETTINGS,
      { method: 'POST', body: settings });
  }
  async resetRadio(code) { return this._req(API.RADIO_RESET, { method: 'POST', body: { identity_code: code } }); }
  async factoryReset(code) { return this._req(API.SYSTEM_FACTORY_RESET, { method: 'POST', body: { identity_code: code }, unauthed: true }); }
  async wifiScan() { return this._req(API.WIFI_SCAN); }
  async wifiConfigure(ssid, psk, identityCode) {
    // identity_code is required from the bootstrap softAP (no token yet); when
    // called from a logged-in session the bearer token also satisfies the
    // require_physical_auth check on the device.
    const body = { ssid, psk };
    if (identityCode) body.identity_code = identityCode;
    return this._req(API.WIFI_CONFIGURE, { method: 'POST', body, unauthed: !!identityCode });
  }
  async fetchAttachment(identityId, filename) {
    const tok = this.tokenGetter();
    const url = API.ATTACHMENT_DOWNLOAD(filename);
    const r = await fetch(url, { headers: tok ? { 'Authorization': 'Bearer ' + tok } : {} });
    if (!r.ok) {
      let body = {}; try { body = await r.json(); } catch { /* opaque */ }
      const msg = body.message || body.error || ('http ' + r.status);
      const err = new Error(msg); err.status = r.status; throw err;
    }
    return { blob: await r.blob(), filename };
  }
  async wifiSavedList() { return this._req(API.WIFI_SAVED); }
  async wifiForget(ssid, identityCode) {
    return this._req(API.WIFI_FORGET, { method: 'POST',
      body: { ssid: ssid || '', identity_code: identityCode || '' } });
  }
  async pathEstimate(toHex, bytes) {
    const q = '?to=' + encodeURIComponent(toHex) + '&bytes=' + (bytes | 0);
    return this._req(API.PATHS_ESTIMATE + q);
  }
}

