// Wall console: athletes, media, rotation and settings for the Strength Tank LED wall.
// Plain ES modules, no build step, so the ESP32 can serve these files as they are.

import {
  LIFTS, MIN_SECONDS, MAX_SECONDS, fitWhiteboard, renderWhiteboardPage, sortAthletes,
  formatKg, parseKg, validSeconds, isRecentPr,
} from './layout.js';
import { LedView } from './ledview.js';
import { openSource, convert, clipFrames, fileNameFor, MAX_CLIP_SECONDS } from './media.js';
import { estimateSupplyAmps, slideBrightness, SUPPLY_BUDGET_A, PANEL_FULL_WHITE_A } from './power.js';
import { renderStorySlide, renderPostsSlide, STORY_W, STORY_H, POST_SIZE } from './igslides.js';

const MAX_UPLOAD_BYTES = 8 * 1024 * 1024;
const DEFAULT_WB_SECONDS = 8;
const DEFAULT_MEDIA_SECONDS = 10;
const DEFAULT_STORY_SECONDS = 6;
const DEFAULT_POSTS_SECONDS = 10;
const LIFT_NAMES = { snatch: 'Snatch', clean_jerk: 'Clean & jerk', total: 'Total', back_squat: 'Back squat', front_squat: 'Front squat' };
const LIFT_SHORT = { snatch: 'Snatch', clean_jerk: 'C&J', total: 'Total', back_squat: 'Back sq.', front_squat: 'Front sq.' };

// ---------------------------------------------------------------- helpers

const $ = (sel) => document.querySelector(sel);
const clone = (v) => JSON.parse(JSON.stringify(v));
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

function el(tag, props = {}, ...children) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v === undefined || v === null) continue;
    if (k === 'class') n.className = v;
    else if (k === 'dataset') Object.assign(n.dataset, v);
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else if (k in n) n[k] = v;
    else n.setAttribute(k, v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) n.append(c);
  return n;
}

// crypto.randomUUID needs a secure context; the board serves plain http.
function newId(prefix) {
  return prefix + Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function isoDate(d) {
  return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
}

// Today on the wall (its own time zone), from the last /api/status. The phone's date is
// only a fallback while the wall's clock isn't set.
function wallToday() {
  const d = state.status?.local_date;
  const m = typeof d === 'string' && d.match(/^(\d{4})-(\d{2})-(\d{2})$/);
  return m ? new Date(Number(m[1]), Number(m[2]) - 1, Number(m[3])) : new Date();
}

function kb(bytes) {
  return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

function duration(sec) {
  if (sec < 60) return `${sec} s`;
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  return s ? `${m} min ${s} s` : `${m} min`;
}

let toastTimer;
function toast(msg, bad = false) {
  const t = $('#toast');
  t.textContent = msg;
  t.className = `show${bad ? ' bad' : ''}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (t.className = ''), bad ? 6000 : 3000);
}

function debounce(fn, ms) {
  let t;
  return (...args) => {
    clearTimeout(t);
    t = setTimeout(() => fn(...args), ms);
  };
}

// ---------------------------------------------------------------- API

const PIN_KEY = 'wall_admin_pin';
let pinCache = null;

function storedPin() {
  if (pinCache === null) {
    try {
      pinCache = sessionStorage.getItem(PIN_KEY) || '';
    } catch {
      pinCache = '';
    }
  }
  return pinCache;
}

function rememberPin(pin) {
  pinCache = pin;
  try {
    sessionStorage.setItem(PIN_KEY, pin);
  } catch {
    /* private mode: the PIN lasts for this page only */
  }
}

// Resolves to the typed PIN, or null if the coach cancels (Cancel, Esc).
// Enter in the box submits: the only submit button is Save, so Enter can't hit Cancel.
let pinPending = null;
function askPin(error = '') {
  if (pinPending) return pinPending; // several saves at once share one prompt
  const dialog = $('#pin-dialog');
  const input = $('#pin-input');
  const form = $('#pin-form');
  input.value = '';
  $('#pin-error').hidden = !error;
  $('#pin-error').textContent = error;
  pinPending = new Promise((resolve) => {
    let ok = false;
    const onSubmit = (e) => {
      if (!input.value.trim()) {
        e.preventDefault();
        $('#pin-error').hidden = false;
        $('#pin-error').textContent = 'Type the PIN first.';
        input.focus();
        return;
      }
      ok = true;
    };
    form.addEventListener('submit', onSubmit);
    dialog.addEventListener(
      'close',
      () => {
        form.removeEventListener('submit', onSubmit);
        pinPending = null;
        resolve(ok ? input.value.trim() : null);
      },
      { once: true },
    );
    dialog.showModal();
    input.focus();
  });
  return pinPending;
}

function xhr(method, url, headers, body, onProgress) {
  return new Promise((resolve, reject) => {
    const r = new XMLHttpRequest();
    r.open(method, url);
    for (const [k, v] of Object.entries(headers)) r.setRequestHeader(k, v);
    if (onProgress) r.upload.onprogress = (e) => e.lengthComputable && onProgress(e.loaded / e.total);
    r.timeout = 180000;
    r.onload = () => resolve({ status: r.status, text: r.responseText });
    r.onerror = () => {
      setOnline(false);
      reject(new Error("Can't reach the wall. Check this phone is on the gym Wi-Fi and the wall is switched on."));
    };
    r.ontimeout = () => {
      setOnline(false);
      reject(new Error('The wall took too long to answer. Check it is switched on, then try again.'));
    };
    r.send(body ?? null);
  });
}

// An error from the wall, with its machine-readable code ("stale", "busy", "locked" ...).
class WallError extends Error {
  constructor(message, status, data) {
    super(message);
    this.status = status;
    this.code = data?.code ?? null;
    this.data = data;
  }
}

// Lockout after too many wrong PINs: a banner with a countdown, and no PIN prompts until it ends.
let lockUntil = 0;
let lockTimer = 0;
function showLock(seconds) {
  lockUntil = Date.now() + seconds * 1000;
  const tick = () => {
    const left = Math.ceil((lockUntil - Date.now()) / 1000);
    if (left <= 0) {
      $('#lock').hidden = true;
      clearInterval(lockTimer);
      return;
    }
    $('#lock-msg').textContent = `Too many wrong PINs. Saving is locked on the wall for ${waitText(left)}. Your changes are still on this page.`;
    $('#lock').hidden = false;
  };
  clearInterval(lockTimer);
  lockTimer = setInterval(tick, 1000);
  tick();
}
function waitText(sec) {
  return sec < 60 ? `${sec} s` : `${Math.floor(sec / 60)} min ${String(sec % 60).padStart(2, '0')} s`;
}

async function request(method, path, { json, body, headers = {}, onUploadProgress } = {}) {
  for (let attempt = 0; ; attempt++) {
    const h = { ...headers };
    if (storedPin()) h['X-Admin-Pin'] = storedPin();
    let payload = body;
    if (json !== undefined) {
      h['Content-Type'] = 'application/json';
      payload = JSON.stringify(json);
    }
    const res = await xhr(method, path, h, payload, onUploadProgress);
    setOnline(true);
    let data = null;
    try {
      data = JSON.parse(res.text);
    } catch {
      /* not JSON */
    }
    if (res.status === 429) {
      const wait = Number(data?.retry_after_s) || 30;
      showLock(wait);
      throw new WallError(`Too many wrong PINs. Try again in ${waitText(wait)}.`, 429, data);
    }
    if (res.status === 403 && data?.code === 'pin_not_set') {
      showSetup();
      throw new WallError(data.error || 'Set an admin PIN first.', 403, data);
    }
    if (res.status === 401) {
      const hadPin = Boolean(storedPin());
      rememberPin('');
      if (attempt >= 3) throw new WallError('That PIN is not right. Ask whoever set up the wall for the PIN.', 401, data);
      const pin = await askPin(hadPin ? "That PIN didn't work. Try again." : '');
      if (!pin) throw new WallError('Not saved: the admin PIN is needed. Your changes are still here.', 401, data);
      rememberPin(pin);
      continue;
    }
    if (res.status < 200 || res.status >= 300) throw new WallError(data?.error || `The wall answered ${res.status}.`, res.status, data);
    return data;
  }
}

// Banner shown while the wall can't be reached. Unsaved edits stay in the page.
let loaded = false;
function setOnline(ok) {
  const banner = $('#conn');
  if (ok) {
    banner.hidden = true;
    return;
  }
  $('#conn-msg').textContent = loaded
    ? "Can't reach the wall. Your unsaved changes are kept on this page; save again once it's back."
    : "Can't reach the wall, so nothing could be loaded. Check this phone is on the gym Wi-Fi and the wall is switched on.";
  banner.hidden = false;
}

// ---------------------------------------------------------------- state

const state = {
  athletes: [],
  athletesSaved: [],
  athletesRev: 0, // the wall's revision of the list we loaded; sent back on save
  slides: [],
  slidesSaved: [],
  slidesRev: 0,
  status: null, // last GET /api/status
  settings: {},
  settingsSaved: {},
  media: { files: [], storage: { used_bytes: 0, total_bytes: 1, usable_bytes: 1 } },
  instagram: { enabled: false, stories: [], posts: [], username: 'thestrengthtank' },
  version: 0, // bumped on every edit that changes what the wall shows
};

// Text typed into a lift box that isn't a valid weight yet, keyed "id:lift".
const rawEntries = new Map();

function changed() {
  state.version++;
  scheduleFit();
  updateDirty();
}

function athletesDirty() {
  return rawEntries.size > 0 || !same(state.athletes, state.athletesSaved);
}
function rotationDirty() {
  return !same(state.slides, state.slidesSaved);
}
function settingsDirty() {
  return !same(state.settings, state.settingsSaved);
}

// What each save bar says when nothing is waiting. Set after a save, so the coach can
// see it went through after the toast has gone.
const savedNote = { ath: null, rot: null, set: null };

function changedAthleteCount() {
  const before = new Map(state.athletesSaved.map((a) => [a.id, a]));
  const now = new Set(state.athletes.map((a) => a.id));
  let n = state.athletesSaved.filter((a) => !now.has(a.id)).length;
  for (const a of state.athletes) if (!same(a, before.get(a.id))) n++;
  return n;
}

function setBar(key, dirty, dirtyText, tab) {
  const bar = $(`#${key}-bar`);
  $(`#${key}-save`).disabled = !dirty;
  $(`#${key}-revert`).disabled = !dirty;
  bar.classList.toggle('dirty', dirty);
  const state_ = $(`#${key}-state`);
  if (dirty) {
    if (state_.dataset.mode !== 'dirty' || state_.textContent !== dirtyText) {
      state_.className = 'save-state';
      state_.replaceChildren(dirtyText);
    }
    state_.dataset.mode = 'dirty';
  } else if (state_.dataset.mode !== 'clean') {
    state_.className = 'save-state';
    state_.replaceChildren(...(savedNote[key] ?? ['All changes saved']));
    state_.dataset.mode = 'clean';
  }
  const link = document.querySelector(`#tabs a[href="#${tab}"]`);
  link.classList.toggle('dirty', dirty);
  link.querySelector('.sr-only').textContent = dirty ? ' (unsaved changes)' : '';
}

function updateDirty() {
  const n = athletesDirty() ? Math.max(1, changedAthleteCount()) : 0;
  setBar('ath', n > 0, `Not saved yet: ${n} ${n === 1 ? 'athlete' : 'athletes'} changed`, 'athletes');
  setBar('rot', rotationDirty(), 'Not saved yet', 'rotation');
  setBar('set', settingsDirty(), 'Not saved yet', 'settings');
}

// A save that failed: say so in the bar, where it stays until the next edit or save.
function barError(key, message, ...extra) {
  const st = $(`#${key}-state`);
  st.className = 'save-state bad';
  st.dataset.mode = 'error';
  st.replaceChildren(/^Not saved/.test(message) ? message : `Not saved: ${message}`, ...extra);
}

// Someone else saved the same list first (409 "stale"). The coach's edits stay on screen;
// "Load the latest" swaps them for what's on the wall, after asking.
function staleBar(key, what, reload) {
  barError(
    key,
    'someone else saved first. Your edits are still here. ',
    el('button', {
      type: 'button',
      class: 'small',
      onclick: async () => {
        if (!confirm(`Load the latest ${what} from the wall? Your unsaved changes here will be lost.`)) return;
        try {
          await reload();
          toast(`Loaded the latest ${what}.`);
        } catch (e) {
          toast(e.message, true);
        }
      },
    }, 'Load the latest'),
  );
}

function savedAt() {
  return new Date().toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
}

let fitCache = { version: -1, fit: null };
function currentFit() {
  if (fitCache.version !== state.version) {
    const named = state.athletes.filter((a) => a.name.trim());
    fitCache = { version: state.version, fit: fitWhiteboard(sortAthletes(named, state.settings.sort)) };
  }
  return fitCache.fit;
}

function wbOptions() {
  return { title: state.settings.title ?? '', prDays: state.settings.pr_highlight_days ?? 0, today: wallToday() };
}

function whiteboardSeconds() {
  const wb = state.slides.find((s) => s.type === 'whiteboard' && s.enabled);
  return wb && validSeconds(wb.duration_s) ? wb.duration_s : null;
}

// ---------------------------------------------------------------- instagram

// Decoded tiles, keyed by file name + fetch time so a refresh reloads them.
const igTiles = new Map();

function tileKey(file) {
  return `${file}@${state.instagram.last_fetch ?? 'sample'}`;
}

function loadTiles() {
  const want = [
    ...state.instagram.stories.map((s) => [s.file, STORY_W, STORY_H]),
    ...state.instagram.posts.map((p) => [p.file, POST_SIZE, POST_SIZE]),
  ];
  for (const [file, w, h] of want) {
    const key = tileKey(file);
    if (igTiles.has(key)) continue;
    igTiles.set(key, null);
    const img = new Image();
    img.onload = () => {
      const c = el('canvas', { width: w, height: h });
      const ctx = c.getContext('2d', { willReadFrequently: true });
      ctx.drawImage(img, 0, 0, w, h);
      igTiles.set(key, ctx.getImageData(0, 0, w, h).data);
      changed();
      if ($('#rotation').classList.contains('active')) renderRotation();
    };
    img.src = `/ig/${file.split('/').map(encodeURIComponent).join('/')}`;
  }
}

const tile = (file) => igTiles.get(tileKey(file)) ?? null;

function igStoryBuffer(index, progress = 0) {
  const ig = state.instagram;
  const st = ig.stories[index];
  return renderStorySlide({
    username: ig.username,
    story: st ? { tile: tile(st.file), timestamp: st.timestamp } : null,
    index,
    count: ig.stories.length,
    progress,
  });
}

function igPostsBuffer() {
  const ig = state.instagram;
  return renderPostsSlide({ username: ig.username, posts: ig.posts.map((p) => ({ ...p, tile: tile(p.file) })) });
}

function igActive() {
  return state.instagram.enabled !== false;
}

// ---------------------------------------------------------------- tabs

const TABS = ['wall', 'athletes', 'media', 'rotation', 'settings'];
const TAB_KEY = 'wall_last_tab';

// With no #tab in the address, open the tab used last on this phone (usually Athletes).
function lastTab() {
  try {
    const t = localStorage.getItem(TAB_KEY);
    return TABS.includes(t) ? t : 'wall';
  } catch {
    return 'wall';
  }
}

function showTab() {
  const tab = TABS.includes(location.hash.slice(1)) ? location.hash.slice(1) : lastTab();
  try {
    localStorage.setItem(TAB_KEY, tab);
  } catch {
    /* private mode */
  }
  for (const t of TABS) $(`#${t}`).classList.toggle('active', t === tab);
  for (const a of document.querySelectorAll('#tabs a')) {
    a.classList.toggle('active', a.hash === `#${tab}`);
    if (a.hash === `#${tab}`) a.setAttribute('aria-current', 'page');
    else a.removeAttribute('aria-current');
  }
  if (tab === 'wall') {
    startPreview();
    refreshStatus();
  } else {
    stopPreview();
  }
  if (tab === 'athletes') renderFit();
  if (tab === 'rotation') renderRotation();
  if (tab === 'settings') renderPowerNote();
}

// ---------------------------------------------------------------- athletes

function athleteRow(a, i) {
  const tr = el('tr', { dataset: { id: a.id }, class: a.visible === false ? 'hidden-row' : '' });
  const last = state.athletes.length - 1;
  const saved = state.athletesSaved.find((x) => x.id === a.id);
  const who = () => a.name.trim() || 'new athlete';
  // Screen-reader names carry the athlete's name, because on a phone the rows are cards
  // and the column headers are not read out.
  const relabel = () => {
    for (const lift of LIFTS) liftInputs[lift].setAttribute('aria-label', `${LIFT_NAMES[lift]}, ${who()}`);
    upBtn.setAttribute('aria-label', `Move ${who()} up`);
    downBtn.setAttribute('aria-label', `Move ${who()} down`);
    showBox.setAttribute('aria-label', `Show ${who()} on the wall`);
    delBtn.setAttribute('aria-label', `Remove ${who()}`);
  };

  const name = el('input', {
    type: 'text',
    value: a.name,
    maxLength: 40,
    placeholder: 'handle or name',
    'aria-label': 'Name or handle',
    oninput: (e) => {
      a.name = e.target.value;
      e.target.classList.toggle('invalid', !a.name.trim());
      relabel();
      changed();
    },
  });
  if (!a.name.trim()) name.classList.add('invalid');

  const liftInputs = {};
  const liftCells = LIFTS.map((lift) => {
    const key = `${a.id}:${lift}`;
    const input = el('input', {
      type: 'text',
      inputMode: 'decimal',
      value: rawEntries.has(key) ? rawEntries.get(key) : formatKg(a[lift]),
      autocomplete: 'off',
      enterKeyHint: 'done',
      oninput: (e) => {
        const r = parseKg(e.target.value);
        if (r.error) {
          rawEntries.set(key, e.target.value);
          e.target.title = r.error;
        } else {
          rawEntries.delete(key);
          a[lift] = r.value;
          e.target.title = '';
        }
        e.target.classList.toggle('invalid', Boolean(r.error));
        e.target.setAttribute('aria-invalid', String(Boolean(r.error)));
        markUp(e.target, lift);
        totalWarning();
        changed();
      },
    });
    if (rawEntries.has(key)) {
      input.classList.add('invalid');
      input.setAttribute('aria-invalid', 'true');
    }
    markUp(input, lift);
    liftInputs[lift] = input;
    const prDate = a.pr_dates?.[lift];
    const dot = isRecentPr(a, lift, state.settings.pr_highlight_days, wallToday())
      ? el('span', { class: 'pr-dot', title: `New PR on ${prDate}` }, '●')
      : null;
    const label = el('span', { class: 'c-label', 'aria-hidden': 'true' }, LIFT_SHORT[lift]);
    if (lift !== 'total') return el('td', { class: 'num' }, label, input, dot);
    const sum = el(
      'button',
      {
        type: 'button',
        class: 'small',
        title: 'Fill in snatch + clean & jerk',
        'aria-label': 'Fill total with snatch plus clean and jerk',
        onclick: () => {
          if (a.snatch == null || a.clean_jerk == null) return toast('Enter S and CJ first.', true);
          a.total = a.snatch + a.clean_jerk;
          rawEntries.delete(key);
          input.value = formatKg(a.total);
          input.classList.remove('invalid');
          markUp(input, lift);
          totalWarning();
          changed();
        },
      },
      'S+CJ',
    );
    return el('td', { class: 'num' }, label, el('span', { class: 'with-btn' }, input, sum), dot);
  });

  // Green outline: this lift is higher than the saved one, so saving records a new PR.
  function markUp(input, lift) {
    const up = Boolean(saved) && a[lift] != null && !rawEntries.has(`${a.id}:${lift}`) && (saved[lift] == null || a[lift] > saved[lift]);
    input.classList.toggle('up', up);
    if (up && !input.title) input.title = 'Higher than before: saved as a new PR';
    else if (!up && input.title.startsWith('Higher than before')) input.title = '';
  }

  function totalWarning() {
    const { snatch: s, clean_jerk: c, total: t } = a;
    const warn = t != null && s != null && c != null && t > s + c;
    liftInputs.total.classList.toggle('warn', warn);
    if (warn) liftInputs.total.title = `Higher than S + CJ (${formatKg(s + c)}). A total can't beat the best snatch plus the best clean & jerk.`;
  }
  totalWarning();

  const upBtn = el('button', { type: 'button', class: 'icon', title: 'Move up', disabled: i === 0, onclick: () => moveAthlete(i, -1) }, '▲');
  const downBtn = el('button', { type: 'button', class: 'icon', title: 'Move down', disabled: i === last, onclick: () => moveAthlete(i, 1) }, '▼');
  const showBox = el('input', {
    type: 'checkbox',
    checked: a.visible !== false,
    onchange: (e) => {
      a.visible = e.target.checked;
      tr.classList.toggle('hidden-row', !a.visible);
      updateAthleteSummary();
      changed();
    },
  });
  const delBtn = el('button', { type: 'button', class: 'icon', title: 'Remove', onclick: () => deleteAthlete(a.id) }, '✕');
  relabel();

  tr.append(
    el('td', { class: 'c-move' }, upBtn, downBtn),
    el('td', { class: 'c-name' }, name),
    ...liftCells,
    el(
      'td',
      { class: 'c-notes' },
      el('span', { class: 'c-label', 'aria-hidden': 'true' }, 'Notes (not shown on the wall)'),
      el('input', {
        type: 'text',
        value: a.notes ?? '',
        maxLength: 200,
        placeholder: 'Notes (not shown on the wall)',
        'aria-label': 'Notes, not shown on the wall',
        oninput: (e) => {
          a.notes = e.target.value;
          updateDirty();
        },
      }),
    ),
    el('td', { class: 'c-show' }, el('label', { class: 'inline' }, showBox, el('span', { class: 'c-label', 'aria-hidden': 'true' }, 'On the wall'))),
    el('td', { class: 'c-del' }, delBtn),
  );
  return tr;
}

function renderAthletes() {
  $('#ath-table tbody').replaceChildren(...state.athletes.map(athleteRow));
  applyFilter();
  updateAthleteSummary();
  changed();
}

function updateAthleteSummary() {
  const shown = state.athletes.filter((a) => a.visible !== false).length;
  const hidden = state.athletes.length - shown;
  $('#ath-summary').textContent = `${state.athletes.length} athletes${hidden ? `, ${hidden} hidden from the wall` : ''}`;
}

function applyFilter() {
  const raw = $('#ath-filter').value.trim();
  const q = raw.toLowerCase();
  let shown = 0;
  for (const tr of $('#ath-table tbody').children) {
    const a = state.athletes.find((x) => x.id === tr.dataset.id);
    const match = !q || a.name.toLowerCase().includes(q);
    tr.style.display = match ? '' : 'none';
    if (match) shown++;
  }
  const box = $('#ath-nomatch');
  box.hidden = !q || shown > 0;
  if (!box.hidden) {
    box.replaceChildren(
      `No athlete matches "${raw}". `,
      el('button', { type: 'button', class: 'small', onclick: () => addAthlete(raw.slice(0, 40)) }, `+ Add "${raw.slice(0, 40)}"`),
    );
  }
}

function moveAthlete(i, delta) {
  const j = i + delta;
  if (j < 0 || j >= state.athletes.length) return;
  [state.athletes[i], state.athletes[j]] = [state.athletes[j], state.athletes[i]];
  renderAthletes();
  if (state.settings.sort !== 'board') toast('The wall is sorted by ' + (state.settings.sort === 'total' ? 'total' : 'name') + '. Pick "Board order" in Settings to use this order.');
}

function deleteAthlete(id) {
  const a = state.athletes.find((x) => x.id === id);
  if (!confirm(`Remove ${a.name.trim() || 'this athlete'} from the board?`)) return;
  state.athletes = state.athletes.filter((x) => x.id !== id);
  for (const lift of LIFTS) rawEntries.delete(`${id}:${lift}`);
  renderAthletes();
}

function addAthlete(name = '') {
  const a = { id: newId('a'), name: typeof name === 'string' ? name : '', notes: '', visible: true, pr_dates: {} };
  for (const lift of LIFTS) a[lift] = null;
  state.athletes.push(a);
  $('#ath-filter').value = '';
  renderAthletes();
  const row = $(`#ath-table tbody tr[data-id="${a.id}"]`);
  row.scrollIntoView({ block: 'center' });
  // With a name already in, go straight to the first lift.
  row.querySelector(a.name ? 'td.num input' : 'input').focus();
}

function athleteErrors() {
  const errors = [];
  state.athletes.forEach((a, i) => {
    const who = a.name.trim() || `Row ${i + 1}`;
    if (!a.name.trim()) errors.push(`Row ${i + 1}: name is empty`);
    for (const lift of LIFTS) {
      const key = `${a.id}:${lift}`;
      if (rawEntries.has(key)) errors.push(`${who}: ${lift.replace('_', ' ')} "${rawEntries.get(key)}" is not a weight (0.5 kg steps)`);
    }
  });
  return errors;
}

function showErrors(box, errors) {
  box.hidden = errors.length === 0;
  box.replaceChildren(el('ul', {}, errors.slice(0, 12).map((e) => el('li', {}, e))));
}

async function saveAthletes() {
  const errors = athleteErrors();
  showErrors($('#ath-errors'), errors);
  if (errors.length) {
    // Show the hidden rows too, so the first problem can be reached.
    $('#ath-filter').value = '';
    applyFilter();
    const bad = $('#ath-table input.invalid');
    bad?.scrollIntoView({ block: 'center' });
    bad?.focus();
    barError('ath', `fix the ${errors.length === 1 ? 'box' : `${errors.length} boxes`} outlined in red`);
    return toast('Fix the boxes outlined in red, then save.', true);
  }

  // The wall stamps PR dates itself, in its own time zone; pr_dates aren't sent.
  const before = new Map(state.athletesSaved.map((a) => [a.id, a]));
  const out = state.athletes.map(({ pr_dates, ...a }) => ({ ...a, name: a.name.trim() }));
  // Point at the first athlete that changed, so "See it" opens the page they are on.
  const firstChanged = state.athletes.find((a) => !same(a, before.get(a.id)));
  const button = $('#ath-save');
  button.disabled = true;
  try {
    const res = await request('PUT', '/api/athletes', { json: { rev: state.athletesRev, athletes: out } });
    const prs = newPrs(before, res.athletes);
    applyAthletes(res);
    rawEntries.clear();
    const note = [`Saved at ${savedAt()}.`];
    if (prs.length) note.push(` New ${prs.length === 1 ? 'PR' : 'PRs'}: ${prs.slice(0, 3).join(', ')}${prs.length > 3 ? ` and ${prs.length - 3} more` : ''}.`);
    const warning = whiteboardWarning(firstChanged);
    if (warning) note.push(` ${warning}`);
    else if (firstChanged) note.push(el('button', { type: 'button', class: 'small', onclick: () => showAthleteOnPreview(firstChanged.id) }, 'See it on the preview'));
    savedNote.ath = note;
    $('#ath-state').dataset.mode = '';
    renderAthletes();
    toast(warning ? `Saved. ${warning}` : 'Saved. The wall shows it the next time the PR whiteboard comes round.', Boolean(warning));
  } catch (e) {
    if (e.code === 'stale') staleBar('ath', 'athletes', loadLatestAthletes);
    else {
      barError('ath', e.message);
      toast(e.message, true); // stale: the bar explains it and offers "Load the latest"; a toast would cover that
    }
    updateDirtyButtonsOnly();
  }
}

function applyAthletes(res) {
  state.athletes = clone(res.athletes);
  state.athletesSaved = clone(res.athletes);
  state.athletesRev = res.rev;
}

async function loadLatestAthletes() {
  applyAthletes(await request('GET', '/api/athletes'));
  rawEntries.clear();
  showErrors($('#ath-errors'), []);
  savedNote.ath = [`Loaded the latest at ${savedAt()}.`];
  $('#ath-state').dataset.mode = '';
  renderAthletes();
}

// New PRs in a save, from the wall's answer: a lift whose PR date changed, or one that went
// up and carries today's date (a second PR on the same day keeps the same date).
function newPrs(before, saved) {
  const today = state.status?.local_date ?? isoDate(new Date());
  const prs = [];
  for (const b of saved) {
    const old = before.get(b.id);
    if (!old) continue;
    for (const lift of LIFTS) {
      const d = b.pr_dates?.[lift];
      if (!d || b[lift] == null) continue;
      const up = old[lift] == null || b[lift] > old[lift];
      if (d !== old.pr_dates?.[lift] || (up && d === today)) prs.push(`${b.name} ${LIFT_NAMES[lift].toLowerCase()} ${formatKg(b[lift])} kg`);
    }
  }
  return prs;
}

// Why a saved change won't reach the wall, or null when it will.
function whiteboardWarning(athlete) {
  if (!state.slides.some((s) => s.type === 'whiteboard' && s.enabled)) return 'The PR whiteboard is off in the Rotation tab, so the wall is not showing it.';
  if (athlete && athlete.visible === false) return `${athlete.name} is hidden from the wall (the "On the wall" box is unticked).`;
  return null;
}

function updateDirtyButtonsOnly() {
  $('#ath-save').disabled = !athletesDirty();
}

// Opens the Wall tab held on the whiteboard page that has this athlete.
function showAthleteOnPreview(id) {
  location.hash = '#wall';
  const fit = currentFit();
  const page = Math.max(0, fit.pages.findIndex((p) => p.some((a) => a.id === id)));
  const i = sequence().findIndex((item) => item.kind === 'wb' && item.page === page);
  if (i >= 0) jumpTo(i);
}

// CSV import / export -------------------------------------------------------

const CSV_COLUMNS = {
  name: ['name', 'handle', 'athlete', 'instagram'],
  snatch: ['snatch', 'snatch_kg', 's', 'sn'],
  clean_jerk: ['clean_jerk', 'clean_jerk_kg', 'cj', 'c&j', 'clean and jerk'],
  total: ['total', 'total_kg', 'ttl', 'tot'],
  back_squat: ['back_squat', 'back_squat_kg', 'bs'],
  front_squat: ['front_squat', 'front_squat_kg', 'fs'],
  notes: ['notes', 'note'],
  visible: ['visible', 'show'],
};

function parseCsv(text) {
  const rows = [];
  let row = [];
  let field = '';
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quoted) {
      if (ch === '"' && text[i + 1] === '"') {
        field += '"';
        i++;
      } else if (ch === '"') quoted = false;
      else field += ch;
    } else if (ch === '"') quoted = true;
    else if (ch === ',') {
      row.push(field);
      field = '';
    } else if (ch === '\n' || ch === '\r') {
      if (ch === '\r' && text[i + 1] === '\n') i++;
      row.push(field);
      if (row.some((f) => f.trim())) rows.push(row);
      row = [];
      field = '';
    } else field += ch;
  }
  row.push(field);
  if (row.some((f) => f.trim())) rows.push(row);
  return rows;
}

function athletesFromCsv(text) {
  const rows = parseCsv(text.replace(/^﻿/, ''));
  if (rows.length < 2) throw new Error('The file has no athlete rows.');
  const header = rows[0].map((h) => h.trim().toLowerCase());
  const col = {};
  for (const [key, names] of Object.entries(CSV_COLUMNS)) col[key] = header.findIndex((h) => names.includes(h));
  if (col.name < 0) throw new Error('The first row needs a "name" column.');
  const problems = [];
  const athletes = rows.slice(1).flatMap((r) => {
    const name = (r[col.name] ?? '').trim().slice(0, 40);
    if (!name) return [];
    const a = { id: newId('a'), name, notes: col.notes >= 0 ? (r[col.notes] ?? '').trim().slice(0, 200) : '', visible: true, pr_dates: {} };
    if (col.visible >= 0) a.visible = !/^(false|no|0|hide|hidden)$/i.test((r[col.visible] ?? '').trim());
    for (const lift of LIFTS) {
      const raw = col[lift] >= 0 ? r[col[lift]] ?? '' : '';
      const p = parseKg(raw);
      a[lift] = p.error ? null : p.value;
      if (p.error) problems.push(`${name}: ${lift.replace('_', ' ')} "${raw.trim()}"`);
    }
    return [a];
  });
  return { athletes, problems };
}

function importCsv(file) {
  file.text().then((text) => {
    let parsed;
    try {
      parsed = athletesFromCsv(text);
    } catch (e) {
      return toast(e.message, true);
    }
    const bar = $('#ath-import-bar');
    const close = () => {
      bar.hidden = true;
      $('#ath-import').value = '';
    };
    const apply = (mode) => {
      if (mode === 'replace') {
        state.athletes = parsed.athletes;
      } else {
        const byName = new Map(state.athletes.map((a) => [a.name.trim().toLowerCase(), a]));
        for (const n of parsed.athletes) {
          const cur = byName.get(n.name.toLowerCase());
          if (!cur) state.athletes.push(n);
          else {
            for (const lift of LIFTS) if (n[lift] != null) cur[lift] = n[lift];
            if (n.notes) cur.notes = n.notes;
          }
        }
      }
      rawEntries.clear();
      close();
      renderAthletes();
      toast('Imported. Check the list, then press Save.');
    };
    bar.replaceChildren(
      el('span', {}, `${parsed.athletes.length} athletes in ${file.name}.`),
      parsed.problems.length
        ? el('span', { class: 'muted' }, `${parsed.problems.length} values couldn't be read and will be blank: ${parsed.problems.slice(0, 4).join('; ')}${parsed.problems.length > 4 ? '…' : ''}`)
        : null,
      el('span', { class: 'spacer' }),
      el('button', { type: 'button', onclick: () => apply('merge'), title: 'Update athletes with the same name, add the rest' }, 'Merge by name'),
      el('button', { type: 'button', onclick: () => confirm('Replace the whole list with this file?') && apply('replace') }, 'Replace the list'),
      el('button', { type: 'button', class: 'ghost', onclick: close }, 'Cancel'),
    );
    bar.hidden = false;
  });
}

function exportCsv() {
  const q = (v) => {
    const s = String(v ?? '');
    return /[",\n\r]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
  };
  const lines = [['name', ...LIFTS.map((l) => `${l}_kg`), 'notes', 'visible'].join(',')];
  for (const a of state.athletes) {
    lines.push([q(a.name), ...LIFTS.map((l) => formatKg(a[l])), q(a.notes), a.visible !== false].join(','));
  }
  const url = URL.createObjectURL(new Blob([lines.join('\n') + '\n'], { type: 'text/csv' }));
  el('a', { href: url, download: `strength-tank-athletes-${isoDate(new Date())}.csv` }).click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

// Fit preview -------------------------------------------------------------

const fitViews = [];

function renderFit() {
  if (!$('#athletes').classList.contains('active')) return;
  const fit = currentFit();
  const box = $('#fit-pages');
  while (fitViews.length < fit.pageCount) {
    const canvas = el('canvas', { class: 'led' });
    const caption = el('figcaption');
    box.append(el('figure', {}, canvas, caption));
    fitViews.push({ view: new LedView(canvas, 2), caption, figure: canvas.parentElement });
  }
  fitViews.forEach((f, i) => {
    f.figure.hidden = i >= fit.pageCount;
    if (i >= fit.pageCount) return;
    f.view.showBuffer(renderWhiteboardPage(fit, i, wbOptions()));
    f.caption.textContent = `Page ${i + 1} of ${fit.pageCount}: ${fit.pages[i].length} athletes`;
  });
  const sizes = fit.pages.map((p) => p.length).join(', ');
  const secs = whiteboardSeconds();
  const parts = [
    `${fit.count} on the wall → ${fit.pageCount} ${fit.pageCount === 1 ? 'page' : 'pages'} (${sizes})`,
    `names up to ${fit.nameChars} characters`,
  ];
  if (secs) parts.push(`${secs} s per page, ${duration(secs * fit.pageCount)} per pass`);
  if (fit.truncated.length) parts.push(`cut short: ${fit.truncated.slice(0, 3).join(', ')}${fit.truncated.length > 3 ? '…' : ''}`);
  $('#fit-summary').textContent = parts.join(' · ');
}
const scheduleFit = debounce(() => {
  renderFit();
  if ($('#rotation').classList.contains('active')) updateRotationSummary();
}, 120);

// ---------------------------------------------------------------- media

let cropView;
let source = null;
let convertCtrl = null;

function cropOptions() {
  return {
    fit: $('#crop-fit').value,
    zoom: Number($('#crop-zoom').value),
    panX: Number($('#crop-x').value),
    panY: Number($('#crop-y').value),
    start: Number($('#clip-start').value) || 0,
    end: Number($('#clip-end').value) || 0,
    fps: Number($('#clip-fps').value),
  };
}

async function pickFile(file) {
  closeSource();
  try {
    source = await openSource(file);
  } catch (e) {
    return toast(e.message, true);
  }
  $('#media-editor').hidden = false;
  $('#crop-fit').value = 'cover';
  $('#crop-zoom').value = 1;
  $('#crop-x').value = 0.5;
  $('#crop-y').value = 0.5;
  $('#media-name').value = file.name.replace(/\.[^.]+$/, '');
  $('#media-seconds').value = DEFAULT_MEDIA_SECONDS;
  const isVideo = source.kind === 'video';
  $('#video-opts').hidden = !isVideo;
  if (isVideo) {
    $('#clip-start').value = 0;
    $('#clip-start').max = source.duration.toFixed(1);
    $('#clip-end').max = source.duration.toFixed(1);
    $('#clip-end').value = Math.min(source.duration, MAX_CLIP_SECONDS).toFixed(1);
  }
  drawCrop();
  $('#media-editor').scrollIntoView({ block: 'nearest' });
}

function closeSource() {
  convertCtrl?.abort();
  source?.close?.();
  source = null;
  $('#media-editor').hidden = true;
  $('#media-progress-wrap').hidden = true;
  $('#media-file').value = '';
}

const drawCrop = debounce(async () => {
  if (!source) return;
  const o = cropOptions();
  await source.drawPreview(cropView.srcCtx, o, o.start);
  cropView.present();
  mediaInfo(o);
}, 60);

function mediaInfo(o) {
  const parts = [`${source.width}×${source.height}`];
  if (source.kind === 'video') {
    const end = Math.min(o.end || source.duration, source.duration);
    const frames = clipFrames(o.start, end, o.fps);
    parts.push(`clip ${Math.max(0, end - o.start).toFixed(1)} s of ${source.duration.toFixed(1)} s → ${frames} frames`);
    if (end - o.start > MAX_CLIP_SECONDS) parts.push(`only the first ${MAX_CLIP_SECONDS} s are used`);
  } else if (source.kind === 'gif') {
    parts.push(`animated, ${source.frames.length} frames, ${source.duration.toFixed(1)} s`);
  }
  const bright = state.settings.brightness ?? 50;
  const amps = estimateSupplyAmps(cropView.pixels(), bright);
  const capped = slideBrightness(peakOf(cropView.pixels()), bright);
  let power = `power at ${bright}% brightness ≈ ${amps.a.toFixed(1)} A / ${amps.b.toFixed(1)} A per supply`;
  if (capped < bright) power += ` (over the ${SUPPLY_BUDGET_A.toFixed(1)} A budget, so the wall would play this at ${capped}%)`;
  parts.push(power);
  $('#media-info').textContent = parts.join(' · ');
}

// The busier supply's current for one frame at 100% brightness: the upload's peak_a.
function peakOf(rgba) {
  const { a, b } = estimateSupplyAmps(rgba, 100);
  return Math.max(a, b);
}

// What the wall does with a file's brightness, in words, or '' when it plays at the setting.
function dimmingNote(peakA) {
  const bright = state.settings.brightness ?? 50;
  const capped = slideBrightness(peakA, bright);
  return capped < bright ? `plays at ${capped}% (its brightest frame would overload a supply at ${bright}%)` : '';
}

function setProgress(value, label) {
  $('#media-progress-wrap').hidden = false;
  $('#media-progress').value = value;
  $('#media-progress-label').textContent = label;
}

async function convertAndUpload() {
  if (!source) return;
  const name = $('#media-name').value.trim();
  if (!name) return toast('Give it a name first.', true);
  const add = $('#media-add').checked;
  const secs = Number($('#media-seconds').value);
  if (add && !validSeconds(secs)) return toast(`Screen time must be ${MIN_SECONDS} to ${MAX_SECONDS} seconds.`, true);
  const o = cropOptions();
  if (source.kind === 'video' && !(Math.min(o.end, source.duration) > o.start)) return toast('End must be after start.', true);

  const fileName = fileNameFor(name, new Set(state.media.files.map((f) => f.name)));
  const button = $('#media-convert');
  button.disabled = true;
  convertCtrl = new AbortController();
  try {
    const { blob, frames, peakFrame } = await convert(source, o, (p) => setProgress(p * 0.8, `Converting… ${Math.round(p * 100)}%`), convertCtrl.signal);
    if (blob.size > MAX_UPLOAD_BYTES) throw new Error(`The result is ${kb(blob.size)}, over the ${kb(MAX_UPLOAD_BYTES)} limit. Shorten the clip or lower the frame rate.`);
    const { used_bytes: used, usable_bytes: usable } = state.media.storage;
    if (used + blob.size > usable) throw new Error(`Not enough room on the board (${kb(Math.max(0, usable - used))} free, needs ${kb(blob.size)}). Delete something first.`);
    const peakA = Math.min(40, peakOf(peakFrame));
    await request('POST', `/api/media?name=${encodeURIComponent(fileName)}&frames=${frames}&peak_a=${peakA.toFixed(2)}`, {
      body: blob,
      headers: { 'Content-Type': 'image/gif' },
      onUploadProgress: (p) => setProgress(0.8 + p * 0.2, `Uploading… ${Math.round(p * 100)}%`),
    });
    await refreshMedia();
    const dim = dimmingNote(peakA);
    toast(`Uploaded ${fileName}: ${kb(blob.size)}, ${frames} ${frames === 1 ? 'frame' : 'frames'}${dim ? `. It ${dim}` : ''}.`);
    closeSource();
    if (add) {
      state.slides.push({ id: newId('s'), type: 'media', file: fileName, duration_s: secs, enabled: true });
      await saveRotation({ quiet: true }).catch((e) => toast(`Uploaded, but not added to the rotation: ${e.message}`, true));
    }
  } catch (e) {
    if (e.name !== 'AbortError') toast(`Not uploaded: ${e.message} Your crop is kept, so you can try again.`, true);
    $('#media-progress-wrap').hidden = true;
  } finally {
    button.disabled = false;
    convertCtrl = null;
  }
}

async function refreshMedia() {
  state.media = await request('GET', '/api/media');
  renderMedia();
}

function renderMedia() {
  // The meter is against the usable space: the wall keeps the last 10% of its flash free.
  const { used_bytes: used, usable_bytes: usable } = state.media.storage;
  const pct = Math.min(100, (used / usable) * 100);
  $('#storage').textContent = `${kb(used)} of ${kb(usable)} used`;
  $('#storage-bar span').style.width = `${pct}%`;
  $('#storage-bar').classList.toggle('full', pct > 90);
  const inRotation = new Set(state.slides.filter((s) => s.type === 'media').map((s) => s.file));
  const list = $('#media-list');
  if (!state.media.files.length) {
    list.replaceChildren(el('p', { class: 'muted' }, 'Nothing uploaded yet.'));
  } else {
    list.replaceChildren(
      ...state.media.files.map((f) =>
        el(
          'div',
          { class: 'media-item' },
          el('img', { src: `/media/${encodeURIComponent(f.name)}`, alt: f.name, loading: 'lazy' }),
          el('strong', {}, f.name),
          el('span', { class: 'meta' }, `${kb(f.size)} · ${f.frames ?? '?'} ${f.frames === 1 ? 'frame' : 'frames'}${f.uploaded ? ` · ${f.uploaded}` : ''}`),
          dimmingNote(f.peak_a) ? el('span', { class: 'meta' }, `Bright: ${dimmingNote(f.peak_a)}`) : null,
          el(
            'div',
            { class: 'row' },
            inRotation.has(f.name)
              ? el('span', { class: 'badge' }, 'In the rotation')
              : el('button', { type: 'button', class: 'small', onclick: () => addMediaSlide(f.name, true) }, '+ Add to rotation'),
            el('span', { class: 'spacer' }),
            el(
              'button',
              {
                type: 'button',
                class: 'small ghost',
                disabled: inRotation.has(f.name),
                title: inRotation.has(f.name) ? 'Remove it from the rotation first' : 'Delete from the board',
                onclick: () => deleteMedia(f.name),
              },
              'Delete',
            ),
          ),
        ),
      ),
    );
  }
  const select = $('#rot-add-media');
  select.replaceChildren(el('option', { value: '' }, '+ Media…'), ...state.media.files.map((f) => el('option', { value: f.name }, f.name)));
}

async function deleteMedia(name) {
  if (!confirm(`Delete ${name} from the board?`)) return;
  try {
    await request('DELETE', `/api/media/${encodeURIComponent(name)}`);
    await refreshMedia();
    toast(`Deleted ${name}.`);
  } catch (e) {
    toast(e.message, true);
  }
}

// ---------------------------------------------------------------- rotation

function bufferCanvas(buf, cls) {
  const c = el('canvas', { class: cls, width: 256, height: 96 });
  c.getContext('2d').putImageData(new ImageData(buf, 256, 96), 0, 0);
  return c;
}

function slideThumb(s, fit) {
  if (s.type === 'media') return el('img', { class: 'thumb', src: `/media/${encodeURIComponent(s.file)}`, alt: '' });
  if (s.type === 'instagram_story') return bufferCanvas(igStoryBuffer(0, 0.4), 'thumb');
  if (s.type === 'instagram_posts') return bufferCanvas(igPostsBuffer(), 'thumb');
  return bufferCanvas(renderWhiteboardPage(fit, 0, wbOptions()), 'thumb');
}

const SLIDE_TITLES = { whiteboard: 'PR whiteboard', instagram_story: 'Instagram story', instagram_posts: 'Instagram posts' };

// How many screens a slide expands to right now (whiteboard pages, current stories).
function screensFor(s, fit) {
  if (s.type === 'whiteboard') return fit.pageCount;
  if (s.type === 'instagram_story') return igActive() ? state.instagram.stories.length : 0;
  if (s.type === 'instagram_posts') return igActive() && state.instagram.posts.length ? 1 : 0;
  return 1;
}

function renderRotation() {
  const fit = currentFit();
  const files = new Map(state.media.files.map((f) => [f.name, f]));
  const last = state.slides.length - 1;
  $('#rot-list').replaceChildren(
    ...state.slides.map((s, i) => {
      const isWb = s.type === 'whiteboard';
      const isIg = s.type.startsWith('instagram');
      const sub = el('div', { class: 'sub' });
      const describe = () => {
        if (isIg) {
          const ig = state.instagram;
          const n = screensFor(s, currentFit());
          if (!igActive()) sub.textContent = 'Instagram is switched off in Settings, so this is skipped';
          else if (s.type === 'instagram_story') {
            sub.textContent = n
              ? `${n} ${n === 1 ? 'story' : 'stories'} from the last 24 h${validSeconds(s.duration_s) ? ` · ${duration(s.duration_s * n)} in all` : ''}`
              : 'No story in the last 24 h, so this is skipped for now';
          } else sub.textContent = n ? `${ig.posts.length} latest ${ig.posts.length === 1 ? 'post' : 'posts'}` : 'No posts yet, so this is skipped';
          if (ig.sample) sub.textContent += ' · sample content until Instagram is connected';
        } else if (isWb) {
          const f = currentFit();
          sub.textContent = validSeconds(s.duration_s)
            ? `${f.pageCount} ${f.pageCount === 1 ? 'page' : 'pages'} · ${duration(s.duration_s * f.pageCount)} per pass`
            : `${f.pageCount} ${f.pageCount === 1 ? 'page' : 'pages'}`;
        } else {
          const meta = files.get(s.file);
          sub.textContent = meta ? `${meta.frames ?? '?'} ${meta.frames === 1 ? 'frame (still image)' : 'frames, loops'} · ${kb(meta.size)}` : 'missing from the board';
        }
      };
      describe();
      const secs = el('input', {
        type: 'number',
        min: MIN_SECONDS,
        max: MAX_SECONDS,
        step: 1,
        inputMode: 'numeric',
        value: Number.isFinite(s.duration_s) ? s.duration_s : '',
        class: 'secs',
        'aria-label': 'Seconds on screen',
        oninput: (e) => {
          const v = Number(e.target.value);
          s.duration_s = e.target.value.trim() && Number.isInteger(v) ? v : NaN;
          const ok = validSeconds(s.duration_s);
          e.target.classList.toggle('invalid', !ok);
          e.target.title = ok ? '' : `Whole seconds, ${MIN_SECONDS} to ${MAX_SECONDS}`;
          describe();
          changed();
          updateRotationSummary();
        },
      });
      if (!validSeconds(s.duration_s)) secs.classList.add('invalid');
      const title = SLIDE_TITLES[s.type] ?? s.file;
      secs.setAttribute('aria-label', `Seconds on screen, ${title}`);
      return el(
        'li',
        { class: `slide${s.enabled ? '' : ' disabled'}` },
        el(
          'div',
          { class: 'order' },
          el('button', { type: 'button', class: 'icon', title: 'Move up', 'aria-label': `Move ${title} up`, disabled: i === 0, onclick: () => moveSlide(i, -1) }, '▲'),
          el('button', { type: 'button', class: 'icon', title: 'Move down', 'aria-label': `Move ${title} down`, disabled: i === last, onclick: () => moveSlide(i, 1) }, '▼'),
        ),
        slideThumb(s, fit),
        el('div', { class: 'info' }, el('div', { class: 'title' }, title), sub),
        el('label', { class: 'secs-wrap secs' }, secs, isWb ? 's per page' : s.type === 'instagram_story' ? 's per story' : 's'),
        el(
          'label',
          { class: 'secs-wrap on', title: 'Include in the rotation' },
          el('input', {
            type: 'checkbox',
            'aria-label': `Show ${title} in the rotation`,
            checked: s.enabled,
            onchange: (e) => {
              s.enabled = e.target.checked;
              e.target.closest('li').classList.toggle('disabled', !s.enabled);
              changed();
              updateRotationSummary();
            },
          }),
          'On',
        ),
        el('button', { type: 'button', class: 'icon del', title: 'Remove from the rotation', 'aria-label': `Remove ${title} from the rotation`, onclick: () => removeSlide(i) }, '✕'),
      );
    }),
  );
  updateRotationSummary();
  updateDirty();
}

function updateRotationSummary() {
  const fit = currentFit();
  const on = state.slides.filter((s) => s.enabled);
  const bad = state.slides.filter((s) => !validSeconds(s.duration_s)).length;
  let text;
  if (!on.length) text = 'Nothing is switched on, so the wall will be blank.';
  else {
    const total = on.reduce((sum, s) => sum + (validSeconds(s.duration_s) ? s.duration_s * screensFor(s, fit) : 0), 0);
    const screens = on.reduce((n, s) => n + screensFor(s, fit), 0);
    text = `${screens} screens · one full pass takes ${duration(total)}`;
  }
  if (bad) text += ` · ${bad} screen ${bad === 1 ? 'time needs' : 'times need'} fixing (${MIN_SECONDS} to ${MAX_SECONDS} s)`;
  $('#rot-summary').textContent = text;
}

function moveSlide(i, delta) {
  const j = i + delta;
  [state.slides[i], state.slides[j]] = [state.slides[j], state.slides[i]];
  changed();
  renderRotation();
}

function removeSlide(i) {
  state.slides.splice(i, 1);
  changed();
  renderRotation();
  renderMedia();
}

async function addMediaSlide(file, save) {
  state.slides.push({ id: newId('s'), type: 'media', file, duration_s: DEFAULT_MEDIA_SECONDS, enabled: true });
  changed();
  renderRotation();
  renderMedia();
  if (save) {
    try {
      await saveRotation({ quiet: true });
      toast(`${file} added to the rotation for ${DEFAULT_MEDIA_SECONDS} s. Change the time on the Rotation tab.`);
    } catch (e) {
      toast(`Added on the Rotation tab, but not saved yet: ${e.message}`, true);
    }
  }
}

async function saveRotation({ quiet = false } = {}) {
  const bad = state.slides.filter((s) => !validSeconds(s.duration_s));
  if (bad.length) {
    const msg = `Screen times must be whole seconds from ${MIN_SECONDS} to ${MAX_SECONDS}.`;
    if (quiet) throw new Error(msg);
    barError('rot', 'fix the times outlined in red');
    $('#rot-list input.invalid')?.focus();
    return toast(msg, true);
  }
  try {
    const res = await request('PUT', '/api/playlist', { json: { rev: state.slidesRev, slides: state.slides } });
    applySlides(res);
    savedNote.rot = [`Saved at ${savedAt()}. The wall uses the new order from its next screen.`];
    $('#rot-state').dataset.mode = '';
    changed();
    renderRotation();
    renderMedia();
    if (!quiet) toast('Rotation saved.');
  } catch (e) {
    if (e.code === 'stale') staleBar('rot', 'rotation', loadLatestSlides);
    if (quiet) throw e;
    if (e.code !== 'stale') {
      barError('rot', e.message);
      toast(e.message, true);
    }
  }
}

function applySlides(res) {
  state.slides = clone(res.slides);
  state.slidesSaved = clone(res.slides);
  state.slidesRev = res.rev;
}

async function loadLatestSlides() {
  applySlides(await request('GET', '/api/playlist'));
  savedNote.rot = [`Loaded the latest at ${savedAt()}.`];
  $('#rot-state').dataset.mode = '';
  changed();
  renderRotation();
  renderMedia();
}

// ---------------------------------------------------------------- settings

function renderSettings() {
  const s = state.settings;
  $('#set-title').value = s.title ?? '';
  $('#set-brightness').value = s.brightness ?? 50;
  $('#set-brightness-val').textContent = `${s.brightness ?? 50}%`;
  $('#set-sort').value = s.sort ?? 'board';
  $('#set-pr-days').value = s.pr_highlight_days ?? 7;
  $('#set-tz').value = s.timezone ?? 'America/Los_Angeles';
  $('#pin-state').textContent = s.pin_set
    ? 'A PIN is set. This browser remembers it until you close the tab. To change it, type a new one; the wall asks for the current PIN first.'
    : 'No PIN is set yet.';
  renderPowerNote();
  updateDirty();
}

function renderPowerNote() {
  const b = state.settings.brightness ?? 50;
  const fullWhite = 6 * PANEL_FULL_WHITE_A * (b / 100);
  const fit = currentFit();
  const wb = estimateSupplyAmps(renderWhiteboardPage(fit, 0, wbOptions()), b);
  const cap = slideBrightness(6 * PANEL_FULL_WHITE_A, 100);
  $('#set-power').textContent =
    `Estimated load per supply at ${b}%: PR whiteboard about ${Math.max(wb.a, wb.b).toFixed(1)} A, an all-white image about ${fullWhite.toFixed(1)} A. ` +
    `Each supply is rated 18 A. The wall dims a whole image or video to keep its brightest frame under ${SUPPLY_BUDGET_A.toFixed(1)} A, so all-white images top out around ${cap}%.`;
}

function renderInstagram() {
  const ig = state.instagram;
  $('#ig-enabled').checked = ig.enabled !== false;
  $('#ig-account').value = ig.account_id ?? '';
  $('#ig-minutes').value = ig.refresh_minutes ?? 15;
  $('#ig-token').value = '';
  let msg;
  if (!ig.token_set) msg = 'Not connected: the screens show sample content.';
  else if (ig.last_error) msg = `Last check failed: ${ig.last_error}`;
  else if (ig.last_fetch) msg = `Connected as @${ig.username}. Last checked ${new Date(ig.last_fetch).toLocaleString()}: ${ig.stories.length} ${ig.stories.length === 1 ? 'story' : 'stories'}, ${ig.posts.length} posts.`;
  else msg = 'Token saved. Press Refresh now to check it.';
  if (ig.busy === 'refresh') msg = 'Checking Instagram now…';
  else if (ig.busy === 'accounts') msg = 'Looking up Instagram accounts…';
  $('#ig-state').textContent = msg;
  $('#ig-clear').disabled = !ig.token_set;
  $('#ig-refresh').disabled = Boolean(ig.busy);
  $('#ig-find').disabled = Boolean(ig.busy);
}

// Instagram work runs on the wall in the background: start it, then poll until it's done.
const IG_POLL_MS = 1500;
const IG_GIVE_UP_MS = 90000;
async function runIgJob(path) {
  try {
    await request('POST', path);
  } catch (e) {
    if (e.code === 'busy') toast('The wall is already talking to Instagram. Try again in a minute.', true);
    else toast(e.message, true);
    return null;
  }
  state.instagram = { ...state.instagram, busy: path.endsWith('refresh') ? 'refresh' : 'accounts' };
  renderInstagram();
  const end = Date.now() + IG_GIVE_UP_MS;
  while (Date.now() < end) {
    await new Promise((r) => setTimeout(r, IG_POLL_MS));
    let ig;
    try {
      ig = await request('GET', '/api/instagram');
    } catch {
      continue; // a dropped poll on busy Wi-Fi: try again
    }
    if (!ig.busy) return ig;
  }
  toast("Instagram is taking longer than 90 s. The wall keeps trying; check this page again in a few minutes.", true);
  $('#ig-refresh').disabled = false;
  $('#ig-find').disabled = false;
  return null;
}

function showAccounts(accounts) {
  const box = $('#ig-accounts');
  box.hidden = false;
  box.replaceChildren(
    ...(accounts.length
      ? accounts.map((a) =>
          el('button', { type: 'button', class: 'small', onclick: () => ($('#ig-account').value = a.id) }, `@${a.username} (${a.page}): ${a.id}`),
        )
      : [el('span', { class: 'muted' }, 'This token sees no Instagram business account. Check that the account is linked to the Facebook Page.')]),
  );
}

function applyInstagram(data) {
  state.instagram = data;
  renderInstagram();
  loadTiles();
  changed();
  if ($('#rotation').classList.contains('active')) renderRotation();
}

function bindInstagram() {
  $('#ig-save').addEventListener('click', async () => {
    const minutes = Number($('#ig-minutes').value);
    if (!Number.isInteger(minutes) || minutes < 5 || minutes > 1440) return toast('Check every 5 to 1440 minutes.', true);
    const account = $('#ig-account').value.trim();
    if (!/^\d{0,32}$/.test(account)) return toast('The account ID is a number.', true);
    const body = { enabled: $('#ig-enabled').checked, account_id: account, refresh_minutes: minutes };
    const token = $('#ig-token').value.trim();
    if (token) body.access_token = token;
    try {
      applyInstagram(await request('PUT', '/api/instagram', { json: body }));
      toast('Instagram settings saved.');
    } catch (e) {
      toast(e.message, true);
    }
  });
  $('#ig-clear').addEventListener('click', async () => {
    if (!confirm('Remove the Instagram token from the wall?')) return;
    try {
      applyInstagram(await request('PUT', '/api/instagram', { json: { access_token: '' } }));
      toast('Token removed.');
    } catch (e) {
      toast(e.message, true);
    }
  });
  $('#ig-refresh').addEventListener('click', async () => {
    const ig = await runIgJob('/api/instagram/refresh');
    if (!ig) return renderInstagram();
    applyInstagram(ig);
    if (ig.last_error) toast(`Instagram check failed: ${ig.last_error}. The wall keeps showing what it had.`, true);
    else toast(`Fetched ${ig.stories.length} ${ig.stories.length === 1 ? 'story' : 'stories'} and ${ig.posts.length} posts.`);
  });
  $('#ig-find').addEventListener('click', async () => {
    const ig = await runIgJob('/api/instagram/accounts');
    if (!ig) return renderInstagram();
    state.instagram = ig;
    renderInstagram();
    if (ig.accounts_error) toast(ig.accounts_error, true);
    else showAccounts(ig.accounts ?? []);
  });
}

function bindSettings() {
  bindInstagram();
  const s = () => state.settings;
  $('#set-title').addEventListener('input', (e) => {
    s().title = e.target.value;
    changed();
  });
  $('#set-brightness').addEventListener('input', (e) => {
    s().brightness = Number(e.target.value);
    $('#set-brightness-val').textContent = `${s().brightness}%`;
    renderPowerNote();
    updateDirty();
  });
  $('#set-sort').addEventListener('change', (e) => {
    s().sort = e.target.value;
    changed();
  });
  $('#set-pr-days').addEventListener('input', (e) => {
    const v = Number(e.target.value);
    const ok = e.target.value.trim() !== '' && Number.isInteger(v) && v >= 0 && v <= 60;
    e.target.classList.toggle('invalid', !ok);
    if (ok) s().pr_highlight_days = v;
    changed();
  });
  $('#set-tz').addEventListener('change', (e) => {
    s().timezone = e.target.value;
    updateDirty();
  });
  $('#set-save').addEventListener('click', async () => {
    if ($('#set-pr-days').classList.contains('invalid')) return toast('PR highlight must be 0 to 60 days.', true);
    try {
      const { title, brightness, sort, pr_highlight_days, timezone } = state.settings;
      const res = await request('PUT', '/api/settings', { json: { title, brightness, sort, pr_highlight_days, timezone } });
      refreshStatus(true); // the wall's date may have moved with the time zone
      state.settings = clone(res);
      state.settingsSaved = clone(res);
      savedNote.set = [`Saved at ${savedAt()}.`];
      $('#set-state').dataset.mode = '';
      renderSettings();
      changed();
      toast('Settings saved.');
    } catch (e) {
      barError('set', e.message);
      toast(e.message, true);
    }
  });
  $('#set-revert').addEventListener('click', () => {
    state.settings = clone(state.settingsSaved);
    $('#set-pr-days').classList.remove('invalid');
    renderSettings();
    changed();
  });
  $('#set-pin-save').addEventListener('click', async () => {
    const problem = pinProblem($('#set-pin').value.trim(), $('#set-pin2').value.trim());
    if (problem) return toast(problem, true);
    const pin = $('#set-pin').value.trim();
    try {
      const res = await request('PUT', '/api/settings', { json: { new_pin: pin } });
      rememberPin(pin);
      $('#set-pin').value = '';
      $('#set-pin2').value = '';
      state.settings.pin_set = state.settingsSaved.pin_set = res.pin_set;
      renderSettings();
      toast('PIN changed. Use the new one from now on.');
    } catch (e) {
      toast(e.message, true);
    }
  });
}

function pinProblem(pin, again) {
  if (!/^\d{6,12}$/.test(pin)) return 'The PIN must be 6 to 12 digits.';
  if (pin !== again) return "The two PINs don't match. Type them again.";
  return null;
}

// ---------------------------------------------------------------- first run

let setupDone = null;
function showSetup() {
  document.body.classList.add('setup-mode');
  $('#setup').hidden = false;
  $('#setup-pin').focus();
  return (setupDone ??= new Promise((resolve) => {
    $('#setup-form').addEventListener('submit', async function onSubmit(e) {
      e.preventDefault();
      const err = $('#setup-error');
      const pin = $('#setup-pin').value.trim();
      const problem = pinProblem(pin, $('#setup-pin2').value.trim());
      err.hidden = !problem;
      err.textContent = problem ?? '';
      if (problem) return;
      $('#setup-save').disabled = true;
      try {
        const res = await request('PUT', '/api/settings', { json: { new_pin: pin } });
        rememberPin(pin);
        state.settings.pin_set = res.pin_set;
        if (state.settingsSaved) state.settingsSaved.pin_set = res.pin_set;
        $('#setup-form').removeEventListener('submit', onSubmit);
        $('#setup').hidden = true;
        document.body.classList.remove('setup-mode');
        setupDone = null;
        toast('PIN set. The wall is ready to edit.');
        resolve();
      } catch (ex) {
        err.hidden = false;
        err.textContent = ex.message;
      } finally {
        $('#setup-save').disabled = false;
      }
    });
  }));
}

// ---------------------------------------------------------------- wall preview

let previewView;
const preview = { index: 0, startedAt: 0, pausedAt: null, raf: 0, seqVersion: -1, seq: [], bufKey: '', lastDraw: 0 };
const mediaImages = new Map();

function sequence() {
  if (preview.seqVersion !== state.version) {
    const fit = currentFit();
    const seq = [];
    for (const s of state.slides) {
      if (!s.enabled || !validSeconds(s.duration_s)) continue;
      if (s.type === 'whiteboard') {
        for (let p = 0; p < fit.pageCount; p++) {
          seq.push({ kind: 'wb', page: p, seconds: s.duration_s, label: `PR whiteboard, page ${p + 1} of ${fit.pageCount}` });
        }
      } else if (s.type === 'instagram_story') {
        if (!igActive()) continue;
        const n = state.instagram.stories.length;
        for (let i = 0; i < n; i++) seq.push({ kind: 'story', index: i, seconds: s.duration_s, label: `Instagram story ${i + 1} of ${n}` });
      } else if (s.type === 'instagram_posts') {
        if (igActive() && state.instagram.posts.length) seq.push({ kind: 'posts', seconds: s.duration_s, label: 'Instagram posts' });
      } else seq.push({ kind: 'media', file: s.file, seconds: s.duration_s, label: s.file });
    }
    preview.seq = seq;
    preview.seqVersion = state.version;
    preview.bufKey = '';
    if (preview.index >= seq.length) preview.index = 0;
    renderStrip();
  }
  return preview.seq;
}

// One thumbnail per screen; clicking one shows it on the preview and holds it there.
const renderStrip = debounce(() => {
  const fit = currentFit();
  $('#pv-strip').replaceChildren(
    ...preview.seq.map((item, i) => {
      let thumb;
      if (item.kind === 'wb') {
        thumb = bufferCanvas(renderWhiteboardPage(fit, item.page, wbOptions()));
      } else if (item.kind === 'story') {
        thumb = bufferCanvas(igStoryBuffer(item.index, 0.5));
      } else if (item.kind === 'posts') {
        thumb = bufferCanvas(igPostsBuffer());
      } else {
        thumb = el('img', { src: `/media/${encodeURIComponent(item.file)}`, alt: '' });
      }
      return el(
        'button',
        {
          type: 'button',
          class: i === preview.index ? 'current' : '',
          title: item.label,
          'aria-label': `Screen ${i + 1}: ${item.label}, ${item.seconds} seconds`,
          'aria-current': i === preview.index ? 'true' : null,
          onclick: () => jumpTo(i),
        },
        thumb,
        el('span', { class: 'cap' }, `${i + 1}. ${item.kind === 'wb' ? `Whiteboard ${item.page + 1}/${fit.pageCount}` : item.kind === 'media' ? item.file : item.label} · ${item.seconds} s`),
      );
    }),
  );
}, 150);

function markStrip() {
  const strip = $('#pv-strip');
  [...strip.children].forEach((b, i) => {
    b.classList.toggle('current', i === preview.index);
    if (i === preview.index) b.setAttribute('aria-current', 'true');
    else b.removeAttribute('aria-current');
  });
  // Scroll the strip sideways only. scrollIntoView would also scroll the page, which
  // pulled a phone back up to the preview every few seconds while the coach read below it.
  const cur = strip.children[preview.index];
  if (cur) {
    const left = cur.offsetLeft - strip.offsetLeft;
    if (left < strip.scrollLeft || left + cur.offsetWidth > strip.scrollLeft + strip.clientWidth) {
      strip.scrollLeft = Math.max(0, left - (strip.clientWidth - cur.offsetWidth) / 2);
    }
  }
}

function jumpTo(i) {
  preview.index = i;
  preview.startedAt = performance.now();
  preview.pausedAt = preview.startedAt;
  preview.bufKey = '';
  $('#pv-play').textContent = 'Play';
  markStrip();
}

function mediaImage(file) {
  if (!mediaImages.has(file)) {
    const img = el('img', { src: `/media/${encodeURIComponent(file)}` });
    // Draw as soon as it arrives rather than waiting for the next animation frame,
    // so a held or freshly opened preview isn't left blank.
    img.addEventListener('load', () => {
      const item = preview.seq[preview.index];
      if (item?.kind === 'media' && item.file === file) previewView.showImage(img);
    });
    mediaImages.set(file, img);
  }
  return mediaImages.get(file);
}

function previewTick(now) {
  preview.raf = requestAnimationFrame(previewTick);
  const seq = sequence();
  if (!seq.length) {
    if (preview.bufKey !== 'empty') {
      previewView.showBuffer(new Uint8ClampedArray(256 * 96 * 4).fill(0));
      preview.bufKey = 'empty';
      $('#pv-label').textContent = 'Nothing in the rotation';
      $('#pv-timer').textContent = '';
    }
    return;
  }
  const item = seq[preview.index];
  const elapsed = ((preview.pausedAt ?? now) - preview.startedAt) / 1000;
  if (elapsed >= item.seconds && preview.pausedAt === null) return stepPreview(1);

  const key = item.kind === 'media' ? `media:${item.file}` : `${item.kind}:${item.page ?? item.index ?? 0}:${state.version}`;
  if (item.kind === 'wb') {
    if (preview.bufKey !== key) previewView.showBuffer(renderWhiteboardPage(currentFit(), item.page, wbOptions()));
  } else if (item.kind === 'posts') {
    if (preview.bufKey !== key) previewView.showBuffer(igPostsBuffer());
  } else if (item.kind === 'story') {
    // The progress bar moves, so redraw a few times a second.
    if (preview.bufKey !== key || now - preview.lastDraw > 100) {
      previewView.showBuffer(igStoryBuffer(item.index, Math.min(1, elapsed / item.seconds)));
      preview.lastDraw = now;
    }
  } else if (now - preview.lastDraw > 40) {
    const img = mediaImage(item.file);
    if (img.complete && img.naturalWidth) previewView.showImage(img);
    preview.lastDraw = now;
  }
  preview.bufKey = key;
  $('#pv-label').textContent = `${preview.index + 1} of ${seq.length}: ${item.label}`;
  $('#pv-timer').textContent = preview.pausedAt !== null ? 'Held on this screen' : `next in ${Math.max(0, Math.ceil(item.seconds - elapsed))} s`;
}

function stepPreview(delta) {
  const n = sequence().length || 1;
  preview.index = (preview.index + delta + n) % n;
  preview.startedAt = performance.now();
  if (preview.pausedAt !== null) preview.pausedAt = preview.startedAt;
  preview.bufKey = '';
  markStrip();
}

function startPreview() {
  if (preview.raf) return;
  if (!preview.startedAt) preview.startedAt = performance.now();
  preview.raf = requestAnimationFrame(previewTick);
}

function stopPreview() {
  cancelAnimationFrame(preview.raf);
  preview.raf = 0;
}

const RESET_REASONS = {
  poweron: 'switched on', software: 'restarted by its software', panic: 'crashed and restarted',
  int_wdt: 'stuck and restarted (watchdog)', task_wdt: 'stuck and restarted (watchdog)', wdt: 'stuck and restarted (watchdog)',
  brownout: 'power dipped and it restarted', external: 'reset button', deepsleep: 'woke from sleep', unknown: 'unknown',
};

function nowPlayingText(np) {
  if (!np) return null;
  if (np.type === 'media') return np.file ?? 'an image or video';
  return SLIDE_TITLES[np.type] ?? np.type;
}

async function refreshStatus(force = false) {
  if (!force && !$('#wall').classList.contains('active')) return;
  try {
    const s = await request('GET', '/api/status');
    const wasDate = state.status?.local_date;
    state.status = s;
    if (s.local_date !== wasDate) {
      // The wall's day changed (midnight there, or a new time zone): PR highlights move with it.
      scheduleFit();
      preview.bufKey = '';
    }
    const dd = (v, cls) => el('dd', cls ? { class: cls } : {}, v);
    const rows = [
      ['Wall', dd(s.paused ? 'Paused' : 'Playing')],
      nowPlayingText(s.now_playing) && ['Showing', dd(nowPlayingText(s.now_playing))],
      ['Storage', dd(`${kb(s.storage.used_bytes)} of ${kb(s.storage.usable_bytes ?? s.storage.total_bytes)} used`)],
      ['Wi-Fi', dd(`${s.wifi.ssid} (${s.wifi.rssi} dBm)`)],
      ['Up for', dd(duration(s.uptime_s))],
      s.time_valid === false && ['Clock', dd("Not set yet. New PRs won't get a date until the wall reaches the internet.", 'warn')],
      ...(s.config_errors ?? []).map((c) => ['Problem', dd(`${c.file} was damaged and has been reset. The old copy is kept as ${c.renamed_to}.`, 'bad')]),
    ].filter(Boolean);
    $('#status').replaceChildren(...rows.flatMap(([k, v]) => [el('dt', {}, k), v]));
    renderDiagnostics(s);
    $('[data-control="pause"]').disabled = Boolean(s.paused);
    $('[data-control="resume"]').disabled = !s.paused;
  } catch (e) {
    $('#status').replaceChildren(el('dt', {}, 'Wall'), el('dd', {}, e.message));
  }
}

function renderDiagnostics(s) {
  const rows = [
    ['Firmware', s.mock ? `${s.fw_version} (mock server, no hardware connected)` : s.fw_version],
    ['Console', s.ui_version],
    ['Last restart', s.reset_reason ? `${RESET_REASONS[s.reset_reason] ?? s.reset_reason} (${s.reset_reason})` : null],
    ['Lowest free memory', s.min_free_heap != null ? kb(s.min_free_heap) : null],
    ['Refresh rate', s.refresh_hz != null ? `${s.refresh_hz} Hz` : null],
    ['Power limiter', s.limiter_scale != null ? (s.limiter_scale >= 1 ? 'not dimming' : `dimming to ${Math.round(s.limiter_scale * 100)}%`) : null],
    ['Clock', s.time_valid ? `set, ${s.local_date} on the wall (${s.timezone ?? ''})` : 'not set'],
    ['Storage', `${kb(s.storage.used_bytes)} used, ${kb(s.storage.usable_bytes)} usable of ${kb(s.storage.total_bytes)}`],
  ].filter(([, v]) => v !== null && v !== undefined);
  const errors = s.recent_errors ?? [];
  $('#diag-list').replaceChildren(
    ...rows.flatMap(([k, v]) => [el('dt', {}, k), el('dd', {}, String(v))]),
    el('dt', {}, 'Recent errors'),
    el('dd', {}, errors.length
      ? el('ul', { class: 'errors' }, errors.slice().reverse().map((e) => el('li', {}, `${new Date(e.time).toLocaleString()}: ${e.message}`)))
      : 'None'),
  );
}

// ---------------------------------------------------------------- start up

function bind() {
  window.addEventListener('hashchange', showTab);
  $('#pin-cancel').addEventListener('click', () => $('#pin-dialog').close());

  $('#pv-prev').addEventListener('click', () => stepPreview(-1));
  $('#pv-next').addEventListener('click', () => stepPreview(1));
  $('#pv-seams').addEventListener('change', (e) => {
    previewView.seams = e.target.checked;
    preview.bufKey = '';
  });
  document.addEventListener('keydown', (e) => {
    if (!$('#wall').classList.contains('active') || e.target.closest('input, select, textarea')) return;
    if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
      jumpTo((preview.index + (e.key === 'ArrowRight' ? 1 : -1) + preview.seq.length) % Math.max(1, preview.seq.length));
      e.preventDefault();
    }
  });
  $('#pv-play').addEventListener('click', (e) => {
    if (preview.pausedAt === null) {
      preview.pausedAt = performance.now();
      e.target.textContent = 'Play';
    } else {
      preview.startedAt += performance.now() - preview.pausedAt;
      preview.pausedAt = null;
      e.target.textContent = 'Pause';
    }
  });
  for (const b of document.querySelectorAll('[data-control]')) {
    b.addEventListener('click', async () => {
      try {
        await request('POST', '/api/control', { json: { action: b.dataset.control } });
        toast(`Sent "${b.textContent.replace(/[◀▶]/g, '').trim()}" to the wall.`);
        refreshStatus();
      } catch (e) {
        toast(e.message, true);
      }
    });
  }

  $('#ath-add').addEventListener('click', addAthlete);
  $('#ath-save').addEventListener('click', saveAthletes);
  $('#ath-revert').addEventListener('click', () => {
    state.athletes = clone(state.athletesSaved);
    rawEntries.clear();
    showErrors($('#ath-errors'), []);
    renderAthletes();
  });
  $('#ath-filter').addEventListener('input', applyFilter);
  $('#ath-import').addEventListener('change', (e) => e.target.files[0] && importCsv(e.target.files[0]));
  $('#ath-export').addEventListener('click', exportCsv);

  $('#media-file').addEventListener('change', (e) => e.target.files[0] && pickFile(e.target.files[0]));
  const drop = $('#drop');
  drop.addEventListener('dragover', (e) => {
    e.preventDefault();
    drop.classList.add('over');
  });
  drop.addEventListener('dragleave', () => drop.classList.remove('over'));
  drop.addEventListener('drop', (e) => {
    e.preventDefault();
    drop.classList.remove('over');
    if (e.dataTransfer.files[0]) pickFile(e.dataTransfer.files[0]);
  });
  for (const id of ['#crop-fit', '#crop-zoom', '#crop-x', '#crop-y', '#clip-start', '#clip-end', '#clip-fps']) {
    $(id).addEventListener('input', drawCrop);
  }
  $('#media-convert').addEventListener('click', convertAndUpload);
  $('#media-cancel').addEventListener('click', closeSource);
  $('#media-stop').addEventListener('click', () => {
    convertCtrl?.abort();
    toast('Stopped. Nothing was uploaded.');
  });
  // The file pickers are labels; let Enter and Space open them from the keyboard too.
  for (const id of ['#drop', '#ath-import-label']) {
    $(id).addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        $(id).querySelector('input[type="file"]').click();
      }
    });
  }

  $('#rot-add-wb').addEventListener('click', () => {
    state.slides.push({ id: newId('s'), type: 'whiteboard', duration_s: DEFAULT_WB_SECONDS, enabled: true });
    changed();
    renderRotation();
  });
  $('#rot-add-story').addEventListener('click', () => {
    state.slides.push({ id: newId('s'), type: 'instagram_story', duration_s: DEFAULT_STORY_SECONDS, enabled: true });
    changed();
    renderRotation();
  });
  $('#rot-add-posts').addEventListener('click', () => {
    state.slides.push({ id: newId('s'), type: 'instagram_posts', duration_s: DEFAULT_POSTS_SECONDS, enabled: true });
    changed();
    renderRotation();
  });
  $('#rot-add-media').addEventListener('change', (e) => {
    if (e.target.value) addMediaSlide(e.target.value, false);
    e.target.value = '';
  });
  $('#rot-save').addEventListener('click', () => saveRotation());
  $('#rot-revert').addEventListener('click', () => {
    state.slides = clone(state.slidesSaved);
    changed();
    renderRotation();
    renderMedia();
  });

  bindSettings();

  window.addEventListener('beforeunload', (e) => {
    if (athletesDirty() || rotationDirty() || settingsDirty()) e.preventDefault();
  });
  setInterval(refreshStatus, 10000);
  $('#diag').addEventListener('toggle', () => $('#diag').open && refreshStatus(true));
}

async function load() {
  const [a, p, s, m, ig, st] = await Promise.all([
    request('GET', '/api/athletes'),
    request('GET', '/api/playlist'),
    request('GET', '/api/settings'),
    request('GET', '/api/media'),
    request('GET', '/api/instagram'),
    request('GET', '/api/status'),
  ]);
  state.status = st;
  state.instagram = ig;
  applyAthletes(a);
  applySlides(p);
  state.settings = s;
  state.settingsSaved = clone(s);
  state.media = m;
}

async function main() {
  previewView = new LedView($('#pv-canvas'), 4);
  cropView = new LedView($('#crop-canvas'), 3);
  bind();
  $('#conn-retry').addEventListener('click', async () => {
    if (loaded) {
      // Just check the wall answers; unsaved edits on the page are left alone.
      await request('GET', '/api/status').then(() => toast('The wall is back. Save any changes you made.')).catch((e) => toast(e.message, true));
      return;
    }
    start();
  });
  start();
}

async function start() {
  try {
    await load();
  } catch (e) {
    setOnline(false);
    toast(e.message, true);
    return;
  }
  loaded = true;
  if (!state.settings.pin_set) await showSetup();
  renderDiagnostics(state.status);
  renderAthletes();
  renderMedia();
  renderSettings();
  renderInstagram();
  loadTiles();
  renderRotation();
  showTab();
}

main();
