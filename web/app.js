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
  pins: null,     // last /api/pins answer (Hardware settings)
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
  } else if (opts.body !== undefined) {
    // Raw bytes, e.g. a photo as image/jpeg; the board stores them as they arrive.
    init.method = opts.method || 'POST';
    init.headers['Content-Type'] = opts.type;
    init.body = opts.body;
  } else if (opts.json !== undefined) {
    init.method = opts.method || 'POST';
    init.headers['Content-Type'] = 'application/json';
    init.body = typeof opts.json === 'string' ? opts.json : JSON.stringify(opts.json);
  }
  // no-store: an answer must never come from the browser's cache (after a
  // restore, /api/system has new counts under the same URL).
  init.cache = 'no-store';
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

// A string with {n} may have a singular form under "<key>.one" ("1 part"
// rather than "1 parts"). Russian phrases its strings so no form is needed.
function t(key, vars) {
  const one = vars && Number(vars.n) === 1 ? state.strings[`${key}.one`] : undefined;
  let s = one ?? state.strings[key] ?? state.fallback[key] ?? key;
  if (vars) for (const [k, v] of Object.entries(vars)) s = s.replaceAll(`{${k}}`, v);
  return s;
}

function applyTranslations(root = document) {
  for (const el of $$('[data-i18n]', root)) el.textContent = t(el.dataset.i18n);
  for (const el of $$('[data-i18n-ph]', root)) el.placeholder = t(el.dataset.i18nPh);
  for (const el of $$('[data-i18n-title]', root)) el.title = t(el.dataset.i18nTitle);
  for (const el of $$('[data-i18n-aria]', root)) el.setAttribute('aria-label', t(el.dataset.i18nAria));
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
  renderBackup();
  renderPasswordState();
  renderDeviceName();
  if ($('#page-settings').classList.contains('active')) renderHardware();
  if ($('#page-inventory').classList.contains('active')) {
    loadFilterLists();
    renderInventory();
  }
}

// ---------------------------------------------------------------------------
// Pages

const pages = {
  inventory: { enter: enterInventory },
  lights: { enter: loadLights },
  grid: { enter: enterGrid },
  settings: { enter: enterSettings },
  system: { enter: enterSystem },
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

// Color presets: white, warm white, then the primaries and their mixes around
// the color wheel. A few are tuned for LEDs rather than taken literally: on a
// strip the green die is much brighter than red, so "yellow" needs less green
// (ffb000) and orange even less, or both look green-tinted. Warm white is sent
// as a color temperature, which the board turns into the matching RGB mix.
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
  renderMotion(data.motion);
}

// The motion sensor's line on the Lights page: whether someone counts as at
// the bench, and the sensor's output right now (handy while aiming it).
function renderMotion(m) {
  const line = $('#lights-motion');
  line.hidden = !m;
  // Nobody there: the sensor's master switch has everything dark, whatever
  // the cards and the effect chips say.
  state.motionDark = !!m && !m.present;
  $('#lights-dark').hidden = !state.motionDark;
  renderEffectsDark();
  if (!m) return;
  line.textContent = t('lights.motion', {
    state: t(m.present ? 'lights.motionPresent' : 'lights.motionAbsent'),
    signal: t(m.signal ? 'lights.motionSignalOn' : 'lights.motionSignalOff'),
  });
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
  guardRange(level);
  level.addEventListener('input', () => {
    card.querySelector('.light-level-out').textContent = percent(level.value);
    sendLight(id, { level: level.value });
  });
  // Kept on the card so fillLightCard() can move the hue thumb to the light's color.
  card.hueSlider = makeSlider(card.querySelector('.hue'), 0, 359, 1, v => sendLight(id, { color: hueToHex(v) }));
  card.kelvinSlider = makeSlider(card.querySelector('.kelvin'), 1500, 10000, 100, v => {
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
    // Only a clearly colored light has a place on the hue track; for white or
    // pastel tones the thumb stays where it was.
    const hue = hexToHue(l.color);
    if (hue !== null) card.hueSlider.set(hue);
    // A white tone set here or on another page: the board keeps only the
    // color, so the tone is found back from it (see colorToKelvin()).
    const kelvin = colorToKelvin(l.color);
    if (kelvin !== null) {
      card.kelvinSlider.set(kelvin);
      card.querySelector('.light-kelvin-out').textContent = `${kelvin} K`;
    }
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

// The same black-body curve as kelvinToRgb() in the firmware
// (lib/Led/src/WorkEffects.h), as [r, g, b].
function kelvinToRgb(kelvin) {
  const t = kelvin / 100;
  const c = v => Math.round(Math.min(255, Math.max(0, v)));
  const r = t <= 66 ? 255 : 329.698727446 * Math.pow(t - 60, -0.1332047592);
  const g = t <= 66 ? 99.4708025861 * Math.log(t) - 161.1195681661 : 288.1221695283 * Math.pow(t - 60, -0.0755148492);
  const b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * Math.log(t - 10) - 305.0447927307;
  return [c(r), c(g), c(b)];
}

// The white tone (1500..10000 K, in the slider's 100 K steps) a "#rrggbb"
// color was made from, or null when it isn't one: the board stores a white
// tone as its color, so another page can only tell the tone from the color.
// The closest step wins (above 7000 K neighboring steps differ by a single
// unit), and only a near-exact match counts, so a color picked on the hue
// slider or a preset never moves the white-tone thumb.
function colorToKelvin(hex) {
  const rgb = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
  let best = null, bestErr = 3;
  for (let k = 1500; k <= 10000; k += 100) {
    const c = kelvinToRgb(k);
    const err = c.reduce((sum, v, i) => sum + Math.abs(v - rgb[i]), 0);
    if (err < bestErr) { best = k; bestErr = err; }
  }
  return best;
}

// Hue (0..359) of a "#rrggbb" color, or null when it is too close to white or
// grey to have a meaningful hue (saturation below 50 %).
function hexToHue(hex) {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
  if (max === 0 || d / max < 0.5) return null;
  let h = max === r ? (g - b) / d : max === g ? 2 + (b - r) / d : 4 + (r - g) / d;
  return Math.round((h * 60 + 360) % 360);
}

// A slider on a gradient track (hue and white tone), built by hand because
// restyled range inputs are unreliable on phones: the drag is taken by the
// browser as a back-swipe or scroll and the thumb sticks. Here:
//   - CSS touch-action: pan-y leaves vertical moves to the browser (the page
//     scrolls as usual) and keeps sideways ones for the slider, so a sideways
//     drag is never taken as a back-swipe;
//   - a finger that lands on the slider doesn't change anything yet: only a
//     move that is clearly sideways starts a drag, and a tap (a touch that
//     ends without moving) sets the value where it landed. A swipe up or down
//     that starts on the slider just scrolls the page and leaves the value
//     alone (the browser cancels the pointer when it starts scrolling). The
//     mouse still acts on press, as on a desktop;
//   - pointer capture keeps the drag going when the finger leaves the track;
//   - while a finger is down, updates from the board (set()) are ignored, so
//     the thumb never jumps back under the finger;
//   - arrow keys and Home/End work for keyboard users.
// onInput(value) is called for every change the user makes.
// Movement (px) that tells a sideways drag from a tap or the start of a scroll.
const SLIDER_SLOP = 8;

function makeSlider(el, min, max, step, onInput) {
  let value = min, dragging = false;
  // A touch that hasn't been told apart yet: where it started.
  let pending = null;
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
  const startDrag = e => {
    pending = null;
    dragging = true;
    el.setPointerCapture(e.pointerId);
    show(fromX(e.clientX), true);
  };
  el.addEventListener('pointerdown', e => {
    if (e.pointerType === 'mouse') {
      startDrag(e);
      e.preventDefault();  // no text selection
      return;
    }
    pending = { id: e.pointerId, x: e.clientX, y: e.clientY };
  });
  el.addEventListener('pointermove', e => {
    if (dragging) return show(fromX(e.clientX), true);
    if (!pending || e.pointerId !== pending.id) return;
    const dx = Math.abs(e.clientX - pending.x), dy = Math.abs(e.clientY - pending.y);
    if (dx > SLIDER_SLOP && dx > dy) startDrag(e);
    else if (dy > SLIDER_SLOP) pending = null;  // a scroll: the browser has it
  });
  el.addEventListener('pointerup', e => {
    // A tap: the finger came up close to where it landed.
    if (pending && e.pointerId === pending.id) show(fromX(pending.x), true);
    pending = null;
    dragging = false;
  });
  el.addEventListener('pointercancel', () => { pending = null; dragging = false; });
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
  return { set: v => { if (!dragging && !pending) show(v, false); } };
}

// The same touch handling for a plain range input (brightness, speed and so
// on): a phone's browser moves a range's thumb the moment a finger lands on
// it, so a swipe up or down that happens to start on one changed the value
// before the page scrolled. guardRange() wraps the input in a span that
// takes the pointer instead (the input itself gets pointer-events: none, see
// app.css) and acts as makeSlider() does:
//   - a finger that lands changes nothing yet; a clearly sideways move
//     starts a drag, a tap sets the value where it landed, and a swipe up or
//     down scrolls the page and leaves the value alone (touch-action: pan-y
//     on the span hands vertical moves to the browser);
//   - the mouse still acts on press;
//   - the input keeps its value and its usual 'input' (while it moves) and
//     'change' (when let go) events, so the code around it is unchanged, and
//     it keeps its keyboard handling;
//   - it takes the focus while it is moved, as a native range does, so
//     setIfIdle() leaves it alone under the finger.
// The thumb's centre is taken to run from RANGE_THUMB / 2 inside one end of
// the input to as far inside the other: about where browsers draw it.
const RANGE_THUMB = 16;

function guardRange(input) {
  if (input.parentElement.classList.contains('range-wrap')) return;  // already done
  const wrap = document.createElement('span');
  wrap.className = 'range-wrap';
  input.replaceWith(wrap);
  wrap.append(input);
  const min = Number(input.min || 0), max = Number(input.max || 100);
  let dragging = false, pending = null, startValue = null;
  const fire = type => input.dispatchEvent(new Event(type, { bubbles: true }));
  const setFromX = x => {
    const r = input.getBoundingClientRect();
    const v = min + (x - r.left - RANGE_THUMB / 2) / Math.max(1, r.width - RANGE_THUMB) * (max - min);
    const before = input.value;
    input.value = v;  // the input rounds it to its step and clamps it
    if (input.value !== before) fire('input');
  };
  const begin = () => {
    startValue = input.value;
    input.focus({ preventScroll: true });
  };
  const end = () => {
    if (startValue !== null && input.value !== startValue) fire('change');
    startValue = null;
  };
  const startDrag = e => {
    pending = null;
    dragging = true;
    wrap.setPointerCapture(e.pointerId);
    begin();
    setFromX(e.clientX);
  };
  wrap.addEventListener('pointerdown', e => {
    if (input.disabled) return;
    if (e.pointerType === 'mouse') {
      startDrag(e);
      e.preventDefault();  // no text selection, and the focus stays on the input
      return;
    }
    pending = { id: e.pointerId, x: e.clientX, y: e.clientY };
  });
  wrap.addEventListener('pointermove', e => {
    if (dragging) return setFromX(e.clientX);
    if (!pending || e.pointerId !== pending.id) return;
    const dx = Math.abs(e.clientX - pending.x), dy = Math.abs(e.clientY - pending.y);
    if (dx > SLIDER_SLOP && dx > dy) startDrag(e);
    else if (dy > SLIDER_SLOP) pending = null;  // a scroll: the browser has it
  });
  wrap.addEventListener('pointerup', e => {
    // A tap: the finger came up close to where it landed.
    if (pending && e.pointerId === pending.id) {
      begin();
      setFromX(pending.x);
    }
    pending = null;
    dragging = false;
    end();
  });
  wrap.addEventListener('pointercancel', () => {
    pending = null;
    dragging = false;
    end();
  });
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
// Inventory
//
// The parts list is the board's /api/parts: search words, filters, sort and
// pages are all handled there (see README), so the page only keeps what it
// shows. Results come 50 at a time; "Show more" appends the next page.
//
// Three views of the same results: cards (phones), a table (wide screens) and
// the bin grid, which shows what is stored where and filters by bin on a tap.

const PAGE_SIZE = 50;
const SEARCH_DELAY_MS = 250;  // typing pause before a search is sent

// Sort keys whose natural first look is "biggest / newest first".
const DESC_FIRST = new Set(['quantity', 'updated']);

const inv = {
  q: '',
  category: '',
  manufacturer: '',
  bin: null,          // {x, y} when the list is filtered to one bin
  sort: 'partNumber',
  order: 'asc',
  view: 'cards',      // cards | table | bins
  parts: [],          // parts shown, in order
  total: 0,           // matches on the board, across all pages
  seq: 0,             // searches started; only the newest one's answer is shown
  searching: false,   // a search is on its way to the board (see search())
  searchAgain: false, // and another was asked for meanwhile
  lastKey: '',        // searchKey() of the last search sent (see searchKey())
  // What the two kinds of view have loaded, as listKey() and binsKey() of the
  // search they answer ('' for nothing). Switching between the views shows
  // what is already here instead of asking the board again (see showView()).
  listKey: '',
  binsKey: '',
  bins: [],           // /api/bins: {x, y, label, parts, outside}
  binHits: null,      // bin grid while searching: "x,y" -> matching parts (see searchBins)
  located: '',        // the result set auto-locate last lit, so it lights once
  searchTimer: 0,
  valuesAt: 0,        // when the autocomplete lists were last loaded (ms)
};

// Small per-browser preferences (view, sort, auto-locate), like the language.
function pref(key, fallback) {
  try { return localStorage.getItem(`inv.${key}`) ?? fallback; } catch (e) { return fallback; }
}
function setPref(key, value) {
  try { localStorage.setItem(`inv.${key}`, value); } catch (e) { /* not remembered */ }
}

async function enterInventory() {
  if (!state.config) state.config = await attempt(() => api('/api/config'));
  forgetResults();  // the parts may have changed while the page was away
  await Promise.all([loadBins(), loadFilterLists()]);
  search();
}

// Bins with their labels and part counts: for bin names everywhere, the bin
// picker in the form and the bin grid view.
async function loadBins() {
  const res = await attempt(() => api('/api/bins'));
  if (res) inv.bins = res.bins;
}

function findBin(x, y) {
  return inv.bins.find(b => b.x === x && b.y === y);
}

// A bin's name: its label when it has one, else the A1-style position.
function binName(x, y) {
  const b = findBin(x, y);
  return b && b.label ? b.label : binLabel(x, y);
}

// The category and manufacturer filters list the values in use, most used first.
async function loadFilterLists() {
  const [cats, makers] = await Promise.all([
    attempt(() => api('/api/parts/values?field=category&limit=200')),
    attempt(() => api('/api/parts/values?field=manufacturer&limit=200')),
  ]);
  fillFilter($('#inv-category'), cats, 'inv.allCategories', inv.category);
  fillFilter($('#inv-maker'), makers, 'inv.allMakers', inv.manufacturer);
}

function fillFilter(select, res, allKey, current) {
  const values = res ? res.values : [];
  const options = [new Option(t(allKey), '')];
  for (const v of values) options.push(new Option(`${v.value} (${v.parts})`, v.value));
  // A filter whose value no longer exists (its last part was changed) stays
  // selectable, so the list doesn't silently switch to "all".
  if (current && !values.some(v => v.value === current)) options.push(new Option(current, current));
  select.replaceChildren(...options);
  select.value = current;
}

function scheduleSearch() {
  clearTimeout(inv.searchTimer);
  inv.searchTimer = setTimeout(() => { if (searchKey() !== inv.lastKey) search(); }, SEARCH_DELAY_MS);
}

// Everything that decides what the list shows, as one string. Typing and
// the Search key compare it with the last search sent (inv.lastKey) and
// skip asking the board again for the same thing: by the time the Search
// key is pressed, the typing pause has usually searched already, and
// typing a letter and deleting it again lands on the same text.
// Saving, deleting and "Show more" call search() directly and always ask.
function searchKey() {
  return JSON.stringify([inv.view, inv.q.trim(), inv.category, inv.manufacturer, inv.sort, inv.order, inv.bin]);
}

// What a list view (cards or table) shows, without the view itself: cards
// and table show the same page of parts, only drawn differently.
function listKey() {
  return JSON.stringify([inv.q.trim(), inv.category, inv.manufacturer, inv.sort, inv.order, inv.bin]);
}

// What the bin grid shows: the search words and the two filters (it shows
// every bin, so the bin filter and the sort don't matter).
function binsKey() {
  return JSON.stringify([inv.q.trim(), inv.category, inv.manufacturer]);
}

// Forgets what the views have loaded, after the parts changed (saved,
// deleted, a failed quantity change): the next view switch asks the board.
function forgetResults() {
  inv.listKey = '';
  inv.binsKey = '';
}

// Switches the view. Every search reads the whole parts table on the board,
// which takes a moment with a few hundred parts, so a view that already
// holds the answer to the same search is shown from it: cards and table
// share the list, and the bin grid keeps its counts. Otherwise it asks.
function showView(view) {
  inv.view = view;
  const cached = view === 'bins' ? inv.binsKey === binsKey() : inv.listKey === listKey();
  if (!cached) return search();
  // A search still on its way was for something else: its answer is
  // dropped (see stale()), and this view stays as shown.
  inv.seq++;
  inv.lastKey = searchKey();
  renderInventory();
}

// Fetches the first page (or, with `more`, the next one) for the current
// search, filters and sort.
//
// At most one search is on its way to the board at a time. The board works
// through requests one after another, and every search reads the whole
// parts table, so typing "stm32" letter by letter would otherwise queue a
// search for "s", "st", "stm"... with the last one waiting behind all of
// them, each also holding its answer in the board's RAM until sent. So a
// search asked for while one is running only marks that another is needed;
// when the running one returns, its answer is dropped and one search for
// whatever the field says by then follows.
async function search(more = false) {
  clearTimeout(inv.searchTimer);
  if (inv.searching) {
    inv.searchAgain = true;
    return;
  }
  inv.searching = true;
  try {
    await runSearch(more);
  } finally {
    inv.searching = false;
    if (inv.searchAgain) {
      inv.searchAgain = false;
      search();
    }
  }
}

function stale(seq) {
  return seq !== inv.seq || inv.searchAgain;
}

async function runSearch(more) {
  const seq = ++inv.seq;
  if (!more) inv.lastKey = searchKey();
  if (inv.view === 'bins') return searchBins(seq);
  const params = searchParams();
  params.set('sort', inv.sort);
  params.set('order', inv.order);
  params.set('limit', PAGE_SIZE);
  params.set('offset', more ? inv.parts.length : 0);
  if (inv.bin) { params.set('x', inv.bin.x); params.set('y', inv.bin.y); }
  let res;
  try {
    res = await api(`/api/parts?${params}`);
  } catch (e) {
    if (!stale(seq)) searchFailed(e);
    return;
  }
  if (stale(seq)) return;  // a newer search is on its way
  searchWorked();
  inv.parts = more ? inv.parts.concat(res.parts) : res.parts;
  inv.total = res.total;
  inv.listKey = listKey();
  renderInventory();
  if (!more) autoLocate();
}

// The search words and the two filters, as /api/parts and /api/parts/bins
// take them. The bin filter is not here: the bin grid shows every bin.
function searchParams() {
  const params = new URLSearchParams();
  if (inv.q.trim()) params.set('q', inv.q.trim());
  if (inv.category) params.set('category', inv.category);
  if (inv.manufacturer) params.set('manufacturer', inv.manufacturer);
  return params;
}

function searchFailed(e) {
  inv.lastKey = '';  // nothing was shown, so the same search may be tried again
  // 503: no database (failed to open); everything else is a passing error.
  const unavailable = /unavailable/.test(e.message);
  $('#inv-unavailable').hidden = !unavailable;
  $('#inv-main').hidden = unavailable;
  if (!unavailable) toast(e.message, true);
}

function searchWorked() {
  $('#inv-unavailable').hidden = true;
  $('#inv-main').hidden = false;
}

// The bin grid's search: with search words or a filter set, asks the board
// which bins hold matching parts and how many (inv.binHits, keyed "x,y";
// "-1,-1" counts the matches in no bin), so the grid can show where the
// results are. Without any, the grid shows every bin's own part count and
// inv.binHits is null. The bins themselves are reloaded too, since counts
// change as parts are edited.
async function searchBins(seq) {
  const params = searchParams();
  const searching = [...params.keys()].length > 0;
  let hits = null;
  try {
    [, hits] = await Promise.all([loadBins(), searching ? api(`/api/parts/bins?${params}`) : null]);
  } catch (e) {
    if (!stale(seq)) searchFailed(e);
    return;
  }
  if (stale(seq)) return;  // a newer search is on its way
  searchWorked();
  inv.binHits = hits ? new Map(hits.bins.map(b => [`${b.x},${b.y}`, b.parts])) : null;
  inv.binsKey = binsKey();
  renderInventory();
}

// Keeps the view switch and the sort on one line. CSS already scales them
// with the screen width; when a sort field's name still wouldn't fit (a
// narrow phone, or the longer Russian and Spanish words), the view buttons
// drop their words and show icons only, which frees most of the line. The
// words stay as the buttons' tooltips and accessible names.
//
// The test uses the longest field name, not the one shown, so the buttons
// don't switch between words and icons each time the sort changes.
function fitInventoryBar() {
  const bar = $('.inv-bar');
  const label = $('#inv-sort-label');
  bar.classList.remove('compact');
  if (!label.offsetParent) return;  // page or sort hidden: nothing to measure
  const shown = label.textContent;
  const names = [...$('#inv-sort').options].map(o => o.textContent);
  label.textContent = names.reduce((a, b) => (b.length > a.length ? b : a), shown);
  if (label.scrollWidth > label.clientWidth) bar.classList.add('compact');
  label.textContent = shown;
}

function filtered() {
  return !!(inv.q.trim() || inv.category || inv.manufacturer || inv.bin);
}

function renderInventory() {
  const view = inv.view;
  for (const b of $$('#inv-view button')) b.classList.toggle('on', b.dataset.view === view);
  const bins = view === 'bins';
  const sort = $('#inv-sort');
  sort.value = inv.sort;
  $('#inv-sort-label').textContent = sort.selectedOptions[0] ? sort.selectedOptions[0].textContent : '';
  $('#inv-order').textContent = inv.order === 'asc' ? '↑' : '↓';
  $('#inv-order').title = t(inv.order === 'asc' ? 'inv.ascending' : 'inv.descending');
  // The grid has a fixed layout, so sorting means nothing there.
  $('#inv-sort-box').hidden = bins;
  fitInventoryBar();

  // The bin chip filters the lists; the grid always shows every bin.
  const filter = $('#inv-bin-filter');
  filter.hidden = bins || !inv.bin;
  if (inv.bin) $('#inv-bin-name').textContent = t('inv.inBin', { bin: binName(inv.bin.x, inv.bin.y) });

  $('#inv-cards').hidden = view !== 'cards';
  $('#inv-table-wrap').hidden = view !== 'table';
  $('#inv-bins').hidden = !bins;
  $('#inv-more').hidden = bins || inv.parts.length >= inv.total;
  const empty = $('#inv-empty');
  empty.textContent = t(filtered() ? 'inv.noMatch' : 'inv.noParts');

  if (bins) {
    // With a search: how many matches, in how many bins, and how many of
    // them are in no bin (those can't be shown on the grid).
    let count = '';
    let matches = 0;
    if (inv.binHits) {
      let inBins = 0;
      for (const [key, n] of inv.binHits) {
        matches += n;
        if (key !== '-1,-1') inBins++;
      }
      const loose = inv.binHits.get('-1,-1') || 0;
      if (matches) count = t('inv.binHits', { n: matches, bins: inBins });
      if (loose) count += ` · ${t('inv.looseHits', { n: loose })}`;
    }
    $('#inv-count').textContent = count;
    empty.hidden = !inv.binHits || matches > 0;
    return renderBinGrid();
  }
  $('#inv-count').textContent = inv.total ? t('inv.count', { n: inv.total }) : '';
  empty.hidden = inv.total > 0;
  if (view === 'cards') $('#inv-cards').replaceChildren(...inv.parts.map(buildPartCard));
  else $('#inv-table tbody').replaceChildren(...inv.parts.map(buildPartRow));
  for (const th of $$('#inv-table th')) {
    th.classList.toggle('sorted', th.dataset.sort === inv.sort);
    th.dataset.order = th.dataset.sort === inv.sort ? inv.order : '';
  }
}

// The line under a part's description: category, package, value.
function partMeta(p) {
  const value = p.value ? `${p.value}${p.unit ? ` ${p.unit}` : ''}` : '';
  return [p.category, p.package, value].filter(Boolean).join(' · ');
}

function buildPartCard(p) {
  const card = $('#part-card').content.firstElementChild.cloneNode(true);
  card.dataset.id = p.id;
  fillPart(card, p);
  card.addEventListener('click', e => { if (!e.target.closest('button, a')) openPart(p.id); });
  wirePartButtons(card, p.id);
  return card;
}

// One table row. The same quantity and bin buttons as a card, smaller.
function buildPartRow(p) {
  const tr = document.createElement('tr');
  tr.dataset.id = p.id;
  tr.innerHTML = '<td class="thumb-col"><img class="part-thumb" alt="" loading="lazy" decoding="async" hidden></td>'
    + '<td><b class="part-pn"></b></td><td class="part-desc"></td><td class="part-maker"></td>'
    + '<td class="part-pkg"></td><td class="num"><div class="qty small"><button class="qty-btn" data-delta="-1">&minus;</button>'
    + '<span class="qty-value"></span><button class="qty-btn" data-delta="1">+</button></div></td>'
    + '<td><button class="bin-btn secondary small"></button></td>';
  fillPart(tr, p);
  tr.addEventListener('click', e => { if (!e.target.closest('button, a')) openPart(p.id); });
  wirePartButtons(tr, p.id);
  return tr;
}

// Fills a card or a table row (they use the same class names) from a part.
function fillPart(el, p) {
  // A part may have only a description; the card then leads with that.
  const set = (sel, text) => { const n = el.querySelector(sel); if (n) n.textContent = text; };
  set('.part-pn', p.partNumber || p.description);
  set('.part-maker', p.manufacturer);
  set('.part-desc', p.partNumber ? p.description : '');
  set('.part-meta', partMeta(p));
  set('.part-pkg', p.package);
  const meta = el.querySelector('.part-meta');
  if (meta && p.datasheetUrl) {
    const a = document.createElement('a');
    a.href = p.datasheetUrl;
    a.target = '_blank';
    a.rel = 'noopener';
    a.textContent = t('inv.datasheet');
    if (meta.textContent) meta.append(' · ');
    meta.append(a);
  }
  set('.qty-value', String(p.quantity));
  el.classList.toggle('out', p.quantity === 0);
  // Nothing to take away at zero: the minus button is disabled, not silent.
  const minus = el.querySelector('.qty-btn[data-delta="-1"]');
  if (minus) minus.disabled = p.quantity <= 0;
  const thumb = el.querySelector('.part-thumb');
  if (thumb) {
    thumb.hidden = !p.photo;
    if (p.photo) thumb.src = photoUrl(p.photo, true);
    else thumb.removeAttribute('src');
  }
  const bin = el.querySelector('.bin-btn');
  bin.textContent = p.bin ? binName(p.bin.x, p.bin.y) : t('inv.noBin');
  bin.disabled = !p.bin;
  // Lit like the bin's button on the Grid page while its LEDs are on (see
  // showLitBins(), which keeps it up to date).
  if (p.bin) bin.dataset.bin = `${p.bin.x},${p.bin.y}`;
  else delete bin.dataset.bin;
  bin.classList.toggle('lit', !!p.bin && isLit(p.bin.x, p.bin.y));
  bin.title = !p.bin ? '' : t(bin.classList.contains('lit') ? 'inv.unlocate' : 'inv.locate');
}

function wirePartButtons(el, id) {
  const thumb = el.querySelector('.part-thumb');
  if (thumb) {
    thumb.addEventListener('click', e => {
      e.stopPropagation();  // the card or row itself opens the edit form
      const p = inv.parts.find(x => x.id === id);
      if (p && p.photo) openPhoto(photoUrl(p.photo));
    });
  }
  for (const b of $$('.qty-btn', el)) b.addEventListener('click', () => adjustQuantity(id, Number(b.dataset.delta)));
  // Like a bin on the Grid page: a dark bin lights up, a lit one goes out.
  el.querySelector('.bin-btn').addEventListener('click', () => {
    const p = inv.parts.find(x => x.id === id);
    if (!p || !p.bin) return;
    if (isLit(p.bin.x, p.bin.y)) unlocateBin(p.bin.x, p.bin.y);
    else locateBin(p.bin.x, p.bin.y);
  });
}

// Redraws every place a part is shown, after it changed.
function refreshPart(p) {
  const i = inv.parts.findIndex(x => x.id === p.id);
  if (i >= 0) inv.parts[i] = p;
  for (const el of $$(`[data-id="${p.id}"]`)) fillPart(el, p);
}

// Quantity +/-: the number changes at once; the board gets the taps in as
// few requests as possible. While one request is on its way, further taps add
// up and go as one when it returns (each change is a flash write on the
// board, a fraction of a second). The answer is only shown once no more taps
// are waiting, so the number never jumps back under the user's thumb.
const adjustQueue = new Map();  // id -> {busy, pending}
function adjustQuantity(id, delta) {
  const p = inv.parts.find(x => x.id === id);
  if (!p || p.quantity + delta < 0) return;
  refreshPart({ ...p, quantity: p.quantity + delta });
  const q = adjustQueue.get(id) || { busy: false, pending: 0 };
  adjustQueue.set(id, q);
  q.pending += delta;
  if (!q.busy) flushAdjust(id);
}

async function flushAdjust(id) {
  const q = adjustQueue.get(id);
  const delta = q.pending;
  q.pending = 0;
  q.busy = true;
  const res = await attempt(() => api('/api/part/adjust', { form: { id, delta } }));
  q.busy = false;
  if (q.pending) return flushAdjust(id);
  if (res) refreshPart(res);
  else {
    // The change failed: show what the board has.
    forgetResults();
    search();
  }
}

async function locateBin(x, y, quiet = false) {
  const res = await attempt(() => api('/api/locate', { form: { x, y } }));
  if (!res) return;
  updateLitBins(res);
  if (!quiet) toast(t('inv.located', { bin: binName(x, y) }));
}

// Turns a located bin off (it fades out), from a part's bin button.
async function unlocateBin(x, y) {
  const res = await attempt(() => api('/api/locate/clear', { form: { x, y } }));
  if (res) updateLitBins(res);
}

// Whether the board reports the bin as lit (state.litBins).
function isLit(x, y) {
  return state.litBins.some(([lx, ly]) => lx === x && ly === y);
}

// Optional: when a search narrows the parts down to one bin, light it. Only
// once per result set, so typing more letters doesn't light it again and
// again, and only for a search or filter (not the full, unfiltered list).
function autoLocate() {
  if (!$('#inv-autolocate').checked || !filtered() || !inv.total || inv.parts.length < inv.total) return;
  const first = inv.parts[0].bin;
  if (!first || !inv.parts.every(p => p.bin && p.bin.x === first.x && p.bin.y === first.y)) {
    inv.located = '';
    return;
  }
  const key = `${first.x},${first.y}`;
  if (key === inv.located) return;
  inv.located = key;
  locateBin(first.x, first.y, true);
}

// The bin grid: every bin with its name and number of parts. A tap shows that
// bin's parts in the list view used last. Bins left outside the grid after it
// shrank, but still holding parts, are listed below it.
function renderBinGrid() {
  const g = state.config && state.config.grid;
  const view = $('#inv-bins');
  if (!g) return view.replaceChildren();
  // Bins share the width down to a size that can still be read and tapped
  // (--bin-min in app.css); a wider grid scrolls sideways instead.
  view.style.gridTemplateColumns = `repeat(${g.cols}, minmax(var(--bin-min), 1fr))`;
  const cells = [];
  const extra = [];
  for (const b of inv.bins) {
    // While searching, a bin counts only its matching parts; bins without
    // any look empty, so the ones holding results stand out.
    const n = inv.binHits ? inv.binHits.get(`${b.x},${b.y}`) || 0 : b.parts;
    const cell = document.createElement('button');
    cell.className = n ? 'has-parts' : 'empty';
    if (inv.binHits && n) cell.classList.add('hit');
    const name = document.createElement('span');
    name.className = 'bin-name';
    name.textContent = b.label || binLabel(b.x, b.y);
    const count = document.createElement('span');
    count.className = 'bin-count';
    count.textContent = n ? String(n) : '';
    cell.append(name, count);
    cell.title = t(inv.binHits ? 'inv.binHitTitle' : 'inv.binTitle', { bin: binLabel(b.x, b.y), n });
    // A lit bin gets a dot in its corner (see showLitBins()).
    cell.dataset.bin = `${b.x},${b.y}`;
    cell.classList.toggle('lit', isLit(b.x, b.y));
    cell.addEventListener('click', () => {
      inv.bin = { x: b.x, y: b.y };
      inv.view = pref('listView', 'cards');
      search();
    });
    if (b.outside) {
      cell.classList.add('outside');
      extra.push(cell);
    } else {
      cells.push(cell);
    }
  }
  view.replaceChildren(...cells);
  // Outside bins go in their own row of plain buttons after the grid.
  // They come with a line saying why they're there: after a grid change they
  // can look like part of the grid, in an odd order.
  let outside = $('#inv-bins-outside');
  if (!outside) {
    outside = document.createElement('div');
    outside.id = 'inv-bins-outside';
    outside.className = 'chips';
    const note = document.createElement('p');
    note.id = 'inv-bins-outside-note';
    note.className = 'muted small';
    view.after(note, outside);
  }
  const note = $('#inv-bins-outside-note');
  note.textContent = t('inv.binsOutside');
  note.hidden = outside.hidden = !extra.length;
  outside.replaceChildren(...extra);
}

// ---- Add / edit form ---------------------------------------------------

const TEXT_FIELDS = ['partNumber', 'manufacturer', 'category', 'description', 'package', 'value', 'unit',
  'datasheetUrl', 'notes'];
let editing = null;  // the part being edited, or null when adding

// Autocomplete for the fields with recurring values. Loaded when the form
// opens, at most once a minute (the lists change only as parts are saved).
async function loadSuggestions() {
  if (Date.now() - inv.valuesAt < 60000) return;
  inv.valuesAt = Date.now();
  await Promise.all(['manufacturer', 'category', 'package', 'unit'].map(async field => {
    const res = await attempt(() => api(`/api/parts/values?field=${field}&limit=200`));
    if (!res) return;
    suggest.values[field] = res.values.map(v => v.value);
  }));
}

// ---- Suggestion lists ----------------------------------------------------
//
// The fields with recurring values (data-suggest="category" and the like)
// get their own list under the field, not the browser's <datalist>. On
// phones a datalist choice is filled in by the browser's autofill, which
// tells the system's autofill service (a password manager) that a form was
// filled, and that popped up its window on every choice.
//
// The list shows the values containing what was typed, those starting with
// it first. A tap or Enter takes one; Escape or leaving the field closes it.

const SUGGEST_MAX = 8;
const suggest = { values: {}, input: null, list: null, active: -1 };

function suggestMatches(input) {
  const all = suggest.values[input.dataset.suggest] || [];
  const q = input.value.trim().toLowerCase();
  if (!q) return [];
  const starts = [], contains = [];
  for (const v of all) {
    const low = v.toLowerCase();
    if (low === q) continue;  // already typed in full
    if (low.startsWith(q)) starts.push(v);
    else if (low.includes(q)) contains.push(v);
  }
  return starts.concat(contains).slice(0, SUGGEST_MAX);
}

function closeSuggest() {
  if (suggest.list) suggest.list.remove();
  suggest.list = null;
  suggest.input = null;
  suggest.active = -1;
}

function showSuggest(input) {
  const matches = suggestMatches(input);
  if (!matches.length) return closeSuggest();
  if (!suggest.list || suggest.input !== input) {
    closeSuggest();
    const list = document.createElement('ul');
    list.className = 'suggest';
    list.setAttribute('role', 'listbox');
    // pointerdown, not click: it comes before the field loses the focus, and
    // preventDefault() keeps the focus (and a phone's keyboard) where it is.
    list.addEventListener('pointerdown', e => {
      const item = e.target.closest('li');
      if (!item) return;
      e.preventDefault();
      pickSuggest(item.textContent);
    });
    // The field's label holds it, so it sits right under the field.
    input.parentElement.append(list);
    suggest.list = list;
    suggest.input = input;
  }
  suggest.active = -1;
  suggest.list.replaceChildren(...matches.map(v => {
    const li = document.createElement('li');
    li.setAttribute('role', 'option');
    li.textContent = v;
    return li;
  }));
}

function pickSuggest(value) {
  const input = suggest.input;
  closeSuggest();
  if (!input) return;
  input.value = value;
  input.focus();
}

function moveSuggest(step) {
  const items = suggest.list ? [...suggest.list.children] : [];
  if (!items.length) return;
  suggest.active = (suggest.active + step + items.length) % items.length;
  items.forEach((li, i) => li.classList.toggle('active', i === suggest.active));
  items[suggest.active].scrollIntoView({ block: 'nearest' });
}

function setupSuggest(root) {
  for (const input of root.querySelectorAll('input[data-suggest]')) {
    input.addEventListener('input', () => showSuggest(input));
    input.addEventListener('blur', closeSuggest);
    input.addEventListener('keydown', e => {
      if (!suggest.list || suggest.input !== input) return;
      if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
        e.preventDefault();
        moveSuggest(e.key === 'ArrowDown' ? 1 : -1);
      } else if (e.key === 'Enter' && suggest.active >= 0) {
        e.preventDefault();  // a choice, not the form's submit
        pickSuggest(suggest.list.children[suggest.active].textContent);
      } else if (e.key === 'Escape') {
        e.preventDefault();  // closes the list, not the whole dialog
        e.stopPropagation();
        closeSuggest();
      }
    });
  }
}

function binOptions(current) {
  const options = [new Option(t('inv.noBin'), '')];
  for (const b of inv.bins) {
    const pos = binLabel(b.x, b.y);
    options.push(new Option(b.label ? `${pos} – ${b.label}` : pos, `${b.x},${b.y}`));
  }
  const select = $('#part-form [name=bin]');
  select.replaceChildren(...options);
  select.value = current ? `${current.x},${current.y}` : '';
}

async function openPart(id) {
  let part = null;
  if (id) {
    part = await attempt(() => api(`/api/part?id=${id}`));
    if (!part) return;
  }
  editing = part;
  const form = $('#part-form');
  form.reset();
  $('#part-title').textContent = t(part ? 'inv.editTitle' : 'inv.addTitle');
  $('#part-delete').hidden = !part;
  if (part) {
    for (const f of TEXT_FIELDS) form.elements[f].value = part[f] || '';
    form.elements.quantity.value = part.quantity;
  } else {
    // A new part starts with what the list is filtered to: adding parts to a
    // bin or a category in a row needs no retyping.
    form.elements.quantity.value = 1;
    form.elements.category.value = inv.category;
    form.elements.manufacturer.value = inv.manufacturer;
  }
  binOptions(part ? part.bin : inv.bin);
  resetPhotoEdit();
  $('#part-times').textContent = part ? partTimes(part) : '';
  $('#part-dialog').showModal();
  if (!part) form.elements.partNumber.focus();
  loadSuggestions();
  loadPhotoInfo();
}

function partTimes(p) {
  // 0: saved before the board knew the time (no network time yet).
  const when = s => (s ? new Date(s * 1000).toLocaleString(state.lang) : '');
  const parts = [];
  if (p.created) parts.push(t('inv.created', { when: when(p.created) }));
  if (p.updated && p.updated !== p.created) parts.push(t('inv.updated', { when: when(p.updated) }));
  return parts.join(' · ');
}

async function savePart() {
  const form = $('#part-form');
  const data = {};
  for (const f of TEXT_FIELDS) data[f] = form.elements[f].value.trim();
  if (!data.partNumber && !data.description) return toast(t('inv.needName'), true);
  const qty = form.elements.quantity.value.trim();
  if (!/^\d+$/.test(qty)) return toast(t('inv.badQuantity'), true);
  data.quantity = qty;
  const bin = form.elements.bin.value;
  if (bin) [data.x, data.y] = bin.split(',');
  else data.x = -1;
  if (editing) data.id = editing.id;
  const save = $('#part-save');
  save.disabled = true;
  try {
    const res = await attempt(() => api(editing ? '/api/part/update' : '/api/part/add', { form: data }));
    if (!res) return;
    // From here on the part exists: a retry after a failed photo upload
    // updates it instead of adding it a second time.
    editing = res;
    if (!await savePhoto(res)) return;  // the dialog stays open to try again
  } finally {
    save.disabled = photoEdit.busy;
  }
  $('#part-dialog').close();
  toast(t('inv.saved'));
  inv.valuesAt = 0;  // new values may have appeared for the autocomplete
  forgetResults();
  await Promise.all([loadBins(), loadFilterLists()]);
  search();
}

// ---- Photos ------------------------------------------------------------
//
// The board can't decode images (no RAM to spare), so the page does all of
// it: a picture from the camera or the gallery is turned upright, scaled to
// the board's size (640 px on the long side in flash, 800 px on a microSD
// card) and a 128 px thumbnail, both as JPEG. Only those two small files are
// uploaded. In the part form a new or removed photo waits for Save, like the
// other fields.

const photoInfo = { loaded: false, maxSide: 640, thumbSide: 128, maxBytes: 400 * 1024, maxThumbBytes: 48 * 1024 };
// The form's photo change: a prepared photo and thumbnail (Blobs), or a
// removal; `busy` while a picture is being prepared or uploaded.
let photoEdit = { photo: null, thumb: null, remove: false, busy: false, preview: '' };

const photoUrl = (name, thumb = false) => `/photos/${name}${thumb ? '-t' : ''}.jpg`;

async function loadPhotoInfo() {
  if (photoInfo.loaded) return;
  const res = await attempt(() => api('/api/photos'));
  if (res) Object.assign(photoInfo, res, { loaded: true });
}

function resetPhotoEdit() {
  if (photoEdit.preview) URL.revokeObjectURL(photoEdit.preview);
  photoEdit = { photo: null, thumb: null, remove: false, busy: false, preview: '' };
  $('#part-photo-file').value = '';
  $('#part-photo-status').textContent = '';
  renderDialogPhoto();
}

// The photo area of the form: the pending photo, else the part's own, else
// "No photo"; buttons to match.
function renderDialogPhoto() {
  const img = $('#part-photo-img');
  const current = editing && editing.photo && !photoEdit.remove ? photoUrl(editing.photo) : '';
  const src = photoEdit.preview || current;
  img.hidden = !src;
  if (src) img.src = src;
  else img.removeAttribute('src');
  $('#part-photo-empty').hidden = !!src;
  $('#part-photo-remove').hidden = !src;
  $('#part-photo-pick-label').textContent = t(src ? 'photo.replace' : 'photo.add');
  $('#part-photo-pick').classList.toggle('disabled', photoEdit.busy);
  $('#part-photo-file').disabled = photoEdit.busy;
  $('#part-save').disabled = photoEdit.busy;
}

// Decodes a picture into something a canvas can draw, turned upright: phone
// cameras store portrait photos sideways with an orientation tag.
async function decodePicture(file) {
  if (window.createImageBitmap) {
    try {
      return await createImageBitmap(file, { imageOrientation: 'from-image' });
    } catch (e) { /* older browser or format: try an <img> */ }
  }
  const url = URL.createObjectURL(file);
  const img = new Image();
  img.src = url;
  try {
    await img.decode();  // <img> applies the orientation tag itself
  } catch (e) {
    URL.revokeObjectURL(url);
    throw new Error(t('photo.unreadable'));
  }
  img.cleanup = () => URL.revokeObjectURL(url);
  return img;
}

// Scales a decoded picture so its long side is at most `side` and encodes it
// as JPEG no larger than `maxBytes`, lowering the quality if needed.
async function encodeJpeg(source, side, maxBytes, qualities) {
  let w = source.width || source.naturalWidth;
  let h = source.height || source.naturalHeight;
  const scale = Math.min(1, side / Math.max(w, h));
  const tw = Math.max(1, Math.round(w * scale));
  const th = Math.max(1, Math.round(h * scale));
  // Halve in steps first: one big jump from a 12-megapixel photo to 640 px
  // skips most pixels and looks grainy.
  let from = source;
  while (w / 2 >= tw * 1.5) {
    w = Math.round(w / 2);
    h = Math.round(h / 2);
    const step = document.createElement('canvas');
    step.width = w;
    step.height = h;
    step.getContext('2d').drawImage(from, 0, 0, w, h);
    from = step;
  }
  const canvas = document.createElement('canvas');
  canvas.width = tw;
  canvas.height = th;
  const ctx = canvas.getContext('2d');
  ctx.imageSmoothingQuality = 'high';
  ctx.fillStyle = '#fff';  // a PNG's transparent areas would turn black in JPEG
  ctx.fillRect(0, 0, tw, th);
  ctx.drawImage(from, 0, 0, tw, th);
  for (const q of qualities) {
    const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', q));
    if (blob && blob.size <= maxBytes) return blob;
  }
  throw new Error(t('photo.tooLarge'));
}

async function pickPhoto(file) {
  if (!file) return;
  photoEdit.busy = true;
  $('#part-photo-status').textContent = t('photo.preparing');
  renderDialogPhoto();
  try {
    await loadPhotoInfo();
    const picture = await decodePicture(file);
    try {
      const photo = await encodeJpeg(picture, photoInfo.maxSide, photoInfo.maxBytes, [0.85, 0.75, 0.6, 0.45]);
      const thumb = await encodeJpeg(picture, photoInfo.thumbSide, photoInfo.maxThumbBytes, [0.75, 0.6, 0.45]);
      if (photoEdit.preview) URL.revokeObjectURL(photoEdit.preview);
      Object.assign(photoEdit, { photo, thumb, remove: false, preview: URL.createObjectURL(photo) });
      $('#part-photo-status').textContent = t('photo.ready', { size: formatBytes(photo.size) });
    } finally {
      if (picture.close) picture.close();  // ImageBitmap: free its memory now
      if (picture.cleanup) picture.cleanup();
    }
  } catch (e) {
    $('#part-photo-status').textContent = '';
    toast(e.message, true);
  } finally {
    photoEdit.busy = false;
    $('#part-photo-file').value = '';  // picking the same file again still fires "change"
    renderDialogPhoto();
  }
}

function removeDialogPhoto() {
  if (photoEdit.preview) URL.revokeObjectURL(photoEdit.preview);
  Object.assign(photoEdit, { photo: null, thumb: null, preview: '', remove: !!(editing && editing.photo) });
  $('#part-photo-status').textContent = photoEdit.remove ? t('photo.removing') : '';
  renderDialogPhoto();
}

// Applies the form's photo change to a saved part. True when done (or
// nothing to do); false after a failure, which has been shown.
async function savePhoto(part) {
  if (!photoEdit.photo && !photoEdit.remove) return true;
  photoEdit.busy = true;
  renderDialogPhoto();
  const status = $('#part-photo-status');
  try {
    if (photoEdit.photo) {
      status.textContent = t('photo.uploading');
      const saved = await api(`/api/part/photo?id=${part.id}`, { body: photoEdit.photo, type: 'image/jpeg' });
      editing = saved;
      // The thumbnail is a nicety: without it the list shows the photo itself.
      try {
        await api(`/api/part/thumb?id=${part.id}&name=${encodeURIComponent(saved.photo)}`,
          { body: photoEdit.thumb, type: 'image/jpeg' });
      } catch (e) { /* keep going */ }
    } else if (photoEdit.remove && part.photo) {
      editing = await api('/api/part/photo/delete', { form: { id: part.id } });
    }
    photoEdit.photo = photoEdit.thumb = null;
    photoEdit.remove = false;
    return true;
  } catch (e) {
    status.textContent = '';
    toast(t('photo.failed', { msg: e.message }), true);
    return false;
  } finally {
    photoEdit.busy = false;
    renderDialogPhoto();
  }
}

// Shows a photo full screen. The image stays hidden (with a spinner in its
// place) until the new photo has loaded: an <img> keeps showing its previous
// picture while the next one downloads, which over a slow link looked as if
// the wrong photo had opened. Closing the viewer clears the picture too.
function openPhoto(src) {
  const viewer = $('#photo-viewer');
  const img = $('#photo-viewer-img');
  viewer.classList.add('loading');
  img.src = src;
  // A cached photo may already be complete here, before any load event.
  if (img.complete && img.naturalWidth) viewer.classList.remove('loading');
  viewer.showModal();
}

async function deletePart() {
  if (!editing) return;
  const name = editing.partNumber || editing.description;
  if (!confirm(t('inv.deleteConfirm', { name }))) return;
  if (!await attempt(() => api('/api/part/delete', { form: { id: editing.id } }))) return;
  $('#part-dialog').close();
  toast(t('inv.deleted', { name }));
  forgetResults();
  await Promise.all([loadBins(), loadFilterLists()]);
  search();
}

async function renameBin() {
  if (!inv.bin) return;
  const b = findBin(inv.bin.x, inv.bin.y);
  const label = prompt(t('inv.binRenamePrompt', { bin: binLabel(inv.bin.x, inv.bin.y) }), (b && b.label) || '');
  if (label === null) return;
  if (!await attempt(() => api('/api/bin/label', { form: { x: inv.bin.x, y: inv.bin.y, label: label.trim() } }))) return;
  await loadBins();
  renderInventory();
}

function bindInventory() {
  inv.view = pref('view', 'cards');
  window.addEventListener('resize', fitInventoryBar);
  inv.sort = pref('sort', 'partNumber');
  inv.order = pref('order', 'asc');
  $('#inv-autolocate').checked = pref('autoLocate', '0') === '1';

  $('#inv-q').addEventListener('input', e => { inv.q = e.target.value; scheduleSearch(); });
  // Enter (the keyboard's Search key on a phone) searches at once and takes
  // the focus off the field, which closes the on-screen keyboard so it
  // doesn't cover the results; tapping the field brings it back.
  $('#inv-q').addEventListener('keydown', e => {
    if (e.key !== 'Enter') return;
    e.preventDefault();
    e.target.blur();
    clearTimeout(inv.searchTimer);
    if (searchKey() !== inv.lastKey) search();
  });
  $('#inv-category').addEventListener('change', e => { inv.category = e.target.value; search(); });
  $('#inv-maker').addEventListener('change', e => { inv.manufacturer = e.target.value; search(); });
  const setSort = (key, order) => {
    inv.sort = key;
    inv.order = order;
    setPref('sort', key);
    setPref('order', order);
    search();
  };
  $('#inv-sort').addEventListener('change', e => setSort(e.target.value, DESC_FIRST.has(e.target.value) ? 'desc' : 'asc'));
  $('#inv-order').addEventListener('click', () => setSort(inv.sort, inv.order === 'asc' ? 'desc' : 'asc'));
  // Table headers sort too: a second click on the same one reverses it.
  for (const th of $$('#inv-table th[data-sort]')) {
    th.addEventListener('click', () => {
      const key = th.dataset.sort;
      setSort(key, key === inv.sort ? (inv.order === 'asc' ? 'desc' : 'asc') : (DESC_FIRST.has(key) ? 'desc' : 'asc'));
    });
  }
  for (const b of $$('#inv-view button')) {
    b.addEventListener('click', () => {
      setPref('view', b.dataset.view);
      if (b.dataset.view !== 'bins') setPref('listView', b.dataset.view);
      // Each view needs its own answer from the board (a page of parts for
      // the lists, per-bin counts for the grid), unless it has it already.
      showView(b.dataset.view);
    });
  }
  $('#inv-autolocate').addEventListener('change', e => {
    setPref('autoLocate', e.target.checked ? '1' : '0');
    inv.located = '';
    autoLocate();
  });
  $('#inv-more').addEventListener('click', () => search(true));
  $('#inv-bin-clear').addEventListener('click', () => { inv.bin = null; search(); });
  $('#inv-bin-rename').addEventListener('click', renameBin);
  $('#inv-bin-locate').addEventListener('click', () => inv.bin && locateBin(inv.bin.x, inv.bin.y));
  $('#part-add').addEventListener('click', () => openPart(null));
  $('#part-form').addEventListener('submit', e => { e.preventDefault(); savePart(); });
  $('#part-cancel').addEventListener('click', () => $('#part-dialog').close());
  $('#part-photo-file').addEventListener('change', e => pickPhoto(e.target.files[0]));
  $('#part-photo-remove').addEventListener('click', removeDialogPhoto);
  $('#part-photo-view').addEventListener('click', () => {
    const img = $('#part-photo-img');
    if (!img.hidden) openPhoto(img.src);
  });
  // The viewer closes on any tap; a closed form forgets an unsaved photo.
  $('#photo-viewer').addEventListener('click', () => $('#photo-viewer').close());
  // Loaded (or failed): show the image, fading in. The events only fire for
  // the current src, as setting a new one cancels the previous download.
  const viewerImg = $('#photo-viewer-img');
  const viewerShown = () => $('#photo-viewer').classList.remove('loading');
  viewerImg.addEventListener('load', viewerShown);
  viewerImg.addEventListener('error', viewerShown);
  $('#photo-viewer').addEventListener('close', () => viewerImg.removeAttribute('src'));
  $('#part-dialog').addEventListener('close', resetPhotoEdit);
  $('#part-delete').addEventListener('click', deletePart);
}

// ---------------------------------------------------------------------------
// Grid

async function enterGrid() {
  // The lights carry the motion sensor's state, which the effects card
  // shows (see renderMotion()).
  loadLights();
  if (!state.config) state.config = await attempt(() => api('/api/config'));
  if (!state.config) return;
  const g = state.config.grid;
  const view = $('#grid-view');
  if (view.dataset.size !== `${g.cols}x${g.rows}`) {
    view.dataset.size = `${g.cols}x${g.rows}`;
    // Columns share the width down to --bin-min (app.css), so ten bins still
    // fit a phone; a wider grid scrolls sideways instead of shrinking its
    // buttons until they can't be read or tapped.
    view.style.gridTemplateColumns = `repeat(${g.cols}, minmax(var(--bin-min), 1fr))`;
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
  const seq = effectSaves;  // a save started meanwhile has the newer settings
  const effects = await attempt(() => api('/api/effects'));
  if (effects && seq === effectSaves) showEffects(effects);
}

// ---------------------------------------------------------------------------
// Grid effects
//
// The board runs one effect at a time under the located bins. Every effect
// keeps its own settings on the board (config.json effects.<name>.*) and
// starts with them; the controls below the chips change the running one's.
// The running effect itself is not saved.

// `e`: an answer from /api/effects ({effect, remainingS, speed, ...}; the
// settings are missing while no effect runs).
function showEffects(e) {
  // The answer carries the grid status number of its moment (see isStale()):
  // a push from before it, still on its way, is then dropped and can't put
  // the previous effect back on the chips.
  if (e.version !== undefined && !(e.version < state.versions.leds)) state.versions.leds = e.version;
  // The music effects need the microphone; without one they stay hidden.
  for (const el of $$('#effects-card .music')) el.hidden = !e.microphone;
  state.effect = e.effect;
  showEffectRunning(e.effect, e.remainingS);
  const settings = $('#effect-settings');
  settings.hidden = e.effect === 'off' || e.speed === undefined;
  if (settings.hidden) return;
  for (const name of ['speed', 'intensity', 'brightness']) {
    const input = $(`#effect-${name}`);
    // Not setIfIdle: a slider just let go still has the focus, and another
    // effect's settings must replace it anyway.
    input.value = e[name];
    $(`#effect-${name}-out`).textContent = percent(input.value);
  }
  $('#effect-palette').value = e.palette;
  const timer = $('#effect-timer');
  // A timer set through the API that isn't in the list gets its own entry.
  if (![...timer.options].some(o => Number(o.value) === e.timer)) {
    timer.add(new Option(t('effects.minutes', { n: e.timer }), e.timer));
  }
  timer.value = String(e.timer);
}

// Marks the running effect and shows how long its timer has left. Also
// called with the grid status pushed over the WebSocket.
// The motion sensor's notice in the effects card: only while it keeps a
// running effect dark. The effect runs on meanwhile, so its timer still
// stops it on time.
function renderEffectsDark() {
  $('#effects-dark').hidden = !(state.motionDark && state.effectRunning);
}

function showEffectRunning(effect, remainingS) {
  for (const b of $$('#effect-pick button')) b.classList.toggle('on', b.dataset.effect === (effect || 'off'));
  state.effectRunning = !!effect && effect !== 'off';
  renderEffectsDark();
  const left = $('#effect-timer-left');
  left.hidden = !remainingS;
  if (remainingS) {
    const time = remainingS >= 60
      ? t('effects.minutes', { n: Math.ceil(remainingS / 60) })
      : t('effects.seconds', { n: remainingS });
    left.textContent = t('effects.timerLeft', { time });
  }
}

// Like the locate defaults (see saveLocateDefaults()), only the answer to the
// newest save is shown. Two settings changed in quick succession (speed, then
// intensity) each send a save, and the first answer still carries the old
// intensity: shown, it would put the slider back for a moment until the
// second answer arrives. `effectSavesOpen` counts saves not yet answered, so
// a refresh the WebSocket asks for meanwhile is skipped too (the last answer
// brings the board's settings anyway).
let effectSaves = 0;
let effectSavesOpen = 0;
async function saveEffects(params) {
  const seq = ++effectSaves;
  effectSavesOpen++;
  const e = await attempt(() => api('/api/effects', { form: params }));
  effectSavesOpen--;
  if (e && seq === effectSaves) showEffects(e);
}

// Locate defaults: how a bin lights up when it is located from anywhere (this
// page, the inventory, the API). Each change is saved on the board at once.
function showLocateDefaults(d) {
  state.locate = d;
  // The bin brightness comes with the defaults (it sits in the same card).
  if (d.brightness !== undefined) {
    setIfIdle($('#grid-brightness'), d.brightness);
    $('#grid-brightness-out').textContent = percent($('#grid-brightness').value);
    if (state.config) state.config.grid.brightness = d.brightness;
  }
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

// The page shows a change at once, and the board applies it as soon as the
// request arrives, also to the bins lit now; it writes the flash a moment
// later on its own (see ConfigStore::update() in the firmware). So there is
// nothing to wait for and no "saved" message: an error still shows as a
// toast, and the board's answer then puts the real settings back.
async function saveLocateDefaults(params) {
  if (state.locate) showLocateDefaults({ ...state.locate, ...params, s: Number(params.s ?? state.locate.s) });
  // Numbered so that only the answer to the newest save is shown: an older
  // answer (a slow one, or one overtaken while the color slider moves on)
  // would put an earlier setting back on screen for a moment.
  const seq = ++locateSaves;
  locateSavesOpen++;
  const d = await attempt(() => api('/api/locate/defaults', { form: params }));
  locateSavesOpen--;
  if (seq !== locateSaves || locateHueTimer) return;
  // Failed: show what the board has.
  showLocateDefaults(d || await attempt(() => api('/api/locate/defaults')) || state.locate);
}

// The locate color slider. Every save writes the board's flash, so a drag is
// not sent move by move: the swatch follows at once, and the color is saved
// once the slider has rested for a moment.
let locateHue;
let locateHueTimer = 0;  // a color waiting to be saved; 0 when none
let locateSaves = 0;     // saves started, see saveLocateDefaults()
let locateSavesOpen = 0; // saves not yet answered, see refreshGridSettings()
function onLocateHue(hue) {
  const hex = hueToHex(hue);
  $('#locate-swatch').style.background = `#${hex}`;
  clearTimeout(locateHueTimer);
  locateHueTimer = setTimeout(() => {
    locateHueTimer = 0;
    saveLocateDefaults({ color: hex });
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

// Marks the bins the board reports as lit ([[x, y], ...]): the buttons on
// the Grid page, the bin buttons of the parts in the cards and the table,
// and the bins of the inventory's bin grid.
function showLitBins() {
  const lit = new Set(state.litBins.map(([x, y]) => `${x},${y}`));
  for (const b of $$('#grid-view button')) b.classList.toggle('lit', lit.has(b.dataset.bin));
  for (const b of $$('#inv-cards .bin-btn[data-bin], #inv-table .bin-btn[data-bin], #inv-bins button[data-bin]')) {
    b.classList.toggle('lit', lit.has(b.dataset.bin));
    if (b.classList.contains('bin-btn')) b.title = t(b.classList.contains('lit') ? 'inv.unlocate' : 'inv.locate');
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
  await loadPins();
  renderPasswordState();
  renderNetwork();
  renderHardware();
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
// Hardware settings: the LED grid with a live preview of its
// wiring, the work-light channels, the microphone and the pin map. Each
// section saves into config.json, which the board checks field by field
// (the same rules as below, and more) and then restarts to apply: strips
// and pins are set up at boot.

const CHIPSETS = ['WS2812', 'WS2812B', 'SK6812', 'SK6812_RGBW', 'WS2811', 'WS2815'];
const COLOR_ORDERS = ['GRB', 'RGB', 'BRG', 'BGR', 'RBG', 'GBR'];
const MAX_GRID_PIXELS = 1500;  // config::kMaxGridPixels
const LIGHT_CHANNELS = 5;      // W1-W5; W1-W3 are universal, W4-W5 white only

function fillSelect(sel, values, current, label = v => v) {
  sel.replaceChildren(...values.map(v => {
    const o = document.createElement('option');
    o.value = v;
    o.textContent = label(v);
    return o;
  }));
  if (current !== undefined) sel.value = String(current);
}

function intValue(input) { return Math.round(Number(input.value)); }

function renderHardware() {
  const cfg = state.config;
  if (!cfg || !cfg.grid) return;
  const g = cfg.grid;
  $('#hw-cols').value = g.cols;
  $('#hw-rows').value = g.rows;
  $('#hw-ppb').value = String(g.pixelsPerBin);
  $('#hw-layout').value = g.layout;
  $('#hw-start').value = g.start;
  $('#hw-segments').value = String(g.segments);
  $('#hw-split').value = g.splitRow;
  fillSelect($('#hw-chipset'), CHIPSETS, g.chipset);
  fillSelect($('#hw-order'), COLOR_ORDERS, g.colorOrder);
  $('#hw-current').value = g.currentLimitMa;
  $('#hw-pixel-ma').value = g.pixelMa || '';
  $('#hw-pixel-ma').placeholder = pixelMaDefault(g.chipset);
  $('#hw-fade').value = cfg.fadeMs;
  $('#hw-invert').checked = !!g.invertData;
  renderGridPreview();
  renderLightFields();
  // The motion sensor first: the pin list shows the analog microphone's pin
  // while the sensor's box is ticked (see motionOn()).
  renderMotionFields();
  renderPinFields();
  updateMotionFields();  // its notice follows the microphone choice, set just now
  setHwDirty(false);
}

// ---- Saving the hardware sections ----
//
// One "Save and restart" for all of them: the bar below the sections shows
// up as soon as a field in any of them changes and stays at the bottom of the
// screen, so a first setup (grid, work lights, pins, sensor) is filled in
// completely and saved with one restart. Each section adds its part to the
// configuration read fresh from the board (applyCommon(), applyGrid(), ...);
// what the page doesn't show stays as it is.

let hwDirty = false;
// What blocks saving, per section ('' = nothing): the grid's size and
// wiring, a GPIO chosen twice.
const hwProblems = { grid: '', pins: '' };

function setHwDirty(dirty) {
  hwDirty = dirty;
  updateSaveBar();
}

function hwProblem() {
  return hwProblems.grid || hwProblems.pins;
}

function updateSaveBar() {
  const problem = hwProblem();
  $('#hw-savebar').hidden = !hwDirty;
  $('#hw-save').disabled = !!problem;
  const note = $('#hw-save-note');
  note.textContent = problem || t('hw.unsaved');
  note.classList.toggle('err', !!problem);
}

async function saveHardware() {
  const problem = hwProblem() || motionProblem();
  if (problem) return toast(problem, true);
  const cfg = await attempt(() => api('/api/config'));
  if (!cfg) return;
  applyCommon(cfg);
  applyGrid(cfg);
  applyLights(cfg);
  applyPins(cfg);
  applyMotion(cfg);  // after the pins: the sensor may take the analog microphone's pin
  await saveAndRestart(cfg);
}

// Common: what applies to the grid and the work lights alike.
function applyCommon(cfg) {
  cfg.fadeMs = intValue($('#hw-fade'));
}

// The grid as the form describes it now (not yet saved).
function gridForm() {
  return {
    cols: intValue($('#hw-cols')),
    rows: intValue($('#hw-rows')),
    pixelsPerBin: intValue($('#hw-ppb')),
    layout: $('#hw-layout').value,
    start: $('#hw-start').value,
    segments: intValue($('#hw-segments')),
    splitRow: intValue($('#hw-split')),
  };
}

// What's wrong with the grid fields, or '' (the board checks again).
function gridProblem(g) {
  if (!(g.cols >= 1 && g.cols <= 64 && g.rows >= 1 && g.rows <= 64)) return t('hw.errSize');
  if (g.segments === 2 && !(g.splitRow >= 1 && g.splitRow < g.rows)) return t('hw.errSplit', { max: g.rows - 1 });
  const pixels = g.cols * g.rows * g.pixelsPerBin;
  if (pixels > MAX_GRID_PIXELS) return t('hw.errPixels', { n: pixels, max: MAX_GRID_PIXELS });
  return '';
}

// Bins in the order the strip passes them, per data line: the same steps as
// GridMapper (lib/Led/src/GridMapper.h) in reverse, so the preview shows
// exactly what the firmware will do. Returns [[{x, y}, ...], ...].
function wiringOrder(g) {
  const startTop = g.start.startsWith('top'), startLeft = g.start.endsWith('left');
  const two = g.segments === 2;
  const lines = two ? [[0, g.splitRow], [g.splitRow, g.rows]] : [[0, g.rows]];
  return lines.map(([from, to]) => {
    const bins = [];
    for (let w = from; w < to; w++) {          // w = row in wiring order
      const rowInLine = w - from;
      const y = startTop ? w : g.rows - 1 - w;
      const ltr = g.layout === 'serpentine' && rowInLine % 2 ? !startLeft : startLeft;
      for (let c = 0; c < g.cols; c++) bins.push({ x: ltr ? c : g.cols - 1 - c, y });
    }
    return bins;
  });
}

// Live preview: the bins seen from the front, the strip's path through them
// (line A solid, line B dashed), each line's first pixel marked with its
// letter, and on small grids the index of each bin's first pixel.
function renderGridPreview() {
  const g = gridForm();
  $('#hw-split-box').hidden = g.segments !== 2;
  $('#hw-split').max = Math.max(1, g.rows - 1);
  const problem = gridProblem(g);
  const summary = $('#hw-grid-summary');
  summary.classList.toggle('err', !!problem);
  hwProblems.grid = problem;
  updateSaveBar();
  if (problem) {
    summary.textContent = problem;
    $('#hw-preview').replaceChildren();
    return;
  }
  const order = wiringOrder(g);
  const counts = order.map(line => line.length * g.pixelsPerBin);
  summary.textContent = g.segments === 2
    ? t('hw.summary2', { n: counts[0] + counts[1], a: counts[0], b: counts[1] })
    : t('hw.summary1', { n: counts[0] });

  // On small grids each bin shows the index of its first pixel in its upper
  // half, and the path runs through the lower half so both stay readable.
  const showIndex = g.cols * g.rows <= 160;
  const cell = 26, pad = 14;
  const w = g.cols * cell + 2 * pad, h = g.rows * cell + 2 * pad;
  const cx = x => pad + x * cell + cell / 2;
  const cy = y => pad + y * cell + (showIndex ? cell * 0.68 : cell / 2);
  let svg = `<svg viewBox="0 0 ${w} ${h}" xmlns="http://www.w3.org/2000/svg">`;
  svg += '<defs><marker id="hw-arrow" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="5" markerHeight="5" orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" class="hw-arrowhead"/></marker></defs>';
  for (let y = 0; y < g.rows; y++) {
    for (let x = 0; x < g.cols; x++) {
      svg += `<rect class="hw-bin" x="${pad + x * cell + 1.5}" y="${pad + y * cell + 1.5}" width="${cell - 3}" height="${cell - 3}" rx="3"/>`;
    }
  }
  order.forEach((line, i) => {
    const pts = line.map(b => `${cx(b.x)},${cy(b.y)}`).join(' ');
    svg += `<polyline class="hw-path${i ? ' b' : ''}" points="${pts}" marker-end="url(#hw-arrow)"/>`;
    if (showIndex) {
      line.forEach((b, k) => {
        svg += `<text class="hw-index" x="${cx(b.x)}" y="${pad + b.y * cell + 10}">${k * g.pixelsPerBin}</text>`;
      });
    }
    const s = line[0];
    svg += `<circle class="hw-start${i ? ' b' : ''}" cx="${cx(s.x)}" cy="${cy(s.y)}" r="6"/>`;
    svg += `<text class="hw-start-label" x="${cx(s.x)}" y="${cy(s.y) + 3}">${i ? 'B' : 'A'}</text>`;
  });
  svg += '</svg>';
  const preview = $('#hw-preview');
  preview.innerHTML = svg;
  // Drawn at the box's width, but never smaller than 16 px a bin (a 64 x 2
  // grid would be a thin line) nor larger than 1.5 times its drawn size; a
  // drawing wider or taller than its box scrolls inside it (app.css).
  const drawing = preview.firstElementChild;
  drawing.style.minWidth = `${Math.round(w * 16 / cell)}px`;
  drawing.style.maxWidth = `${Math.round(w * 1.5)}px`;
}

function applyGrid(cfg) {
  const g = gridForm();
  Object.assign(cfg.grid, g, {
    chipset: $('#hw-chipset').value,
    colorOrder: $('#hw-order').value,
    currentLimitMa: intValue($('#hw-current')),
    pixelMa: pixelMaValue($('#hw-pixel-ma')),
    invertData: $('#hw-invert').checked,
  });
  if (g.segments !== 2) cfg.grid.splitRow = Math.min(cfg.grid.splitRow, Math.max(1, g.rows - 1));
}

// The current per pixel at full white (config pixelMa). An empty field
// stands for the LED type's typical figure, which it shows as placeholder.
function pixelMaDefault(chip) {
  return String((state.pins && state.pins.pixelMa && state.pins.pixelMa[chip]) || '');
}
function pixelMaValue(input) {
  return input.value === '' ? 0 : Math.min(255, Math.max(1, intValue(input)));
}

// The full-white test, for measuring the real current per pixel: every
// pixel of the grid or of one work strip at full white for 2 minutes, past
// the brightness and the current limit (the board stops it by itself). It
// runs on the saved settings, so the warning counts from those.
async function whiteTest(button, pixels, chip, pixelMa, send) {
  if (button.dataset.running) {
    await attempt(() => send(false));
    return whiteTestEnded(button);
  }
  const ma = pixelMa || Number(pixelMaDefault(chip)) || 0;
  if (!confirm(t('hw.whiteConfirm', { n: pixels, a: (pixels * ma / 1000).toFixed(1) }))) return;
  if (!(await attempt(() => send(true)))) return;
  button.dataset.running = '1';
  button.textContent = t('hw.whiteStop');
  button.whiteTimer = setTimeout(() => whiteTestEnded(button), 120000);
}
function whiteTestEnded(button) {
  clearTimeout(button.whiteTimer);
  delete button.dataset.running;
  button.textContent = t('hw.whiteTest');
}
function gridWhiteTest() {
  const g = state.config && state.config.grid;
  if (!g) return;
  whiteTest($('#hw-grid-white'), g.cols * g.rows * g.pixelsPerBin, g.chipset, g.pixelMa,
    on => api('/api/led/test', { form: { pattern: on ? 'white' : 'off' } }));
}
function lightWhiteTest(channel, button) {
  const l = state.config && state.config.lights && state.config.lights[channel];
  if (!l) return;
  whiteTest(button, l.pixels, l.chipset, l.pixelMa,
    on => api('/api/lights/test', { form: { channel, on: on ? 1 : 0 } }));
}

// Writes the whole configuration back and waits for the board to restart.
async function saveAndRestart(cfg) {
  if (!confirm(t('hw.confirm'))) return;
  if (await attempt(() => api('/api/config', { json: JSON.stringify(cfg) }))) waitForRestart();
}

// Work lights: one block per channel. W1-W3 can be white, addressable or,
// all three together, one analog RGB strip; W4-W5 white only.
function renderLightFields() {
  const lights = (state.config && state.config.lights) || [];
  const rgb = lights.length >= 3 && lights.slice(0, 3).every(l => l.mode === 'rgb');
  $('#hw-rgb').checked = rgb;
  const list = $('#hw-light-list');
  list.replaceChildren();
  for (let i = 0; i < LIGHT_CHANNELS; i++) {
    const l = lights[i] || { mode: 'off', maxBrightness: 255, pixels: 60, chipset: 'WS2812B', colorOrder: 'GRB', currentLimitMa: 2000 };
    const box = document.createElement('div');
    box.className = 'hw-light';
    box.dataset.channel = i;
    const modes = i < 3 ? ['off', 'white', 'addressable'] : ['off', 'white'];
    box.innerHTML = `
      <div class="hw-light-head"><b>W${i + 1}</b>
        <select class="hw-mode" aria-label="W${i + 1}"></select>
        <span class="muted small hw-rgb-note" hidden>${t('hw.mode.rgb')}</span></div>
      <div class="hw-fields hw-light-fields">
        <label class="hw-max"><span class="label-long">${t('hw.maxBrightness')}</span><span class="label-short">${t('hw.maxBrightnessShort')}</span><input type="number" class="hw-maxb" min="1" max="100" inputmode="numeric"></label>
        <label class="hw-addr"><span>${t('hw.pixels')}</span><input type="number" class="hw-pixels" min="1" max="1000" inputmode="numeric"></label>
      </div>
      <div class="hw-fields hw-light-fields hw-addr">
        <label class="hw-addr"><span>${t('hw.chipset')}</span><select class="hw-chip"></select></label>
        <label class="hw-addr"><span>${t('hw.order')}</span><select class="hw-ord"></select></label>
        <label class="hw-addr"><span class="label-long">${t('hw.current')}</span><span class="label-short">${t('hw.currentShort')}</span><input type="number" class="hw-cur" min="100" max="30000" step="100" inputmode="numeric"></label>
        <label class="hw-addr"><span class="label-long">${t('hw.pixelMa')}</span><span class="label-short">${t('hw.pixelMaShort')}</span><input type="number" class="hw-pma" min="1" max="255" inputmode="numeric"></label>
      </div>
      <label class="check hw-addr"><input type="checkbox" class="hw-inv"><span>${t('hw.invert')}</span></label>
      <div class="buttons hw-addr"><button type="button" class="secondary hw-white">${t('hw.whiteTest')}</button></div>
      <p class="muted small hw-addr">${t('hw.whiteHint')}</p>`;
    fillSelect($('.hw-mode', box), modes, modes.includes(l.mode) ? l.mode : (l.mode === 'rgb' ? 'white' : 'off'),
      m => t(`hw.mode.${m}`));
    $('.hw-maxb', box).value = Math.max(1, Math.round(l.maxBrightness * 100 / 255));
    $('.hw-pixels', box).value = l.pixels;
    fillSelect($('.hw-chip', box), CHIPSETS, l.chipset);
    fillSelect($('.hw-ord', box), COLOR_ORDERS, l.colorOrder);
    $('.hw-cur', box).value = l.currentLimitMa;
    $('.hw-pma', box).value = l.pixelMa || '';
    $('.hw-pma', box).placeholder = pixelMaDefault(l.chipset);
    $('.hw-chip', box).addEventListener('change', e => { $('.hw-pma', box).placeholder = pixelMaDefault(e.target.value); });
    $('.hw-white', box).addEventListener('click', e => lightWhiteTest(i, e.currentTarget));
    $('.hw-inv', box).checked = !!l.invertData;
    $('.hw-mode', box).addEventListener('change', updateLightFields);
    list.append(box);
  }
  updateLightFields();
}

// Shows each channel's fields for its mode; with the RGB group on, W1-W3
// share W1's maximum brightness and have no mode of their own.
function updateLightFields() {
  const rgb = $('#hw-rgb').checked;
  for (const box of $$('#hw-light-list .hw-light')) {
    const i = Number(box.dataset.channel);
    const grouped = rgb && i < 3;
    const mode = grouped ? 'rgb' : $('.hw-mode', box).value;
    $('.hw-mode', box).hidden = grouped;
    $('.hw-rgb-note', box).hidden = !grouped;
    // Max brightness only for plain channels: an addressable strip is capped
    // by its current limit (see LightConfig in AppConfig.h).
    $('.hw-max', box).hidden = mode === 'off' || mode === 'addressable' || (grouped && i > 0);
    for (const el of $$('.hw-addr', box)) el.hidden = mode !== 'addressable';
  }
}

function applyLights(cfg) {
  const rgb = $('#hw-rgb').checked;
  cfg.lights = $$('#hw-light-list .hw-light').map(box => {
    const i = Number(box.dataset.channel);
    const old = (cfg.lights && cfg.lights[i]) || {};
    const pct = Math.min(100, Math.max(1, intValue($('.hw-maxb', box))));
    return {
      ...old,
      mode: rgb && i < 3 ? 'rgb' : $('.hw-mode', box).value,
      maxBrightness: Math.max(1, Math.round(pct * 255 / 100)),
      pixels: intValue($('.hw-pixels', box)),
      chipset: $('.hw-chip', box).value,
      colorOrder: $('.hw-ord', box).value,
      currentLimitMa: intValue($('.hw-cur', box)),
      pixelMa: pixelMaValue($('.hw-pma', box)),
      invertData: $('.hw-inv', box).checked,
    };
  });
}

// Pin map: one choice per function, offering only the GPIOs the board
// allows for it (outputs can't use the input-only GPIO34-39; the analog
// microphone needs ADC1, GPIO32-39). The BOOT button is shown but fixed.
async function loadPins() {
  const pins = await attempt(() => api('/api/pins'));
  if (pins) state.pins = pins;
}

function renderPinFields() {
  const info = state.pins;
  const list = $('#hw-pin-list');
  $('#hw-mic').value = (state.config && state.config.microphone) || 'i2s';
  // Boards without PSRAM run no microphone (see micAvailable in /api/pins):
  // the choice is greyed out and its pins hidden, but kept as they are.
  const micOff = !!info && info.micAvailable === false;
  $('#hw-mic').disabled = micOff;
  $('#hw-mic-psram').hidden = !micOff;
  if (!info) return list.replaceChildren();
  list.replaceChildren(...info.pins.map(p => {
    const row = document.createElement('label');
    row.className = 'pin-row';
    row.dataset.pin = p.name;
    const name = document.createElement('span');
    name.textContent = t(`pin.${p.name}`);
    const sel = document.createElement('select');
    sel.className = 'pin-gpio';
    sel.disabled = !!p.fixed;
    let allowed = p.input ? info.inputs : info.outputs;
    if (p.name === 'mic_analog') allowed = allowed.filter(g => g >= 32);
    if (p.fixed) allowed = [p.gpio];
    const values = p.fixed ? allowed : [-1, ...allowed];
    fillSelect(sel, values, p.gpio, g => {
      const text = g < 0 ? t('hw.pinNone') : `GPIO${g}`;
      return g === p.default ? `${text} ${t('hw.pinDefault')}` : text;
    });
    sel.dataset.default = p.default;
    sel.addEventListener('change', checkPins);
    row.append(name, sel);
    return row;
  }));
  updateMicPins();
  checkPins();
}

// The motion sensor's box is ticked: it reads the analog microphone's pin,
// which then stays in the pin list (and is saved with the pins).
function motionOn() {
  return $('#hw-motion-on').checked;
}

// Only the pins of the chosen microphone are shown (none when the board
// runs no microphone at all), and the analog one while the motion sensor uses it.
function updateMicPins() {
  const mic = $('#hw-mic').disabled ? 'none' : $('#hw-mic').value;
  for (const row of $$('#hw-pin-list .pin-row')) {
    const n = row.dataset.pin;
    if (n.startsWith('mic_i2s')) row.hidden = mic !== 'i2s';
    if (n === 'mic_analog') row.hidden = mic !== 'analog' && !motionOn();
  }
  checkPins();
}

// Marks GPIOs chosen for two functions (among the shown ones) and returns
// a message for the first clash, or ''.
function checkPins() {
  const used = new Map();
  let problem = '';
  for (const row of $$('#hw-pin-list .pin-row')) row.classList.remove('err');
  for (const row of $$('#hw-pin-list .pin-row')) {
    if (row.hidden) continue;
    const g = Number($('.pin-gpio', row).value);
    if (g < 0) continue;
    if (used.has(g)) {
      row.classList.add('err');
      used.get(g).classList.add('err');
      if (!problem) problem = t('hw.pinTwice', { gpio: g });
    } else {
      used.set(g, row);
    }
  }
  hwProblems.pins = problem;
  updateSaveBar();
  return problem;
}

function applyPins(cfg) {
  cfg.microphone = $('#hw-mic').value;
  cfg.pins = {};
  for (const row of $$('#hw-pin-list .pin-row')) {
    const sel = $('.pin-gpio', row);
    if (sel.disabled) continue;  // the BOOT button
    // A board without a microphone keeps its microphone pins as they were,
    // for the day the configuration moves to a board with PSRAM.
    if (row.hidden && $('#hw-mic').disabled) continue;
    // The motion sensor's pin is kept even with no analog microphone.
    if (row.dataset.pin === 'mic_analog' && motionOn()) {
      cfg.pins.mic_analog = Number(sel.value);
      continue;
    }
    // A hidden microphone pin is set free, so it can't clash with anything.
    cfg.pins[row.dataset.pin] = row.hidden ? -1 : Number(sel.value);
  }
}

// Motion sensor (Settings): on/off and the two delays. Its output is read on
// the analog microphone's pin, so turning it on with the analog microphone
// chosen warns that the microphone goes.
function renderMotionFields() {
  const m = (state.config && state.config.motion) || { enabled: false, onDelayS: 5, offDelayMin: 5 };
  $('#hw-motion-on').checked = !!m.enabled;
  $('#hw-motion-delay-on').value = m.onDelayS;
  $('#hw-motion-delay-off').value = m.offDelayMin;
  updateMotionFields();
}

function updateMotionFields() {
  const on = $('#hw-motion-on').checked;
  for (const el of [$('#hw-motion-delay-on'), $('#hw-motion-delay-off')]) el.disabled = !on;
  // The microphone as chosen in the form (Microphone and pins), saved or not.
  const analogMic = $('#hw-mic').value === 'analog';
  $('#hw-motion-mic').hidden = !(on && analogMic);
}

// The delays are checked only while the sensor is on: off, their fields
// are greyed out and the saved values stay.
function motionProblem() {
  if (!$('#hw-motion-on').checked) return '';
  const onS = Number($('#hw-motion-delay-on').value), offMin = Number($('#hw-motion-delay-off').value);
  return onS >= 1 && onS <= 60 && offMin >= 1 && offMin <= 240 ? '' : t('hw.motionRange');
}

function applyMotion(cfg) {
  const on = $('#hw-motion-on').checked;
  const old = cfg.motion || { onDelayS: 5, offDelayMin: 5 };
  cfg.motion = on
    ? { enabled: true, onDelayS: Math.round(Number($('#hw-motion-delay-on').value)),
      offDelayMin: Math.round(Number($('#hw-motion-delay-off').value)) }
    : { ...old, enabled: false };
  // The sensor takes the analog microphone's pin.
  if (on && cfg.microphone === 'analog') cfg.microphone = 'off';
  // A pin for the sensor: the default one if the microphone had freed it.
  if (on && cfg.pins && cfg.pins.mic_analog === -1) {
    const def = state.pins && state.pins.pins.find(p => p.name === 'mic_analog');
    if (def) cfg.pins.mic_analog = def.default;
  }
}

function defaultPins() {
  for (const sel of $$('#hw-pin-list .pin-gpio')) sel.value = sel.dataset.default;
  checkPins();
  setHwDirty(true);  // set from the page, not by a field's own event
}

// The Photos line of the System page: how many there are and where. With a
// card, photos taken before it went in stay in flash until they are moved,
// so the line counts the two places apart (inFlash).
// The card's figures are the whole card: a new card shows some use for its
// file system before any photo is on it.
function photoSummary(s) {
  const n = s.inventory.photos;
  if (!s.photos) return String(n);
  const sizes = { used: formatBytes(s.photos.used), total: formatBytes(s.photos.total) };
  const inFlash = Math.min(s.photos.inFlash ?? n, n);
  if (s.photos.storage !== 'card') {
    // Photos the database names but flash doesn't have: on a card that is
    // out or didn't start.
    if (inFlash < n) return t('system.photosMissing', { n, flash: inFlash, missing: n - inFlash });
    return t('system.photosFlash', { n, ...sizes });
  }
  if (!inFlash) return t('system.photosCard', { n, ...sizes });
  return t('system.photosSplit', { n, flash: inFlash, card: n - inFlash, ...sizes });
}

// ---------------------------------------------------------------------------
// Firmware update (System page)

function renderFirmwareCard() {
  $('#firmware-card').hidden = !(state.system && state.system.firmwareUpload);
}

function pickFirmware() {
  const file = $('#firmware-file').files[0];
  $('#firmware-name').textContent = file ? `${file.name}, ${formatBytes(file.size)}` : '';
  $('#firmware-name').classList.remove('err');
  $('#firmware-upload').disabled = !file;
}

// Checks the file looks like an ESP32 app image (it starts with the 0xE9
// magic byte) before sending 1-2 MB over Wi-Fi; the board checks the whole
// image again before it switches to it.
async function installFirmware() {
  const file = $('#firmware-file').files[0];
  if (!file) return;
  const head = new Uint8Array(await file.slice(0, 1).arrayBuffer());
  const status = $('#firmware-name');
  if (head[0] !== 0xE9 || file.size < 64 * 1024) {
    status.textContent = t('firmware.notImage');
    status.classList.add('err');
    return;
  }
  if (!confirm(t('firmware.confirm', { file: file.name }))) return;
  const bar = $('#firmware-progress');
  const button = $('#firmware-upload');
  bar.hidden = false;
  button.disabled = true;
  status.classList.remove('err');
  const form = new FormData();
  form.append('firmware', file, file.name);
  try {
    const res = await upload('/api/firmware', form, null, f => {
      bar.value = Math.round(f * 100);
      status.textContent = t('firmware.uploading', { pct: Math.round(f * 100) });
    });
    if (!res.ok) throw new Error((res.body && res.body.error) || `${res.status}`);
    status.textContent = t('firmware.installed');
    waitForRestart();
  } catch (e) {
    status.textContent = t('firmware.failed', { error: e.message });
    status.classList.add('err');
    button.disabled = false;
  } finally {
    bar.hidden = true;
  }
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

async function enterSystem() {
  await Promise.all([loadSystem(), loadBackupInfo()]);
}

// ---------------------------------------------------------------------------
// Backup and restore, CSV, photo storage (System page)

const backup = { info: null, poll: 0 };

async function loadBackupInfo() {
  const info = await attempt(() => api('/api/backup/info'));
  if (!info) return;
  backup.info = info;
  renderBackup();
  // A job that just ended changed what is stored (a copy saved to the card,
  // photos moved or removed): new storage figures on the System page. A
  // restore from the card refreshes it itself (see renderCardDb()).
  const job = info.job || {};
  if (backup.wasRunning && !job.running && job.kind !== 'cardRestore') loadSystem();
  backup.wasRunning = !!job.running;
  // A photo job reports its progress here; ask again while it runs.
  clearTimeout(backup.poll);
  if (info.job && info.job.running && $('#page-system').classList.contains('active')) {
    backup.poll = setTimeout(loadBackupInfo, 1000);
  }
}

function renderBackup() {
  const info = backup.info;
  if (!info) return;
  const prev = info.previousBytes !== undefined;
  $('#previous-db').hidden = !prev;
  if (prev) $('#previous-db-text').textContent = t('backup.previous', { size: formatBytes(info.previousBytes) });

  $('#photo-tools-text').textContent = t(info.card ? 'photoTools.onCard' : 'photoTools.inFlash');
  // Moving makes sense only with a card and photos still in flash.
  $('#photos-move').hidden = !(info.card && info.flashPhotos);
  const job = info.job || {};
  const running = !!job.running;
  // One job at a time (photos and the card copies share the card and the flash).
  for (const b of [$('#photos-move'), $('#photos-clean'), $('#card-db-save'), $('#delete-db')]) b.disabled = running;
  renderCardDb(info, job);
  const line = $('#photo-job');
  if (!(job.kind === 'move' || job.kind === 'clean') || (!running && !backup.jobShown)) {
    line.hidden = true;
    return;
  }
  line.hidden = false;
  line.classList.toggle('err', !running && job.failed > 0);
  if (job.kind === 'move') {
    line.textContent = running
      ? t('photoTools.moving', { done: job.done, total: job.total })
      : t('photoTools.moved', { done: job.done - job.failed, size: formatBytes(job.bytes) })
        + (job.failed ? ` ${t('photoTools.moveFailed', { n: job.failed, error: job.error || '' })}` : '');
  } else {
    line.textContent = running ? t('photoTools.cleaning')
      : t('photoTools.cleaned', { n: job.done, size: formatBytes(job.bytes) });
  }
}

// "inventory-2026-11-08-153012.db" -> "2026-11-08 15:30:12"; other names as they are.
function cardCopyDate(name) {
  const m = /^inventory-(\d{4}-\d{2}-\d{2})-(\d{2})(\d{2})(\d{2})\.db$/.exec(name);
  return m ? `${m[1]} ${m[2]}:${m[3]}:${m[4]}` : name;
}

// The SD card parts of the Backup card: Save DB to SD card beside the
// downloads, Restore from SD card beside Restore from file (enabled once a
// copy exists), and the running copy's progress under each.
function renderCardDb(info, job) {
  const card = !!info.card;
  for (const el of [$('#card-db-save'), $('#card-db-hint'), $('#card-db-restore')]) el.hidden = !card;
  const copies = info.cardBackups || [];
  $('#card-db-restore').disabled = !!job.running || !copies.length;
  $('#card-db-restore').title = copies.length ? '' : t('cardDb.none');

  const saving = job.kind === 'cardSave', restoring = job.kind === 'cardRestore';
  const saveLine = $('#card-save-status');
  saveLine.hidden = !(card && saving && (job.running || backup.cardShown));
  if (!saveLine.hidden) {
    saveLine.classList.toggle('err', !job.running && job.failed > 0);
    const pct = job.total ? Math.round(job.bytes / job.total * 100) : 0;
    saveLine.textContent = job.running ? t('cardDb.saving', { pct })
      : job.failed ? t('cardDb.failed', { error: job.error || '' })
      : t('cardDb.saved', { file: job.file || '', size: formatBytes(job.bytes) });
  }

  // A restore from the card reports where a restore from a file does.
  if (card && restoring && (job.running || backup.cardShown)) {
    const line = $('#restore-status');
    line.hidden = false;
    line.classList.toggle('err', !job.running && (job.failed > 0 || !!job.settingsError || !!job.settingsMissing));
    const pct = job.total ? Math.round(job.bytes / job.total * 100) : 0;
    if (job.running) line.textContent = t('cardDb.restoring', { file: cardCopyDate(job.file || ''), pct });
    else if (job.failed) line.textContent = t('cardDb.failed', { error: job.error || '' });
    else {
      // The count from the database as it is now (in the same answer), not
      // only the one the job noted: one number for the page to trust.
      const parts = info.parts ?? job.parts ?? 0;
      if (job.settingsMissing) line.textContent = t('cardDb.restoredNoSettings', { parts });
      else if (job.settingsError) line.textContent = t('cardDb.restoredSettingsFailed', { parts, error: job.settingsError });
      else line.textContent = t('cardDb.restored', { parts });
    }
    // Just finished: new parts, so new counts on the System page (or a
    // restart, when the settings came back too).
    if (!job.running && backup.wasRunning) {
      if (job.settings) waitForRestart();
      else refreshAfterRestore();
    }
  }
}

// Restore from SD card: a window with the saved copies (newest first and
// chosen), then a confirmation.
function openCardRestore() {
  const copies = (backup.info && backup.info.cardBackups) || [];
  if (!copies.length) return;
  $('#card-restore-list').replaceChildren(...copies.map((c, i) => {
    const row = document.createElement('label');
    const radio = document.createElement('input');
    radio.type = 'radio';
    radio.name = 'card-copy';
    radio.value = c.name;
    radio.checked = i === 0;
    const date = document.createElement('span');
    date.textContent = cardCopyDate(c.name);
    const size = document.createElement('span');
    size.className = 'muted small';
    size.textContent = c.settings === false ? `${formatBytes(c.size)}, ${t('cardDb.noSettings')}` : formatBytes(c.size);
    row.append(radio, date, size);
    return row;
  }));
  const dialog = $('#card-restore-dialog');
  dialog.returnValue = '';
  dialog.showModal();
  $('#card-restore-go').focus();
}

async function onCardRestoreClosed() {
  if ($('#card-restore-dialog').returnValue !== 'restore') return;
  const chosen = $('#card-restore-list input:checked');
  if (!chosen) return;
  const settings = $('#restore-settings').checked;
  const date = cardCopyDate(chosen.value);
  if (!confirm(t(settings ? 'cardDb.restoreConfirmSettings' : 'cardDb.restoreConfirm', { date }))) return;
  backup.cardShown = true;
  await startPhotoJob(`/api/backup/card/restore?name=${encodeURIComponent(chosen.value)}&settings=${settings ? 1 : 0}`);
}

// After a restore or Delete DB: the backup card first, then the System
// page's counts. One request at a time, as the board is short of memory
// right after a restore; a refused one is tried again a couple of times.
async function refreshAfterRestore() {
  await loadBackupInfo();
  for (let i = 0; i < 3; i++) {
    try {
      const sys = await api('/api/system');
      state.system = sys;
      renderSystem();
      return;
    } catch (e) {
      await new Promise(r => setTimeout(r, 1500));
    }
  }
}

async function startPhotoJob(path) {
  const job = await attempt(() => api(path, { method: 'POST' }));
  if (!job) return;
  backup.jobShown = true;  // show the result line once this job ends
  loadBackupInfo();
}

// Uploads with XMLHttpRequest rather than fetch(): only it reports upload
// progress, and a backup with photos (or a firmware image) can take a while
// over Wi-Fi. `body` is a File, or FormData for a multipart upload.
function upload(path, file, type, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', path);
    // No type for FormData: the browser sets multipart with its boundary.
    if (type) xhr.setRequestHeader('Content-Type', type);
    xhr.upload.onprogress = e => { if (e.lengthComputable) onProgress(e.loaded / e.total); };
    xhr.onload = () => {
      let body = null;
      try { body = JSON.parse(xhr.responseText); } catch (e) { /* not JSON */ }
      resolve({ ok: xhr.status >= 200 && xhr.status < 300, status: xhr.status, body });
    };
    xhr.onerror = () => reject(new Error(t('backup.connectionLost')));
    xhr.send(file);
  });
}

async function restoreBackup(file) {
  const settings = $('#restore-settings').checked;
  if (!confirm(t(settings ? 'backup.restoreConfirmSettings' : 'backup.restoreConfirm', { file: file.name }))) return;
  const status = $('#restore-status');
  const pick = $('#restore-pick');
  backup.cardShown = false;  // this line is the file restore's now
  status.hidden = false;
  status.classList.remove('err');
  pick.classList.add('busy');
  try {
    const res = await upload(`/api/backup/restore?settings=${settings ? 1 : 0}`, file, 'application/octet-stream',
      f => { status.textContent = t('backup.uploading', { pct: Math.round(f * 100) }); });
    if (!res.ok) throw new Error((res.body && res.body.error) || `${res.status}`);
    const r = res.body;
    // The board leaves `parts` out when it couldn't count them (short of
    // memory right after the swap); the line then gives the photo files only.
    let text = r.parts === undefined
      ? t('backup.restoredPhotosOnly', { photos: r.photos })
      : t('backup.restored', { parts: r.parts, photos: r.photos });
    if (r.photosFailed) text += ` ${t('backup.photosFailed', { n: r.photosFailed, error: r.photoError || '' })}`;
    if (r.settingsError) text += ` ${t('backup.settingsFailed', { error: r.settingsError })}`;
    if (r.settingsMissing) text += ` ${t('backup.noSettings')}`;
    status.textContent = text;
    status.classList.toggle('err', !!(r.photosFailed || r.settingsError || r.settingsMissing));
    if (r.restarting) return waitForRestart();
    refreshAfterRestore();
  } catch (e) {
    status.textContent = t('backup.restoreFailed', { error: e.message });
    status.classList.add('err');
  } finally {
    pick.classList.remove('busy');
  }
}

async function saveDbToCard() {
  backup.cardShown = true;
  await startPhotoJob('/api/backup/card/save');
}

// Delete DB: a warning dialog first, with the number of parts that go.
// Cancel holds the focus, so a stray Enter keeps the database.
function deleteDb() {
  const dialog = $('#delete-db-dialog');
  const parts = backup.info && backup.info.parts !== undefined ? backup.info.parts : '?';
  $('#delete-db-text').textContent = t('deleteDb.confirmText', { n: parts });
  dialog.returnValue = '';
  dialog.showModal();
  // Cancel is the default: Enter or a stray tap keeps the database.
  $('#delete-db-cancel').focus();
}

async function onDeleteDbClosed() {
  if ($('#delete-db-dialog').returnValue !== 'delete') return;
  if (!await attempt(() => api('/api/backup/delete-db', { method: 'POST' }))) return;
  toast(t('deleteDb.done'));
  refreshAfterRestore();
}

async function deletePreviousDb() {
  if (!confirm(t('backup.previousDeleteConfirm'))) return;
  if (await attempt(() => api('/api/backup/previous/delete', { method: 'POST' }))) loadBackupInfo();
}

// Export CSV: a window asks for the separator, with the one this browser
// chose last already picked (semicolon the first time: what Excel expects in
// most European settings, where the comma is the decimal mark). The import
// reads either, so nothing else needs to know.
function openCsvExport() {
  let sep = null;
  try { sep = localStorage.getItem('csvSep'); } catch (e) { /* storage blocked */ }
  if (sep !== ';' && sep !== ',') sep = ';';
  for (const r of document.querySelectorAll('input[name="csv-sep"]')) r.checked = r.value === sep;
  const dialog = $('#csv-export-dialog');
  dialog.returnValue = '';
  dialog.showModal();
  $('#csv-export-go').focus();
}

function onCsvExportClosed() {
  if ($('#csv-export-dialog').returnValue !== 'export') return;
  const chosen = document.querySelector('input[name="csv-sep"]:checked');
  const sep = chosen ? chosen.value : ';';
  try { localStorage.setItem('csvSep', sep); } catch (e) { /* storage blocked */ }
  // A link with "download" starts the file download without leaving the page.
  const link = document.createElement('a');
  link.href = `/api/parts.csv?sep=${encodeURIComponent(sep)}`;
  link.download = '';
  document.body.append(link);
  link.click();
  link.remove();
}

async function importCsv(file) {
  const out = $('#csv-result');
  const pick = $('#csv-pick');
  out.hidden = false;
  out.classList.remove('err');
  out.textContent = t('csv.importing');
  pick.classList.add('busy');
  try {
    const res = await upload('/api/parts/import', file, 'text/csv', f => {
      out.textContent = `${t('csv.importing')} ${Math.round(f * 100)}%`;
    });
    const r = res.body;
    if (!r) throw new Error(`${res.status}`);
    const parts = [t('csv.imported', { added: r.added, updated: r.updated, unchanged: r.unchanged || 0 })];
    if (r.failed) parts.push(t('csv.skipped', { n: r.failed }));
    if (r.error) parts.push(t('csv.stopped', { error: r.error }));
    out.replaceChildren(parts.join(' '));
    if (r.errors && r.errors.length) {
      const ul = document.createElement('ul');
      for (const e of r.errors) {
        const li = document.createElement('li');
        li.textContent = t('csv.lineError', { line: e.line, error: e.error });
        ul.append(li);
      }
      out.append(ul);
    }
    out.classList.toggle('err', !!(r.error || r.failed));
    loadSystem();
  } catch (e) {
    out.textContent = t('csv.failed', { error: e.message });
    out.classList.add('err');
  } finally {
    pick.classList.remove('busy');
  }
}

function formatBytes(n) {
  if (n >= 1048576) return `${(n / 1048576).toFixed(1)} MB`;
  if (n >= 1024) return `${(n / 1024).toFixed(0)} KB`;
  return `${n} B`;
}

function formatUptime(s) {
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600) % 24, m = Math.floor(s / 60) % 60;
  return d ? `${d} d ${h} h ${m} min` : h ? `${h} h ${m} min` : `${m} min ${s % 60} s`;
}

function renderSystem() {
  const s = state.system;
  if (!s) return;
  renderFirmwareCard();
  const rows = [
    ['system.firmware', s.firmware],
    ['system.device', s.device],
    ['system.chip', `${s.chip}, ${s.cpuMhz} MHz`],
    ['system.flash', formatBytes(s.flashBytes)],
    ['system.uptime', formatUptime(s.uptimeS)],
    // Why the board last restarted; a crash, watchdog, hang or brown-out is
    // highlighted (see src/Health.h).
    ...(s.restart ? [['system.lastRestart', t(`restart.${s.restart}`), s.restartFault]] : []),
    ['system.heap', `${formatBytes(s.heapFree)} (${t('system.minimum')} ${formatBytes(s.heapMin)})`],
    // Only on a board with PSRAM.
    ...(s.psramTotal ? [['system.psram', `${formatBytes(s.psramFree)} / ${formatBytes(s.psramTotal)}`]] : []),
    ['system.storage', `${formatBytes(s.fsUsed)} / ${formatBytes(s.fsTotal)}`],
    // Only with the LED supply under PS-ON control.
    ...(s.supply ? [['system.supply', t(!s.supply.on ? 'system.supplyOff' : s.supply.ready ? 'system.supplyOn' : 'system.supplyStarting')]] : []),
    // Inventory figures; missing when the database couldn't be opened.
    ...(s.inventory && s.inventory.parts !== undefined ? [
      ['system.parts', t('system.partsValue', { parts: s.inventory.parts, pieces: s.inventory.quantity })],
      ['system.photos', photoSummary(s)],
      ['system.bins', t('system.binsValue', { used: s.inventory.binsUsed, total: s.inventory.binsTotal })],
      ['system.categories', String(s.inventory.categories)],
      ['system.manufacturers', String(s.inventory.manufacturers)],
      ['system.database', formatBytes(s.inventory.dbBytes)],
    ] : []),
    ['system.network', s.wifi.ssid ? `${s.wifi.ssid}, ${s.wifi.rssi} dBm` : '-'],
    ['system.address', `${s.wifi.ip} (${s.wifi.host})`],
  ];
  $('#system-info').replaceChildren(...rows.flatMap(([k, v, warn]) => {
    const dt = document.createElement('dt');
    dt.textContent = t(k);
    const dd = document.createElement('dd');
    dd.textContent = v;
    if (warn) dd.className = 'err';
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

// Another page (or the API) changed a grid setting: how a bin lights up,
// the bin brightness or the running effect's settings. The grid status over
// the WebSocket carries a counter of such changes (settingsVersion); when it
// moves, the settings are fetched again. A change of this page's own moves
// it too, which only fetches what is already shown. Skipped while a save of
// ours is unanswered or the color slider is about to save: their answers
// are newer than what the fetch could bring.
async function refreshGridSettings() {
  if (locateSavesOpen === 0 && !locateHueTimer) {
    const seq = locateSaves;
    const d = await attempt(() => api('/api/locate/defaults'));
    if (d && seq === locateSaves && locateSavesOpen === 0 && !locateHueTimer) showLocateDefaults(d);
  }
  if (effectSavesOpen === 0) {
    const seq = effectSaves;
    const e = await attempt(() => api('/api/effects'));
    if (e && seq === effectSaves && effectSavesOpen === 0) showEffects(e);
  }
}

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
    state.ledSettings = undefined;
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
      // Older than a status the page has shown (see isStale()): built before
      // a change of ours, which its answer showed already.
      const old = msg.version !== undefined && msg.version < state.versions.leds;
      updateLitBins(msg);
      // The first status after connecting only sets the mark: the page
      // loaded the settings itself (enterGrid()).
      if (msg.settingsVersion !== undefined) {
        const changed = state.ledSettings !== undefined && msg.settingsVersion !== state.ledSettings;
        state.ledSettings = msg.settingsVersion;
        if (changed && state.config) refreshGridSettings();
      }
      // Not while an effect change of ours is on its way, nor from an old
      // status: either would show the previous effect for a moment (the
      // chip jumps back, then forward again when the answer arrives).
      if (msg.effect !== undefined && !old && effectSavesOpen === 0) {
        showEffectRunning(msg.effect, msg.effectRemainingS);
        // Another effect now (its timer ran out, another page or the API
        // started one): fetch its settings.
        // Not while a save of ours is on its way: its answer is newer, and
        // this list could still show the settings from before it. A save
        // started while the list is loading makes it outdated as well.
        if (msg.effect !== state.effect && state.effect !== undefined && effectSavesOpen === 0) {
          state.effect = msg.effect;
          const seq = effectSaves;
          attempt(() => api('/api/effects')).then(e => { if (e && seq === effectSaves) showEffects(e); });
        }
      }
      $('#grid-status').textContent = t('grid.status', { ma: msg.estimateMa, fps: msg.fps })
        + (msg.currentLimited ? ` ${t('grid.limited')}` : '');
    }
  };
}

// ---------------------------------------------------------------------------
// Start-up

function bindControls() {
  bindInventory();
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
  // "Turn back on" in the motion sensor's notices: the lights and the effect
  // come back until someone has come and gone.
  for (const b of document.querySelectorAll('[data-master-on]')) {
    b.addEventListener('click', async () => {
      if (await attempt(() => api('/api/lights/master-on', { method: 'POST' }))) {
        // The board applies it on its next pass; ask again a moment later.
        setTimeout(loadLights, 300);
      }
    });
  }
  $('#lights-off').addEventListener('click', async () => {
    const res = await attempt(() => api('/api/lights/off', { method: 'POST' }));
    if (res) updateLights(res);
  });
  $('#locate-clear').addEventListener('click', async () => {
    const res = await attempt(() => api('/api/locate/clear', { method: 'POST' }));
    if (res) updateLitBins(res);
  });
  // Locate color: the same hue slider and color presets as the lights
  // (presets that set a color temperature don't apply to a bin and are left out).
  locateHue = makeSlider($('#locate-hue'), 0, 359, 1, onLocateHue);
  // Plain sliders: a swipe that starts on one scrolls the page (see guardRange()).
  for (const input of $$('input[type=range]')) guardRange(input);
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
  for (const b of $$('#effect-pick button')) {
    b.addEventListener('click', () => {
      // Marked at once; the answer (and the push after it) confirm.
      showEffectRunning(b.dataset.effect, 0);
      saveEffects({ effect: b.dataset.effect });
    });
  }
  // Sliders send on release (each change is saved in the board's flash),
  // and show their value while they move.
  for (const name of ['speed', 'intensity', 'brightness']) {
    const input = $(`#effect-${name}`);
    input.addEventListener('input', () => { $(`#effect-${name}-out`).textContent = percent(input.value); });
    input.addEventListener('change', () => saveEffects({ [name]: input.value }));
  }
  $('#effect-palette').addEventListener('change', e => saveEffects({ palette: e.target.value }));
  $('#effect-timer').addEventListener('change', e => saveEffects({ timer: e.target.value }));
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
  // Hardware sections: the preview follows every change of the grid fields.
  for (const id of ['hw-cols', 'hw-rows', 'hw-ppb', 'hw-layout', 'hw-start', 'hw-segments', 'hw-split']) {
    $(`#${id}`).addEventListener('input', renderGridPreview);
  }
  $('#hw-grid-white').addEventListener('click', gridWhiteTest);
  $('#hw-chipset').addEventListener('change', e => { $('#hw-pixel-ma').placeholder = pixelMaDefault(e.target.value); });
  $('#hw-rgb').addEventListener('change', updateLightFields);
  $('#hw-mic').addEventListener('change', () => { updateMicPins(); updateMotionFields(); });
  // The sensor's pin shows in the pin list while its box is ticked.
  $('#hw-motion-on').addEventListener('change', () => { updateMotionFields(); updateMicPins(); });
  // Any change in a hardware section brings up the save bar (fields built
  // later, like the work lights' and the pins', are covered too: the events
  // bubble up to their section).
  for (const sec of $$('#page-settings details.hw')) {
    for (const type of ['input', 'change']) sec.addEventListener(type, () => setHwDirty(true));
  }
  $('#hw-save').addEventListener('click', saveHardware);
  $('#hw-pins-defaults').addEventListener('click', defaultPins);
  $('#firmware-file').addEventListener('change', pickFirmware);
  $('#firmware-upload').addEventListener('click', installFirmware);
  $('#restore-file').addEventListener('change', e => {
    const file = e.target.files[0];
    e.target.value = '';  // the same file can be chosen again
    if (file) restoreBackup(file);
  });
  $('#previous-db-delete').addEventListener('click', deletePreviousDb);
  $('#card-db-save').addEventListener('click', saveDbToCard);
  $('#card-db-restore').addEventListener('click', openCardRestore);
  $('#card-restore-dialog').addEventListener('close', onCardRestoreClosed);
  $('#delete-db').addEventListener('click', deleteDb);
  $('#delete-db-dialog').addEventListener('close', onDeleteDbClosed);
  setupSuggest($('#part-form'));
  $('#csv-export').addEventListener('click', openCsvExport);
  $('#csv-export-dialog').addEventListener('close', onCsvExportClosed);
  $('#csv-file').addEventListener('change', e => {
    const file = e.target.files[0];
    e.target.value = '';
    if (file) importCsv(file);
  });
  $('#photos-move').addEventListener('click', () => {
    if (confirm(t('photoTools.moveConfirm'))) startPhotoJob('/api/photos/move-to-card');
  });
  $('#photos-clean').addEventListener('click', () => {
    if (confirm(t('photoTools.cleanConfirm'))) startPhotoJob('/api/photos/clean');
  });
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
