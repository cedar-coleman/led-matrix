// PR whiteboard layout and renderer for the 256x96 wall.
//
// This file is the layout spec. The browser preview draws with it directly, and the
// firmware's C++ renderer is a line-for-line port, so both always agree on how many
// pages there are and where every pixel goes.

import { FONT5X7 } from './font5x7.js';

export const W = 256;
export const H = 96;
export const LIFTS = ['snatch', 'clean_jerk', 'total', 'back_squat', 'front_squat'];
export const LIFT_LABELS = ['S', 'CJ', 'Ttl', 'BS', 'FS'];
export const MIN_SECONDS = 5;
export const MAX_SECONDS = 999;
export const MAX_KG = 500;

const C = {
  rowAlt: [0, 10, 28], // every other row, dim navy band
  rowRule: [0, 30, 75], // thin line between rows when there is room for one
  grid: [0, 70, 170], // the chalkboard's blue column tape
  teal: [0, 150, 150], // brand teal: frame and header rules
  title: [255, 255, 255],
  header: [235, 215, 70], // the chalkboard's yellow header chalk
  name: [170, 220, 255],
  num: [255, 255, 255],
  pr: [60, 255, 110], // lift set within the highlight window
  page: [120, 120, 120],
  empty: [120, 120, 120],
};

const TITLE_RULE_Y = 11;
const HEADER_Y = 12;
const HEADER_RULE_Y = 20;
const BODY_Y = 21;
const BOTTOM_RULE_Y = H - 1;
const BODY_H = BOTTOM_RULE_Y - BODY_Y; // 74 px for data rows
const ROW_MIN = 8; // glyph height including descenders
const ROW_MAX = 10; // more than this and short pages look sparse
export const MAX_ROWS = Math.floor(BODY_H / ROW_MIN); // 9
const INNER_W = W - 7; // 2 frame rules + 5 column rules
const NAME_PAD = 4; // 2 px each side of the name text
const NUM_PAD = 2; // at least 1 px clear of each column rule

/** kg value for display: '' for blank, no trailing '.0'. */
export function formatKg(v) {
  if (v === null || v === undefined || v === '') return '';
  return Number.isInteger(v) ? String(v) : v.toFixed(1);
}

/** Parse a kg entry. Returns { value } (null for blank) or { error }. */
export function parseKg(text) {
  const s = String(text ?? '').trim().replace(',', '.');
  if (s === '') return { value: null };
  const v = Number(s);
  if (!Number.isFinite(v)) return { error: 'Not a number' };
  if (v <= 0 || v > MAX_KG) return { error: `Must be between 0.5 and ${MAX_KG} kg` };
  if (Math.round(v * 2) !== v * 2) return { error: 'Use 0.5 kg steps' };
  return { value: v };
}

/** Whole seconds within the allowed screen time. */
export function validSeconds(v) {
  return Number.isInteger(v) && v >= MIN_SECONDS && v <= MAX_SECONDS;
}

// Characters outside printable ASCII have no glyph in the 5x7 font; show '?'.
// The one exception is the heart, which the font has at code 3.
function glyphCode(ch) {
  if (ch === '\u2665') return 3;
  const code = ch.charCodeAt(0);
  return code >= 32 && code <= 126 ? code : 63;
}

function advance(ch, narrowDot) {
  // In numbers '.' is one baseline pixel with a 2 px advance, so "250.5" is 25 px wide.
  return narrowDot && ch === '.' ? 2 : 6;
}

export function textWidth(s, narrowDot = false, scale = 1) {
  let w = 0;
  for (const ch of s) w += advance(ch, narrowDot);
  return w ? (w - 1) * scale : 0;
}

/**
 * A 256x96 RGBA drawing surface with the wall's font. Every slide renderer draws
 * through this, and the firmware has the same primitives.
 */
export function createSurface() {
  const buf = new Uint8ClampedArray(W * H * 4);
  for (let i = 3; i < buf.length; i += 4) buf[i] = 255;
  const put = (x, y, c) => {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    const o = (y * W + x) * 4;
    buf[o] = c[0];
    buf[o + 1] = c[1];
    buf[o + 2] = c[2];
  };
  const rect = (x0, y0, x1, y1, c) => {
    for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) put(x, y, c);
  };
  // scale 2 doubles every font pixel, for headline text.
  const text = (s, x, y, c, { narrowDot = false, scale = 1 } = {}) => {
    for (const ch of s) {
      if (narrowDot && ch === '.') {
        rect(x, y + 6 * scale, x + scale - 1, y + 7 * scale - 1, c);
      } else {
        const g = glyphCode(ch) * 5;
        for (let col = 0; col < 5; col++) {
          const bits = FONT5X7[g + col];
          for (let row = 0; row < 8; row++) {
            if ((bits >> row) & 1) rect(x + col * scale, y + row * scale, x + (col + 1) * scale - 1, y + (row + 1) * scale - 1, c);
          }
        }
      }
      x += advance(ch, narrowDot) * scale;
    }
  };
  /** Copy an RGBA tile (w x h) to (x0, y0). */
  const blit = (tile, w, h, x0, y0) => {
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const o = (y * w + x) * 4;
        put(x0 + x, y0 + y, [tile[o], tile[o + 1], tile[o + 2]]);
      }
    }
  };
  const icon = (x0, y0, scale = 1) => instagramIcon((x, y, c) => rect(x0 + x * scale, y0 + y * scale, x0 + (x + 1) * scale - 1, y0 + (y + 1) * scale - 1, c), 0, 0);
  return { buf, put, rect, text, blit, icon };
}

export function sortAthletes(list, mode) {
  const out = [...list];
  if (mode === 'total') {
    out.sort((a, b) => (b.total ?? -1) - (a.total ?? -1));
  } else if (mode === 'name') {
    const key = (a) => a.name.replace(/^[@_.]+/, '').toLowerCase();
    out.sort((a, b) => key(a).localeCompare(key(b)));
  }
  return out; // 'board': the order coaches arranged in the editor
}

/**
 * Decide pages, row height and column widths for a list of athletes.
 *
 * - Fewest pages: at most MAX_ROWS (9) athletes per page.
 * - Balanced pages: 31 athletes become 8, 8, 8, 7 rather than 9, 9, 9, 4.
 * - Rows stretch to fill the page (9 or 10 px pitch with a thin rule between rows)
 *   when a page holds fewer than 9.
 * - Columns are measured once over every athlete, so they don't jump between pages.
 *   The name column is as wide as the longest name needs; whatever is left widens the
 *   number columns. If the longest name can't fit, numbers shrink to their minimum and
 *   long names are cut with '~'.
 *
 * Perf: O(n) over athletes x 5 lifts for measuring, O(n) for paging; no allocation
 * beyond the page arrays. n is capped at 500 by the API.
 */
export function fitWhiteboard(athletes) {
  const rows = athletes.filter((a) => a.visible !== false);
  const n = rows.length;
  const pageCount = Math.max(1, Math.ceil(n / MAX_ROWS));
  const base = Math.floor(n / pageCount);
  const extra = n % pageCount;
  const pages = [];
  for (let p = 0, i = 0; p < pageCount; p++) {
    const k = base + (p < extra ? 1 : 0);
    pages.push(rows.slice(i, i + k));
    i += k;
  }
  const perPage = Math.max(1, base + (extra ? 1 : 0));
  const rowPitch = Math.min(ROW_MAX, Math.max(ROW_MIN, Math.floor(BODY_H / perPage)));

  let valW = Math.max(...LIFT_LABELS.map((l) => textWidth(l)));
  let longest = textWidth('Name');
  for (const a of rows) {
    longest = Math.max(longest, textWidth(a.name));
    for (const k of LIFTS) valW = Math.max(valW, textWidth(formatKg(a[k]), true));
  }
  const numMin = valW + NUM_PAD;
  const nameNeed = longest + NAME_PAD;
  let numW = numMin;
  if (nameNeed + 5 * numMin <= INNER_W) {
    numW += Math.floor((INNER_W - nameNeed - 5 * numMin) / 6);
  }
  const nameW = INNER_W - 5 * numW;
  const nameChars = Math.floor((nameW - NAME_PAD + 1) / 6);
  const truncated = rows.filter((a) => a.name.length > nameChars).map((a) => a.name);

  return { pages, pageCount, perPage, rowPitch, valW, numW, nameW, nameChars, truncated, count: n };
}

/** x of the rule on the left of column i (0 = name, 1..5 = lifts); rule 6 is the right frame. */
export function ruleX(fit, i) {
  return i === 0 ? 0 : fit.nameW + 1 + (i - 1) * (fit.numW + 1);
}

export function isRecentPr(a, lift, days, today = new Date()) {
  const d = a.pr_dates?.[lift];
  if (!days || !d) return false;
  const t = Date.parse(`${d}T00:00:00Z`);
  if (Number.isNaN(t)) return false;
  const now = Date.UTC(today.getFullYear(), today.getMonth(), today.getDate());
  const age = (now - t) / 86400000;
  return age >= 0 && age < days;
}

/**
 * Render one page to RGBA (ImageData layout), 256x96.
 *
 * Perf: fixed-size output (98 KB); work is O(pixels + characters on the page), at most
 * 9 rows x ~45 glyphs, independent of the total athlete count.
 */
export function renderWhiteboardPage(fit, pageIndex, opts = {}) {
  const { title = 'THE STRENGTH TANK', prDays = 7, today = new Date() } = opts;
  const { buf, rect, text, icon } = createSurface();

  // Title band: Instagram glyph, title, page number.
  icon(2, 1);
  const pageLabel = fit.pageCount > 1 ? `${pageIndex + 1}/${fit.pageCount}` : '';
  const titleRoom = Math.floor((W - 15 - (pageLabel ? textWidth(pageLabel) + 6 : 2) + 1) / 6);
  text(title.slice(0, titleRoom), 15, 2, C.title);
  if (pageLabel) text(pageLabel, W - 2 - textWidth(pageLabel), 2, C.page);

  const page = fit.pages[pageIndex] ?? [];
  const pitch = fit.rowPitch;
  const textDy = pitch >= 10 ? 1 : 0;

  // Row bands and thin row rules go under the column rules.
  page.forEach((_, i) => {
    const top = BODY_Y + i * pitch;
    if (i % 2) rect(1, top, W - 2, top + pitch - 1, C.rowAlt);
    if (pitch >= 9 && i < page.length - 1) rect(1, top + pitch - 1, W - 2, top + pitch - 1, C.rowRule);
  });

  // Header labels.
  text('Name', 3, HEADER_Y, C.header);
  LIFT_LABELS.forEach((label, i) => {
    const left = ruleX(fit, i + 1) + 1;
    text(label, left + Math.floor((fit.numW - textWidth(label)) / 2), HEADER_Y, C.header);
  });

  // Column rules, then the teal frame and header rules on top.
  for (let i = 1; i <= 5; i++) rect(ruleX(fit, i), TITLE_RULE_Y, ruleX(fit, i), BOTTOM_RULE_Y, C.grid);
  rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y, C.teal);
  rect(0, HEADER_RULE_Y, W - 1, HEADER_RULE_Y, C.teal);
  rect(0, BOTTOM_RULE_Y, W - 1, BOTTOM_RULE_Y, C.teal);
  rect(0, TITLE_RULE_Y, 0, BOTTOM_RULE_Y, C.teal);
  rect(W - 1, TITLE_RULE_Y, W - 1, BOTTOM_RULE_Y, C.teal);

  if (fit.count === 0) {
    const msg = 'No athletes yet';
    text(msg, Math.floor((fit.nameW - textWidth(msg)) / 2), BODY_Y + 30, C.empty);
    return buf;
  }

  // Numbers sit right-aligned in a block of the widest value, centred in each column.
  const numInset = Math.floor((fit.numW - fit.valW) / 2);
  page.forEach((a, i) => {
    const y = BODY_Y + i * pitch + textDy;
    let name = a.name;
    if (name.length > fit.nameChars) name = name.slice(0, fit.nameChars - 1) + '~';
    text(name, 3, y, C.name);
    LIFTS.forEach((lift, j) => {
      const s = formatKg(a[lift]);
      if (!s) return;
      const right = ruleX(fit, j + 1) + numInset + fit.valW;
      const color = isRecentPr(a, lift, prDays, today) ? C.pr : C.num;
      text(s, right - textWidth(s, true), y, color, { narrowDot: true });
    });
  });
  return buf;
}

// 9x9 Instagram glyph, gradient orange (bottom-left) to magenta (top-right).
const IG = [
  '.GGGGGGG.',
  'GWWWWWWWG',
  'GWGGGGWWG',
  'GWGGWGGWG',
  'GWGWGWGWG',
  'GWGGWGGWG',
  'GWGGGGGWG',
  'GWWWWWWWG',
  '.GGGGGGG.',
];

function instagramIcon(put, x0, y0) {
  IG.forEach((line, y) => {
    [...line].forEach((ch, x) => {
      if (ch === 'W') put(x0 + x, y0 + y, [255, 255, 255]);
      else if (ch === 'G') {
        const t = (x + (8 - y)) / 16;
        put(x0 + x, y0 + y, [255, Math.round(170 * (1 - t)), Math.round(40 + 160 * t)]);
      }
    });
  });
}
