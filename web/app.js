// Smart Bins web UI: a small single-page app without frameworks or external
// files, so it works offline on the local network and fits in the firmware.
//
// Structure:
//   - api():      fetch wrapper; every error becomes an exception with the
//                 server's {"error": "..."} message.
//   - i18n:       English strings are always loaded as the fallback, the
//                 chosen language is laid over them (missing keys show English).
//   - pages:      one <section> per tab, switched by the URL hash; each page
//                 has an optional enter() that loads its data.
//   - live:       a WebSocket that receives state pushes (lights, LED status,
//                 system status) and reconnects by itself.
'use strict';

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const state = {
  lang: 'en',
  fallback: {},   // English strings
  strings: {},    // strings of the chosen language
  system: null,   // last /api/system answer
  config: null,   // last /api/config answer
  locate: null,   // locate defaults: {color, mode, s}
  lights: [],
  litBins: new Set(),  // bins located from this browser, for the grid highlight
};

// ---------------------------------------------------------------------------
// Server access

async function api(path, opts = {}) {
  const init = { method: opts.method || 'GET', headers: {} };
  if (opts.form) {
    // Form encoding: the firmware reads these with the same parameter helpers
    // as query strings, so one handler serves both.
    init.method = opts.method || 'POST';
    init.body = new URLSearchParams(opts.form);
  } else if (opts.json !== undefined) {
    init.method = opts.method || 'POST';
    init.headers['Content-Type'] = 'application/json';
    init.body = typeof opts.json === 'string' ? opts.json : JSON.stringify(opts.json);
  }
  const res = await fetch(path, init);
  let body = null;
  try { body = await res.json(); } catch (e) { /* not JSON: keep null */ }
  if (!res.ok) throw new Error((body && body.error) || `${res.status} ${res.statusText}`);
  return body;
}

// Runs an API call and shows its error as a toast instead of throwing.
async function attempt(fn) {
  try { return await fn(); } catch (e) { toast(e.message, true); return null; }
}

let toastTimer = 0;
function toast(text, isError = false) {
  const el = $('#toast');
  el.textContent = text;
  el.classList.toggle('err', isError);
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, isError ? 5000 : 2500);
}

// ---------------------------------------------------------------------------
// Translations

function t(key, vars) {
  let s = state.strings[key] ?? state.fallback[key] ?? key;
  if (vars) for (const [k, v] of Object.entries(vars)) s = s.replaceAll(`{${k}}`, v);
  return s;
}

function applyTranslations(root = document) {
  for (const el of $$('[data-i18n]', root)) el.textContent = t(el.dataset.i18n);
  for (const el of $$('[data-i18n-ph]', root)) el.placeholder = t(el.dataset.i18nPh);
}

function storedLanguage() {
  try { return localStorage.getItem('lang'); } catch (e) { return null; }
}

async function setLanguage(lang) {
  state.strings = lang === 'en' ? state.fallback : await attempt(() => api(`/i18n/${lang}.json`)) || state.fallback;
  state.lang = lang;
  try { localStorage.setItem('lang', lang); } catch (e) { /* private mode: not remembered */ }
  document.documentElement.lang = lang;
  $('#language').value = lang;
  applyTranslations();
  // Text built by scripts (light types, info labels) is redrawn in the new language.
  renderLights(true);
  renderSystem();
  renderPasswordState();
}

// ---------------------------------------------------------------------------
// Pages

const pages = {
  inventory: {},
  lights: { enter: loadLights },
  grid: { enter: enterGrid },
  settings: { enter: enterSettings },
  system: { enter: loadSystem },
};

function showPage() {
  const name = location.hash.slice(1);
  const page = pages[name] ? name : 'inventory';
  for (const s of $$('.page')) s.classList.toggle('active', s.id === `page-${page}`);
  for (const a of $$('#tabs a')) a.classList.toggle('active', a.dataset.page === page);
  if (pages[page].enter) pages[page].enter();
}

// ---------------------------------------------------------------------------
// Work lights

const PRESETS = ['#ffffff', '#ff8a12', '#ff0000', '#ffb000', '#00ff40', '#00c8ff', '#0030ff', '#c000ff'];

async function loadLights() {
  const res = await attempt(() => api('/api/lights'));
  if (res) updateLights(res.lights);
}

function updateLights(lights) {
  state.lights = lights;
  renderLights(false);
}

// Builds one card per light. Cards are rebuilt only when the set of lights
// changes (a new configuration); live updates just refresh their values, so a
// slider being dragged is not reset under the user's finger.
function renderLights(force) {
  const list = $('#lights-list');
  const key = state.lights.map(l => `${l.id}:${l.type}`).join(',');
  if (force || list.dataset.key !== key) {
    list.dataset.key = key;
    list.replaceChildren(...state.lights.map(buildLightCard));
  }
  $('#lights-empty').hidden = state.lights.length > 0;
  state.lights.forEach((l, i) => fillLightCard(list.children[i], l));
}

function buildLightCard(light) {
  const card = $('#light-card').content.firstElementChild.cloneNode(true);
  applyTranslations(card);
  const id = light.id;
  const white = light.type === 'white';
  card.querySelector('.light-color').hidden = white;
  card.querySelector('[data-effect=rainbow]').hidden = white;

  card.querySelector('.light-on').addEventListener('change', e => sendLight(id, { on: e.target.checked ? 1 : 0 }));
  const level = card.querySelector('.light-level');
  level.addEventListener('input', () => {
    card.querySelector('.light-level-out').textContent = percent(level.value);
    sendLight(id, { level: level.value });
  });
  const hue = card.querySelector('.hue');
  hue.addEventListener('input', () => sendLight(id, { color: hueToHex(+hue.value) }));
  const kelvin = card.querySelector('.kelvin');
  kelvin.addEventListener('input', () => {
    card.querySelector('.light-kelvin-out').textContent = `${kelvin.value} K`;
    sendLight(id, { kelvin: kelvin.value });
  });
  const presets = card.querySelector('.presets');
  for (const c of PRESETS) {
    const b = document.createElement('button');
    b.style.background = c;
    b.title = c;
    b.addEventListener('click', () => sendLight(id, { color: c.slice(1) }));
    presets.append(b);
  }
  for (const b of $$('.effects button', card)) b.addEventListener('click', () => sendLight(id, { effect: b.dataset.effect }));
  return card;
}

function fillLightCard(card, l) {
  if (!card) return;
  card.querySelector('.light-name').textContent = l.name;
  card.querySelector('.light-type').textContent = t(`lights.type.${l.type}`);
  card.querySelector('.light-on').checked = l.on;
  setIfIdle(card.querySelector('.light-level'), l.level);
  card.querySelector('.light-level-out').textContent = percent(l.level);
  if (l.color) card.querySelector('.light-swatch').style.background = l.color;
  for (const b of $$('.effects button', card)) b.classList.toggle('on', b.dataset.effect === l.effect);
  const power = card.querySelector('.light-power');
  power.textContent = l.estimateMa !== undefined
    ? t(l.currentLimited ? 'lights.currentLimited' : 'lights.current', { ma: l.estimateMa }) : '';
}

// Leaves a control alone while the user is interacting with it.
function setIfIdle(input, value) {
  if (document.activeElement !== input) input.value = value;
}

function percent(level) { return `${Math.max(1, Math.round(level * 100 / 255))} %`; }

function hueToHex(h) {
  const f = n => {
    const k = (n + h / 60) % 6;
    return Math.round(255 * (1 - Math.max(0, Math.min(k, 4 - k, 1))));
  };
  return [f(5), f(3), f(1)].map(v => v.toString(16).padStart(2, '0')).join('');
}

// Sends light changes without flooding the board while a slider is dragged:
// while a request for a light is in flight, newer changes are merged and sent
// as one request when it returns. The rate thus follows the board's own speed.
const lightQueue = new Map();  // id -> {busy, pending}
function sendLight(id, params) {
  const q = lightQueue.get(id) || { busy: false, pending: null };
  lightQueue.set(id, q);
  q.pending = { ...(q.pending || {}), ...params };
  if (!q.busy) flushLight(id);
}

async function flushLight(id) {
  const q = lightQueue.get(id);
  const params = q.pending;
  q.pending = null;
  q.busy = true;
  const res = await attempt(() => api('/api/lights/set', { form: { id, ...params } }));
  q.busy = false;
  if (res) updateLights(res.lights);
  if (q.pending) flushLight(id);
}

// ---------------------------------------------------------------------------
// Grid

async function enterGrid() {
  if (!state.config) state.config = await attempt(() => api('/api/config'));
  if (!state.config) return;
  const g = state.config.grid;
  const view = $('#grid-view');
  if (view.dataset.size !== `${g.cols}x${g.rows}`) {
    view.dataset.size = `${g.cols}x${g.rows}`;
    view.style.gridTemplateColumns = `repeat(${g.cols}, minmax(30px, 1fr))`;
    const buttons = [];
    for (let y = 0; y < g.rows; y++) {
      for (let x = 0; x < g.cols; x++) {
        const b = document.createElement('button');
        b.textContent = binLabel(x, y);
        b.addEventListener('click', () => locate(x, y, b));
        buttons.push(b);
      }
    }
    view.replaceChildren(...buttons);
  }
  setIfIdle($('#grid-brightness'), g.brightness);
  $('#grid-brightness-out').textContent = percent($('#grid-brightness').value);
  const defaults = await attempt(() => api('/api/locate/defaults'));
  if (defaults) showLocateDefaults(defaults);
}

// Locate defaults: how a bin lights up when it is located from anywhere (this
// page, the inventory, the API). Each change is saved on the board at once.
function showLocateDefaults(d) {
  state.locate = d;
  for (const b of $$('#locate-mode button')) b.classList.toggle('on', b.dataset.mode === d.mode);
  const duration = $('#locate-duration');
  // A duration set in config.json that isn't in the list gets its own entry.
  if (![...duration.options].some(o => Number(o.value) === d.s)) {
    duration.add(new Option(`${d.s} s`, d.s));
  }
  duration.value = String(d.s);
  setIfIdle($('#locate-color'), `#${d.color.toLowerCase()}`);
}

async function saveLocateDefaults(params) {
  const d = await attempt(() => api('/api/locate/defaults', { form: params }));
  if (d) {
    showLocateDefaults(d);
    toast(t('grid.saved'));
  }
}

// Bins are named like spreadsheet cells: column letters, row number (A1 = top left).
function binLabel(x, y) {
  let col = '';
  for (let n = x + 1; n > 0; n = Math.floor((n - 1) / 26)) col = String.fromCharCode(65 + (n - 1) % 26) + col;
  return `${col}${y + 1}`;
}

async function locate(x, y, button) {
  if (await attempt(() => api('/api/locate', { form: { x, y } }))) {
    button.classList.add('lit');
    // The board turns the highlight off by itself after the configured time;
    // the button follows that time (or stays lit when it is 0 = until cleared).
    const seconds = state.locate ? state.locate.s : 30;
    if (seconds > 0) setTimeout(() => button.classList.remove('lit'), seconds * 1000);
  }
}

// ---------------------------------------------------------------------------
// Settings

async function enterSettings() {
  const cfg = await attempt(() => api('/api/config'));
  if (cfg) {
    state.config = cfg;
    $('#config-text').value = JSON.stringify(cfg, null, 2);
  }
  if (!state.system) await loadSystem();
  renderPasswordState();
}

function renderPasswordState() {
  if (!state.system) return;
  $('#password-state').textContent = t(state.system.passwordSet ? 'settings.passwordOn' : 'settings.passwordOff');
  $('#password-remove').disabled = !state.system.passwordSet;
}

async function savePassword(password) {
  if (await attempt(() => api('/api/security', { form: { password } }))) {
    toast(t(password ? 'settings.passwordSaved' : 'settings.passwordRemoved'));
    $('#password-new').value = $('#password-repeat').value = '';
    await loadSystem();
    renderPasswordState();
  }
}

// Saving the configuration restarts the board; the page waits for it to come
// back and then reloads, so the user sees the new settings take effect.
async function saveConfig() {
  if (await attempt(() => api('/api/config', { json: $('#config-text').value }))) waitForRestart();
}

async function waitForRestart() {
  toast(t('system.restarting'));
  await new Promise(r => setTimeout(r, 4000));
  for (let i = 0; i < 30; i++) {
    try {
      await api('/api/system');
      location.reload();
      return;
    } catch (e) {
      await new Promise(r => setTimeout(r, 1000));
    }
  }
  toast(t('system.notBack'), true);
}

// ---------------------------------------------------------------------------
// System

async function loadSystem() {
  const sys = await attempt(() => api('/api/system'));
  if (sys) {
    state.system = sys;
    $('#device-name').textContent = sys.device;
    document.title = `${sys.device} - Smart Bins`;
    renderSystem();
  }
  return sys;
}

function formatBytes(n) {
  if (n >= 1048576) return `${(n / 1048576).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${n} B`;
}

function formatUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d} d ${h} h` : h ? `${h} h ${m} min` : `${m} min ${s % 60} s`;
}

function renderSystem() {
  const s = state.system;
  if (!s) return;
  const rows = [
    ['system.firmware', s.firmware],
    ['system.device', s.device],
    ['system.chip', `${s.chip}, ${s.cpuMhz} MHz`],
    ['system.flash', formatBytes(s.flashBytes)],
    ['system.uptime', formatUptime(s.uptimeS)],
    ['system.heap', `${formatBytes(s.heapFree)} (${t('system.minimum')} ${formatBytes(s.heapMin)})`],
    ['system.storage', `${formatBytes(s.fsUsed)} / ${formatBytes(s.fsTotal)}`],
    ['system.network', s.wifi.ssid ? `${s.wifi.ssid}, ${s.wifi.rssi} dBm` : '-'],
    ['system.address', `${s.wifi.ip} (${s.wifi.host})`],
  ];
  $('#system-info').replaceChildren(...rows.flatMap(([k, v]) => {
    const dt = document.createElement('dt');
    dt.textContent = t(k);
    const dd = document.createElement('dd');
    dd.textContent = v;
    return [dt, dd];
  }));
  $('#status-wifi').textContent = s.wifi.ssid ? `${s.wifi.rssi} dBm` : '';
}

// ---------------------------------------------------------------------------
// Live updates

let socket = null;
let retryMs = 1000;

async function connectLive() {
  // With a password set, the socket needs a session token: browsers can't
  // answer a digest-auth challenge on a WebSocket, so /api/session (which is
  // protected like every other page) hands one out instead.
  const session = await attempt(() => api('/api/session'));
  const token = session && session.token ? `?token=${session.token}` : '';
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  socket = new WebSocket(`${proto}://${location.host}/ws${token}`);
  socket.onopen = () => {
    retryMs = 1000;
    $('#live-dot').classList.add('live');
    $('#live-dot').title = t('status.live');
  };
  socket.onclose = () => {
    $('#live-dot').classList.remove('live');
    $('#live-dot').title = t('status.offline');
    // Back off up to 15 s so a board that is restarting isn't hammered.
    setTimeout(connectLive, retryMs);
    retryMs = Math.min(retryMs * 2, 15000);
  };
  socket.onmessage = ev => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch (e) { return; }
    if (msg.type === 'lights') updateLights(msg.lights);
    else if (msg.type === 'system' && state.system) {
      Object.assign(state.system, msg.system);
      renderSystem();
    } else if (msg.type === 'leds') {
      $('#grid-status').textContent = t('grid.status', { ma: msg.estimateMa, fps: msg.fps })
        + (msg.currentLimited ? ` ${t('grid.limited')}` : '');
    }
  };
}

// ---------------------------------------------------------------------------
// Start-up

function bindControls() {
  $('#language').addEventListener('change', e => setLanguage(e.target.value));
  $('#lights-off').addEventListener('click', async () => {
    const res = await attempt(() => api('/api/lights/off', { method: 'POST' }));
    if (res) updateLights(res.lights);
  });
  $('#locate-clear').addEventListener('click', async () => {
    if (await attempt(() => api('/api/locate/clear', { method: 'POST' }))) {
      for (const b of $$('#grid-view button')) b.classList.remove('lit');
    }
  });
  for (const b of $$('#grid-tests button')) {
    b.addEventListener('click', () => attempt(() => api('/api/led/test', { form: { pattern: b.dataset.pattern } })));
  }
  for (const b of $$('#locate-mode button')) {
    b.addEventListener('click', () => saveLocateDefaults({ mode: b.dataset.mode }));
  }
  $('#locate-duration').addEventListener('change', e => saveLocateDefaults({ s: e.target.value }));
  $('#locate-color').addEventListener('change', e => saveLocateDefaults({ color: e.target.value }));
  const brightness = $('#grid-brightness');
  brightness.addEventListener('input', () => { $('#grid-brightness-out').textContent = percent(brightness.value); });
  brightness.addEventListener('change', () => attempt(() => api('/api/led/brightness', { form: { value: brightness.value } })));
  $('#password-form').addEventListener('submit', e => {
    e.preventDefault();
    const a = $('#password-new').value, b = $('#password-repeat').value;
    if (a.length < 4) return toast(t('settings.passwordShort'), true);
    if (a !== b) return toast(t('settings.passwordMismatch'), true);
    savePassword(a);
  });
  $('#password-remove').addEventListener('click', () => savePassword(''));
  $('#config-save').addEventListener('click', saveConfig);
  $('#restart').addEventListener('click', async () => {
    if (confirm(t('system.restartConfirm')) && await attempt(() => api('/api/system/restart', { method: 'POST' }))) {
      waitForRestart();
    }
  });
}

async function start() {
  bindControls();
  state.fallback = await attempt(() => api('/i18n/en.json')) || {};
  state.strings = state.fallback;
  await loadSystem();
  // Language: this browser's choice, else the device default from the config.
  const lang = storedLanguage() || (state.system && state.system.language) || 'en';
  await setLanguage(['en', 'es', 'ru'].includes(lang) ? lang : 'en');
  window.addEventListener('hashchange', showPage);
  showPage();
  connectLive();
}

start();
