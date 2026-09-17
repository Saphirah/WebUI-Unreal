/* UnrealWebUI bridge. Independent implementation; no third-party runtime dependencies. */
(function (global) {
  'use strict';
  if (global.webui && global.webui.version === '0.2.0') return;
  const ue = global.ue = global.ue || {};
  const iface = ue.interface = ue.interface || Object.create(null);
  const pending = new Map();
  let sequence = 0;
  const native = () => global.ue && global.ue.webui;
  const validateName = name => {
    if (typeof name !== 'string' || !name.length || name.length > 256)
      throw new TypeError('Event name must contain 1-256 characters');
  };
  function encode(data) {
    const json = JSON.stringify(data === undefined ? null : data);
    if (json === undefined) throw new TypeError('Value is not JSON serializable');
    if (json.length > 1024 * 1024) throw new RangeError('JSON payload exceeds 1 Mi-character');
    return json;
  }
  function send(name, json, callback) {
    validateName(name);
    const bridge = native();
    if (!bridge || typeof bridge.broadcast !== 'function')
      return Promise.reject(new Error('Unreal bridge unavailable; await webui.ready first'));
    try { return Promise.resolve(bridge.broadcast(name, json, callback || '')); }
    catch (error) { return Promise.reject(error); }
  }
  function register(callback, seconds, reject) {
    if (typeof callback !== 'function') return '';
    const token = '__webui_cb_' + (++sequence) + '_' + Math.random().toString(36).slice(2);
    const delay = Number.isFinite(Number(seconds)) ? Math.max(0.001, Number(seconds)) : 1;
    const cleanup = () => {
      const entry = pending.get(token);
      if (entry) global.clearTimeout(entry.timer);
      pending.delete(token); delete iface[token];
    };
    const timer = global.setTimeout(() => {
      cleanup();
      if (reject) reject(new Error('Unreal request timed out'));
    }, delay * 1000);
    pending.set(token, {timer, cleanup, reject});
    iface[token] = value => { cleanup(); callback(value); };
    return token;
  }
  function broadcast(name, data, callback, timeout = 1) {
    validateName(name);
    if (typeof data === 'function') { timeout = callback === undefined ? 1 : callback; callback = data; data = null; }
    const json = encode(data);
    if (typeof callback === 'string') return send(name, json, callback);
    const token = register(callback, timeout);
    return send(name, json, token).catch(error => {
      if (token && pending.has(token)) pending.get(token).cleanup();
      throw error;
    });
  }
  const api = global.webui = {
    version: '0.2.0', broadcast,
    // Chromium performs the hit test, including clipping, transforms, stacking order,
    // inherited pointer-events and children that explicitly restore pointer-events:auto.
    // Ignore only the element actually hit, never its whole subtree.
    hitTest(x, y) {
      if (!Number.isFinite(x) || !Number.isFinite(y)) return false;
      const doc = global.document;
      if (!doc || x < 0 || y < 0 || x >= global.innerWidth || y >= global.innerHeight) return false;
      const target = doc.elementFromPoint(x, y);
      if (!target || target === doc.documentElement || target === doc.body) return false;
      return !target.matches('#root, #app, [data-webui-root]');
    },
    // Private native request/reply channel, independent of DOM pointer events. It keeps
    // working when Slate has removed the browser from its hit-test grid.
    _queryHitTest(request, u, v) {
      const bridge = native();
      if (!bridge || typeof bridge.hittestresult !== 'function') return;
      try {
        const hit = api.hitTest(u * global.innerWidth, v * global.innerHeight);
        Promise.resolve(bridge.hittestresult(request, hit)).catch(() => {});
      } catch (_) {
        // Unknown is not permission to send input through a control. Native times out
        // and retries, retaining browser input until a successful classification.
      }
    },
    request(name, data, timeout = 1) {
      return new Promise((resolve, reject) => {
        validateName(name);
        const json = encode(data);
        const token = register(resolve, timeout, reject);
        send(name, json, token).catch(error => {
          if (pending.has(token)) pending.get(token).cleanup();
          reject(error);
        });
      });
    }
  };
  // The raw interface accepts a JSON string; ue5 accepts an ordinary JS value.
  iface.broadcast = (name, json = 'null', callback = '') => {
    if (typeof json !== 'string') return Promise.reject(new TypeError('Raw broadcast expects JSON text'));
    try { JSON.parse(json || 'null'); } catch (error) { return Promise.reject(error); }
    return send(name, json || 'null', callback);
  };
  global.ue5 = broadcast;
  api.ready = new Promise((resolve, reject) => {
    let attempts = 0;
    const check = () => {
      if (native() && typeof native().broadcast === 'function') {
        resolve(api);
        if (typeof global.dispatchEvent === 'function' && typeof global.Event === 'function')
          global.dispatchEvent(new global.Event('webui-ready'));
      } else if (++attempts >= 200) reject(new Error('Unreal bridge was not available within 10 seconds'));
      else global.setTimeout(check, 50);
    };
    check();
  });
  // Avoid unhandled rejections on ordinary browser previews; consumers can still await ready.
  api.ready.catch(() => {});
  if (typeof global.addEventListener === 'function') global.addEventListener('pagehide', () => {
    for (const entry of [...pending.values()]) {
      entry.cleanup(); if (entry.reject) entry.reject(new Error('Page navigated away'));
    }
  });
})(globalThis);
