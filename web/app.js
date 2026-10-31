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
  litBins: [],    // bins the board reports as lit, [[x, y], ...]
  versions: { lights: -1, leds: -1 },  // newest state numbers shown, see isStale()
  binRequests: 0, // bin toggles sent and not answered yet
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
  for (const el of $$('#locate-presets button', root)) el.title = t(`lights.preset.${el.dataset.preset}`);
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
  renderDeviceName();
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

// Colour presets: white, warm white, then the primaries and their mixes around
// the colour wheel. A few are tuned for LEDs rather than taken literally: on a
// strip the green die is much brighter than red, so "yellow" needs less green
// (ffb000) and orange even less, or both look green-tinted. Warm white is sent
// as a colour temperature, which the board turns into the matching RGB mix.
const PRESETS = [
  { name: 'white', css: '#ffffff', params: { color: 'ffffff' } },
  { name: 'warm', css: '#ffc58f', params: { kelvin: 2700 } },
  { name: 'red', css: '#ff0000', params: { color: 'ff0000' } },
  { name: 'orange', css: '#ff8000', params: { color: 'ff4000' } },
  { name: 'yellow', css: '#ffd000', params: { color: 'ffb000' } },
  { name: 'green', css: '#00ff00', params: { color: '00ff00' } },
  { name: 'cyan', css: '#00ffff', params: { color: '00ffff' } },
  { name: 'azure', css: '#0080ff', params: { color: '0080ff' } },
  { name: 'blue', css: '#0000ff', params: { color: '0000ff' } },
  { name: 'purple', css: '#8000ff', params: { color: '8000ff' } },
  { name: 'magenta', css: '#ff00ff', params: { color: 'ff00ff' } },
  { name: 'pink', css: '#ff4080', params: { color: 'ff2060' } },
];

async function loadLights() {
  const res = await attempt(() => api('/api/lights'));
  if (res) updateLights(res);
}

// Stale updates. The board numbers every state it reports (`version`, one
// counter for the lights and one for the lit bins), and its answers to changes
// carry the number after the change. A WebSocket push can be built just before
// a change and arrive after the answer; shown as is, it would put the old
// value back for a moment (a slider jumping back, a bin going dark and lit
// again). So anything numbered below what the page already shows is dropped.
// The numbers restart with the board, so they are reset when the socket
// (re)connects.
function isStale(kind, version) {
  if (version === undefined) return false;
  if (version < state.versions[kind]) return true;
  state.versions[kind] = version;
  return false;
}

// `data`: {version, lights, stripPower} from an answer or a push.
// A stale list is not used, but the page is still refreshed from the newest
// one it has: a light whose change was on its way skipped that list's update
// (see lightBusy()) and is only now free to show it.
function updateLights(data) {
  if (!isStale('lights', data.version)) state.lights = data.lights;
  renderLights(false);
}

// A light with a change still on its way (being sent, or waiting to be sent
// after the one in flight) keeps what the user set: the board's state for it
// is about to change anyway, and showing the current one would flick back.
function lightBusy(id) {
  const q = lightQueue.get(id);
  return !!q && (q.busy || q.pending !== null);
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
  state.lights.forEach((l, i) => { if (!lightBusy(l.id)) fillLightCard(list.children[i], l); });
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
  // Kept on the card so fillLightCard() can move the hue thumb to the light's colour.
  card.hueSlider = makeSlider(card.querySelector('.hue'), 0, 359, 1, v => sendLight(id, { color: hueToHex(v) }));
  makeSlider(card.querySelector('.kelvin'), 1500, 10000, 100, v => {
    card.querySelector('.light-kelvin-out').textContent = `${v} K`;
    sendLight(id, { kelvin: v });
  });
  const presets = card.querySelector('.presets');
  for (const p of PRESETS) {
    const b = document.createElement('button');
    b.style.background = p.css;
    b.dataset.preset = p.name;
    b.addEventListener('click', () => sendLight(id, p.params));
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
  if (l.color) {
    card.querySelector('.light-swatch').style.background = l.color;
    // Only a clearly coloured light has a place on the hue track; for white or
    // pastel tones the thumb stays where it was.
    const hue = hexToHue(l.color);
    if (hue !== null) card.hueSlider.set(hue);
  }
  for (const b of $$('.presets button', card)) b.title = t(`lights.preset.${b.dataset.preset}`);
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

// Hue (0..359) of a "#rrggbb" colour, or null when it is too close to white or
// grey to have a meaningful hue (saturation below 50 %).
function hexToHue(hex) {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  if (max === 0 || d / max < 0.5) return null;
  let h = max === r ? (g - b) / d : max === g ? 2 + (b - r) / d : 4 + (r - g) / d;
  return Math.round((h * 60 + 360) % 360);
}

// A slider on a gradient track (hue and white tone), built by hand because
// restyled range inputs proved unreliable on phones: the drag was taken by the
// browser as a back-swipe or scroll and the thumb stuck. Here:
//   - CSS touch-action: none stops the browser from claiming the gesture;
//   - pointer capture keeps the drag going when the finger leaves the track;
//   - while dragging, updates from the board (set()) are ignored, so the thumb
//     never jumps back under the finger;
//   - arrow keys and Home/End work for keyboard users.
// onInput(value) is called for every change the user makes.
function makeSlider(el, min, max, step, onInput) {
  let value = min, dragging = false;
  el.setAttribute('aria-valuemin', min);
  el.setAttribute('aria-valuemax', max);
  const show = (v, notify) => {
    v = Math.min(max, Math.max(min, Math.round(v / step) * step));
    if (v === value && !notify) return;
    const changed = v !== value;
    value = v;
    el.style.setProperty('--pos', (v - min) / (max - min));
    el.setAttribute('aria-valuenow', v);
    if (notify && changed) onInput(v);
  };
  // The thumb's centre travels from 16 px inside the left edge to 16 px inside
  // the right one (1 px border + half of the 30 px thumb area), see app.css.
  const fromX = x => {
    const r = el.getBoundingClientRect();
    return min + (x - r.left - 16) / Math.max(1, r.width - 32) * (max - min);
  };
  el.addEventListener('pointerdown', e => {
    dragging = true;
    el.setPointerCapture(e.pointerId);
    show(fromX(e.clientX), true);
    e.preventDefault();  // no text selection or emulated mouse events
  });
  el.addEventListener('pointermove', e => { if (dragging) show(fromX(e.clientX), true); });
  const end = () => { dragging = false; };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
  el.addEventListener('keydown', e => {
    const big = (max - min) / 10;
    const delta = { ArrowLeft: -step, ArrowDown: -step, ArrowRight: step, ArrowUp: step, PageDown: -big, PageUp: big }[e.key];
    if (delta !== undefined) show(value + delta, true);
    else if (e.key === 'Home') show(min, true);
    else if (e.key === 'End') show(max, true);
    else return;
    e.preventDefault();
  });
  el.style.setProperty('--pos', 0);
  el.setAttribute('aria-valuenow', min);
  return { set: v => { if (!dragging) show(v, false); } };
}

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
  if (res) updateLights(res);
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
    // minmax(0, 1fr): columns share the width exactly and may shrink below
    // their text width, so the grid never overflows a phone screen.
    view.style.gridTemplateColumns = `repeat(${g.cols}, minmax(0, 1fr))`;
    const buttons = [];
    for (let y = 0; y < g.rows; y++) {
      for (let x = 0; x < g.cols; x++) {
        const b = document.createElement('button');
        b.textContent = binLabel(x, y);
        b.dataset.bin = `${x},${y}`;
        b.addEventListener('click', () => toggleBin(x, y, b));
        buttons.push(b);
      }
    }
    view.replaceChildren(...buttons);
    showLitBins();
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
  const color = `#${d.color.toLowerCase()}`;
  $('#locate-swatch').style.background = color;
  const hue = hexToHue(color);
  if (hue !== null) locateHue.set(hue);
}

// `quiet`: no "saved" message, for the colour slider, which saves as it moves.
async function saveLocateDefaults(params, quiet = false) {
  // Numbered so that only the answer to the newest save is shown: an older
  // answer (a slow one, or one overtaken while the colour slider moves on)
  // would put an earlier setting back on screen for a moment.
  const seq = ++locateSaves;
  const d = await attempt(() => api('/api/locate/defaults', { form: params }));
  if (seq !== locateSaves || locateHueTimer) return;
  if (d) {
    showLocateDefaults(d);
    if (!quiet) toast(t('grid.saved'));
  }
}

// The locate colour slider. Every save writes the board's flash, so a drag is
// not sent move by move: the swatch follows at once, and the colour is saved
// once the slider has rested for a moment.
let locateHue;
let locateHueTimer = 0;  // a colour waiting to be saved; 0 when none
let locateSaves = 0;     // saves started, see saveLocateDefaults()
function onLocateHue(hue) {
  const hex = hueToHex(hue);
  $('#locate-swatch').style.background = `#${hex}`;
  clearTimeout(locateHueTimer);
  locateHueTimer = setTimeout(() => {
    locateHueTimer = 0;
    saveLocateDefaults({ color: hex }, true);
  }, 400);
}

// Bins are named like spreadsheet cells: column letters, row number (A1 = top left).
function binLabel(x, y) {
  let col = '';
  for (let n = x + 1; n > 0; n = Math.floor((n - 1) / 26)) col = String.fromCharCode(65 + (n - 1) % 26) + col;
  return `${col}${y + 1}`;
}

// Tapping a dark bin lights it, tapping a lit one turns it off. The button
// changes at once; the board then confirms with the list of lit bins over the
// WebSocket (also when a bin times out or another page changes it).
//
// While toggles are unanswered the buttons show what the user tapped and the
// board's lit list is not applied: a list from before the last tap would
// undo it on screen. The answer to the last outstanding toggle brings the
// board's list (and its number, see isStale()), which then takes over.
async function toggleBin(x, y, button) {
  const lit = button.classList.toggle('lit');
  state.binRequests++;
  const res = await attempt(() => api(lit ? '/api/locate' : '/api/locate/clear', { form: { x, y } }));
  state.binRequests--;
  if (!res) button.classList.toggle('lit', !lit);
  else if (state.binRequests === 0) updateLitBins(res);
}

// `status`: a grid status ({version, lit, ...}) from an answer or a push.
function updateLitBins(status) {
  if (state.binRequests > 0 || isStale('leds', status.version)) return;
  state.litBins = status.lit || [];
  showLitBins();
}

// Marks the buttons of the bins the board reports as lit ([[x, y], ...]).
function showLitBins() {
  const lit = new Set(state.litBins.map(([x, y]) => `${x},${y}`));
  for (const b of $$('#grid-view button')) b.classList.toggle('lit', lit.has(b.dataset.bin));
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
  renderNetwork();
}

// IP address settings. With DHCP the fields show the address the board has
// now, so switching to "Fixed" starts from a working setup to adjust.
function renderNetwork() {
  const net = (state.config && state.config.network) || { mode: 'dhcp' };
  const wifi = (state.system && state.system.wifi) || {};
  const fixed = net.mode === 'static';
  $('#ip-mode').value = fixed ? 'static' : 'dhcp';
  $('#ip-form').hidden = !fixed;
  const src = fixed ? net : wifi;
  // 0.0.0.0 is how config.json writes "not set".
  const addr = v => (v && v !== '0.0.0.0' ? v : '');
  setIfIdle($('#ip-address'), addr(src.ip));
  setIfIdle($('#ip-gateway'), addr(src.gateway));
  setIfIdle($('#ip-subnet'), addr(src.subnet) || '255.255.255.0');
  // An empty DNS field means "use the gateway", the usual home router setup.
  setIfIdle($('#ip-dns'), fixed ? addr(net.dns) : '');
}

const IPV4 = /^(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}$/;

// Saves the IP settings into config.json (the board checks them again, e.g.
// that the gateway is in the same subnet) and restarts. When the page was
// opened by the old fixed address, it moves to the new one afterwards.
async function saveNetwork() {
  const fixed = $('#ip-mode').value === 'static';
  const network = { mode: fixed ? 'static' : 'dhcp' };
  if (fixed) {
    for (const key of ['address', 'gateway', 'subnet', 'dns']) {
      const v = $(`#ip-${key}`).value.trim();
      if (key === 'dns' && !v) continue;
      if (!IPV4.test(v)) return toast(t('settings.ipInvalid', { field: t(`settings.ip${key[0].toUpperCase()}${key.slice(1)}`) }), true);
      network[key === 'address' ? 'ip' : key] = v;
    }
  }
  if (!confirm(t('settings.ipConfirm'))) return;
  const cfg = await attempt(() => api('/api/config'));
  if (!cfg) return;
  cfg.network = network;
  if (!await attempt(() => api('/api/config', { json: JSON.stringify(cfg) }))) return;
  if (fixed && location.hostname === (state.system && state.system.wifi.ip) && network.ip !== location.hostname) {
    toast(t('system.restarting'));
    setTimeout(() => { location.href = `${location.protocol}//${network.ip}/#settings`; }, 8000);
  } else {
    waitForRestart();
  }
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

const DEFAULT_DEVICE_NAME = 'smartbins';

async function loadSystem() {
  const sys = await attempt(() => api('/api/system'));
  if (sys) {
    state.system = sys;
    // The product name is always shown; the device (network) name only when it
    // was changed from the default, to tell several boards apart.
    const custom = sys.device !== DEFAULT_DEVICE_NAME;
    $('#device-name').textContent = custom ? sys.device : '';
    document.title = custom ? `${sys.device} - Smart Bins` : 'Smart Bins';
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
    // Inventory figures; missing when the database couldn't be opened.
    ...(s.inventory && s.inventory.parts !== undefined ? [
      ['system.parts', t('system.partsValue', { parts: s.inventory.parts, pieces: s.inventory.quantity })],
      ['system.photos', String(s.inventory.photos)],
      ['system.bins', t('system.binsValue', { used: s.inventory.binsUsed, total: s.inventory.binsTotal })],
      ['system.categories', String(s.inventory.categories)],
      ['system.manufacturers', String(s.inventory.manufacturers)],
      ['system.database', formatBytes(s.inventory.dbBytes)],
    ] : []),
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
  renderWifi(s.wifi);
  renderDeviceName();
}

// Signal bars in the top bar: four at -55 dBm or better, one below -75 dBm.
function renderWifi(wifi) {
  const el = $('#status-wifi');
  el.hidden = !wifi.ssid;
  if (!wifi.ssid) return;
  const bars = wifi.rssi >= -55 ? 4 : wifi.rssi >= -65 ? 3 : wifi.rssi >= -75 ? 2 : 1;
  [...el.children].forEach((bar, i) => bar.classList.toggle('on', i < bars));
  el.title = t('status.wifi', { ssid: wifi.ssid, rssi: wifi.rssi });
}

function renderDeviceName() {
  const s = state.system;
  if (!s) return;
  setIfIdle($('#device-name-input'), s.device);
  $('#device-name-hint').textContent = t('settings.deviceNameHint', { name: s.device });
}

// The device name is also the network name (NAME.local), so it follows host
// name rules, and a new name needs a restart. After that the old address no
// longer answers, so the page moves to the new one when it was opened by name.
async function saveDeviceName(name) {
  const cfg = await attempt(() => api('/api/config'));
  if (!cfg) return;
  cfg.device.name = name;
  if (!await attempt(() => api('/api/config', { json: JSON.stringify(cfg) }))) return;
  if (location.hostname.endsWith('.local')) {
    toast(t('system.restarting'));
    setTimeout(() => { location.href = `${location.protocol}//${name}.local/#settings`; }, 8000);
  } else {
    waitForRestart();
  }
}

// Theme: "auto" follows the system; light and dark override it. The choice is
// kept per browser, like the language, and applied by a script in the page
// head on the next load so the page never flashes the wrong theme.
function storedTheme() {
  try { return localStorage.getItem('theme') || 'auto'; } catch (e) { return 'auto'; }
}

function setTheme(theme) {
  try { localStorage.setItem('theme', theme); } catch (e) { /* not remembered */ }
  if (theme === 'auto') delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = theme;
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
    // A new connection may be to a restarted board, whose numbers start over.
    state.versions = { lights: -1, leds: -1 };
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
    if (msg.type === 'lights') updateLights(msg);
    else if (msg.type === 'system' && state.system) {
      Object.assign(state.system, msg.system);
      renderSystem();
    } else if (msg.type === 'leds') {
      updateLitBins(msg);
      $('#grid-status').textContent = t('grid.status', { ma: msg.estimateMa, fps: msg.fps })
        + (msg.currentLimited ? ` ${t('grid.limited')}` : '');
    }
  };
}

// ---------------------------------------------------------------------------
// Start-up

function bindControls() {
  $('#language').addEventListener('change', e => setLanguage(e.target.value));
  $('#theme').value = storedTheme();
  $('#theme').addEventListener('change', e => setTheme(e.target.value));
  $('#name-form').addEventListener('submit', e => {
    e.preventDefault();
    const name = $('#device-name-input').value.trim().toLowerCase();
    if (!/^[a-z0-9]([a-z0-9-]{0,30}[a-z0-9])?$/.test(name)) return toast(t('settings.deviceNameInvalid'), true);
    if (state.system && name === state.system.device) return;
    if (confirm(t('settings.deviceNameConfirm', { name }))) saveDeviceName(name);
  });
  $('#lights-off').addEventListener('click', async () => {
    const res = await attempt(() => api('/api/lights/off', { method: 'POST' }));
    if (res) updateLights(res);
  });
  $('#locate-clear').addEventListener('click', async () => {
    const res = await attempt(() => api('/api/locate/clear', { method: 'POST' }));
    if (res) updateLitBins(res);
  });
  // Locate colour: the same hue slider and colour presets as the lights
  // (presets that set a colour temperature don't apply to a bin and are left out).
  locateHue = makeSlider($('#locate-hue'), 0, 359, 1, onLocateHue);
  for (const p of PRESETS.filter(p => p.params.color)) {
    const b = document.createElement('button');
    b.style.background = p.css;
    b.dataset.preset = p.name;
    b.addEventListener('click', () => saveLocateDefaults({ color: p.params.color }));
    $('#locate-presets').append(b);
  }
  $('#ip-mode').addEventListener('change', e => { $('#ip-form').hidden = e.target.value !== 'static'; });
  $('#ip-save').addEventListener('click', saveNetwork);
  for (const b of $$('#grid-tests button')) {
    b.addEventListener('click', () => attempt(() => api('/api/led/test', { form: { pattern: b.dataset.pattern } })));
  }
  for (const b of $$('#locate-mode button')) {
    b.addEventListener('click', () => saveLocateDefaults({ mode: b.dataset.mode }));
  }
  $('#locate-duration').addEventListener('change', e => saveLocateDefaults({ s: e.target.value }));
  const brightness = $('#grid-brightness');
  brightness.addEventListener('input', () => { $('#grid-brightness-out').textContent = percent(brightness.value); });
  brightness.addEventListener('change', async () => {
    // Saved on the board; the cached config follows so the slider doesn't
    // jump back to the old value when the page is opened again.
    if (await attempt(() => api('/api/led/brightness', { form: { value: brightness.value } })) && state.config) {
      state.config.grid.brightness = Number(brightness.value);
    }
  });
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
