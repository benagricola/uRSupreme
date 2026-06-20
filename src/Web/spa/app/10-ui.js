// ====================== TOP-BAR PROGRESS STRIP ======================
// Single thin progress bar at the bottom edge of #topbar that lights
// up while a long-running background task is in flight. Three kinds
// of input feed it:
//   - In-flight transfers (state.transfers Map, populated by
//     message_progress / message_complete WS events).
//   - bgTasks registry (this module) - anything calling startBgTask
//     / endBgTask, used by withBusy when opts.bgTaskLabel is set
//     and by long-running explicit operations (upload progress,
//     factory reset, etc.).
// The bar is determinate when every active source carries a
// {done, total} pair, indeterminate otherwise (e.g. a single
// "reboot pending" task without a meaningful percent).
// Plain object keyed by task id so Alpine reactivity tracks mutations.
// The topbar progress strip's data binding observes state.bgTasks.
function startBgTask(id, opts = {}) {
  state.bgTasks[id] = {
    label: opts.label || id,
    done:  (typeof opts.done  === 'number') ? opts.done  : 0,
    total: (typeof opts.total === 'number') ? opts.total : 0,
  };
}
function updateBgTask(id, opts = {}) {
  const t = state.bgTasks[id];
  if (!t) return;
  if (typeof opts.label === 'string') t.label = opts.label;
  if (typeof opts.done  === 'number') t.done  = opts.done;
  if (typeof opts.total === 'number') t.total = opts.total;
}
function endBgTask(id) {
  delete state.bgTasks[id];
}
// Render aggregate progress across transfers + bgTasks. If everything
// active has a known total, render a determinate fill. Otherwise
// indeterminate (the sliding 30% strip). When nothing is active,
// hide.
// Reactive data backing the topbar progress strip. Sums outbound
// transfers + bgTasks → determinate/indeterminate bar.
function topbarProgressData() {
  return {
    get _agg() {
      let total = 0, done = 0, any = false, anyUnknown = false;
      // Touch reactive collections so Alpine re-evaluates on mutation.
      const tfs = this.$store.s.transfers || {};
      for (const k of Object.keys(tfs)) {
        const t = tfs[k];
        // Aggregate BOTH directions - the strip is the mobile-thread fallback
        // (gated to the mobile thread view in CSS), where neither the inbound
        // nor outbound conversation-list row is on screen.
        any = true;
        if (typeof t.bytes_total === 'number' && t.bytes_total > 0) {
          total += t.bytes_total;
          done  += Math.min(t.bytes_done || 0, t.bytes_total);
        } else { anyUnknown = true; }
      }
      for (const t of Object.values(this.$store.s.bgTasks || {})) {
        any = true;
        if (t.total > 0) { total += t.total; done += Math.min(t.done, t.total); }
        else             { anyUnknown = true; }
      }
      return { total, done, any, anyUnknown };
    },
    get active()        { return this._agg.any; },
    get indeterminate() { return this._agg.any && (this._agg.anyUnknown || this._agg.total === 0); },
    get barPct() {
      const { total, done } = this._agg;
      if (total === 0) return 0;
      return Math.max(2, Math.min(100, Math.round((done / total) * 100)));
    },
  };
}
// Stub kept for callers (bgTasks add/end). Alpine reactivity drives
// the actual repaint via topbarProgressData.
function renderTopbarProgress() { /* no-op */ }

// ============================== TOAST ==============================
// Non-blocking notification. Replaces alert() throughout. Kinds:
//   'info' (default), 'success', 'warn', 'error'.
// Defaults: errors live ~7s (long enough to read), success ~3s, info ~4s.
// Legacy global `toast()` - kept as a thin shim that delegates to the
// Alpine store. ~50+ callsites across the SPA still call toast(msg, …);
// rewriting them in this commit would balloon the diff. Cleanup wave
// can rename them to $store.toasts.push directly.
function toast(message, kind = 'info', durationMs) {
  if (!window.Alpine) return;       // pre-init: silently drop. Toasts
                                    // matter for user feedback after
                                    // boot, not before.
  Alpine.store('toasts').push(message, kind, durationMs);
}

// ============================== CONFIRM MODAL ==============================
// In-app replacement for window.confirm / window.prompt - see the
// no-alerts-use-modals memory. Returns a Promise that resolves to:
//   - false                 when user cancels / closes the modal
//   - true                  when user confirms (no extra fields)
//   - { code }              when needsCode and user confirms
//   - { value }             when needsInput and user confirms
//   - { code, value }       both
//
// opts: {
//   title:        string,
//   body:         string,        // plain text, line breaks preserved
//   okLabel:      string,        // default "OK"
//   destructive:  bool,          // styles the OK button as danger-red
//   needsCode:    bool,          // shows an identity-code input
//   needsInput:   { label, value, placeholder } | true   // free-text input
// }
function showConfirm(opts) {
  return new Promise((resolve) => {
    const ic = (typeof opts.needsInput === 'object') ? opts.needsInput : {};
    Object.assign(state.confirm, {
      title:       opts.title || 'Confirm',
      body:        opts.body  || '',
      okLabel:     opts.okLabel || 'OK',
      destructive: !!opts.destructive,
      needsCode:   !!opts.needsCode,
      needsInput:  !!opts.needsInput,
      inputLabel:       ic.label       || '',
      inputValue:       ic.value       || '',
      inputPlaceholder: ic.placeholder || '',
      codeValue:        '',
      _resolve:         resolve,
    });
    state.modals.confirm = true;
    // Focus is driven by x-effect on #mc-input / #mc-code / #mc-ok:
    // each watches $store.s.modals.confirm + the relevant needsX flag
    // and calls $el.focus() in $nextTick when its branch wins.
  });
}
function confirmOk() {
  const c = state.confirm;
  if (!c || !c._resolve) return;
  const out = {};
  if (c.needsCode) {
    const code = (c.codeValue || '').trim();
    if (!code) {
      // Bump the refocus tick - #mc-code's x-effect picks it up as a
      // reactive dependency and re-runs the focus call. Lets us drive
      // an imperative-feeling "snap back to the code field" through
      // the same Alpine declaration as the initial open.
      state.confirm.refocusTick = (state.confirm.refocusTick || 0) + 1;
      return;
    }
    out.code = code;
  }
  if (c.needsInput) out.value = c.inputValue;
  const resolve = c._resolve;
  c._resolve = null;
  state.modals.confirm = false;
  if (c.needsCode || c.needsInput) resolve(out); else resolve(true);
}
function confirmCancel() {
  const c = state.confirm;
  if (!c || !c._resolve) return;
  const resolve = c._resolve;
  c._resolve = null;
  state.modals.confirm = false;
  resolve(false);
}

// ============================== IDENTICON ==============================
// FNV-1a 32-bit hash → 5×5 symmetric grid SVG, colour from HSL(hash mod 360).
// Cached per address so repeated renders are zero-cost.
const _idcCache = new Map();
function fnv1a(s) {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = (h * 0x01000193) >>> 0;
  }
  return h;
}
function identicon(addrHex, size = 40) {
  const key = (addrHex || '') + '@' + size;
  if (_idcCache.has(key)) return _idcCache.get(key);
  const h = fnv1a(addrHex || '');
  const hue = h % 360;
  const colour = 'hsl(' + hue + ', 55%, 55%)';
  const cells = [];
  // 5 wide × 5 tall, mirrored left/right → 15 bit decisions, we take from h.
  for (let row = 0; row < 5; row++) {
    for (let col = 0; col < 3; col++) {
      const bit = (h >> (row * 3 + col)) & 1;
      if (bit) {
        cells.push(col, row);
        if (col !== 2) cells.push(4 - col, row);
      }
    }
  }
  let rects = '';
  for (let i = 0; i < cells.length; i += 2) {
    rects += '<rect x="' + cells[i] + '" y="' + cells[i+1] + '" width="1" height="1" fill="' + colour + '"/>';
  }
  const svg =
    '<svg xmlns="http://www.w3.org/2000/svg" width="' + size + '" height="' + size + '" viewBox="0 0 5 5" shape-rendering="crispEdges">' +
      '<rect width="5" height="5" fill="#1c2128"/>' + rects +
    '</svg>';
  _idcCache.set(key, svg);
  return svg;
}
// ============================== EMOJI ==============================
const EMOJI = [
  '😀','😂','😍','🙂','😎','🤔','😢','😡',
  '👍','👎','👏','🙏','❤️','🔥','✅','❌',
  '📍','🎉','🤝','💬','📞','📷','💡','⚡',
];
// Picker open path. The grid itself is rendered declaratively in the
// markup via x-for over EMOJI; this function just stashes the
// per-open callback + anchors the popover at the click site.
let _emojiPickCallback = null;
function openEmojiPicker(anchorEl, onPick) {
  const pop = $('popup-emoji');
  _emojiPickCallback = onPick;
  const rect = anchorEl.getBoundingClientRect();
  pop.style.left = Math.max(8, rect.left) + 'px';
  pop.style.bottom = (window.innerHeight - rect.top + 6) + 'px';
  state.popovers.emoji = true;
  setTimeout(() => {
    const dismiss = (ev) => {
      if (!pop.contains(ev.target) && ev.target !== anchorEl) {
        state.popovers.emoji = false;
        document.removeEventListener('click', dismiss);
      }
    };
    document.addEventListener('click', dismiss);
  }, 0);
}
// @click handler for each emoji cell in the picker template.
function onEmojiPick(e) {
  if (_emojiPickCallback) _emojiPickCallback(e);
  state.popovers.emoji = false;
}

