// Announcements for the 256x96 wall: one coach's message per screen, white on brand teal.
//
// Like layout.js and prslides.js, the renderer and the rules for which announcements are
// showing are the spec the firmware ports line for line (lib/render/src/announceslides.*).
// unshownChars() and plainText() at the bottom are console helpers only; the wall never
// needs them.

import { W, H, createSurface, textWidth } from './layout.js';

/** At most this many announcements get a screen per pass; it is also the dot row's length. */
export const MAX_ANNOUNCE_SCREENS = 20;
export const MAX_ANNOUNCEMENTS = 50;
export const MAX_ANNOUNCE_CHARS = 200;
export const MAX_ANNOUNCE_BREAKS = 6;

const C = {
  teal: [0, 150, 150], // the logo slide's background
  white: [255, 255, 255],
  dim: [0, 90, 90], // the other dots: visible on teal without competing with the text
};

const MARGIN = 4;
const SCALES = [3, 2];
const DOT = 4;
const DOT_PITCH = 7;
const DOT_Y = H - MARGIN - DOT; // 88
const DOT_GAP = 4; // clear space between the text and the dots

// The wall's local date, 'YYYY-MM-DD'; anything else means the wall has no clock.
const validToday = (today) => typeof today === 'string' && /^\d{4}-\d{2}-\d{2}$/.test(today);

/**
 * Whether one announcement is on the wall today. Undated ones show whenever they are
 * enabled, even without a clock; a dated one needs a clock and today inside its range
 * (end_date included). Dates are 'YYYY-MM-DD', so comparing the strings compares the days.
 */
export function announcementActive(a, today) {
  if (a.enabled === false) return false;
  const start = a.start_date || '';
  const end = a.end_date || '';
  if (!start && !end) return true;
  if (!validToday(today)) return false;
  return (!start || start <= today) && (!end || today <= end);
}

/**
 * The announcements that get a screen in one pass, in list order, at most
 * MAX_ANNOUNCE_SCREENS. today: the wall's local date, or null while it has no clock.
 *
 * Perf: O(n) over the list; n is capped at 50 by the API.
 */
export function announcementScreens(list, today) {
  return list.filter((a) => announcementActive(a, today)).slice(0, MAX_ANNOUNCE_SCREENS);
}

/**
 * Word-wrap at spaces into lines of at most maxChars characters. '\n' starts a new line
 * (two in a row leave a blank line), runs of spaces collapse, and a word longer than a
 * line is split across lines.
 *
 * Perf: O(characters x lines) for the array copies; text is at most 200 characters.
 */
export function wrapLines(text, maxChars) {
  const lines = [];
  for (const para of text.split('\n')) {
    let line = [];
    for (const word of para.split(' ')) {
      if (!word) continue;
      let w = [...word];
      if (line.length && line.length + 1 + w.length > maxChars) {
        lines.push(line.join(''));
        line = [];
      }
      if (line.length) line.push(' ');
      while (line.length + w.length > maxChars) {
        const take = maxChars - line.length;
        lines.push(line.concat(w.slice(0, take)).join(''));
        w = w.slice(take);
        line = [];
      }
      line = line.concat(w);
    }
    lines.push(line.join(''));
  }
  return lines;
}

// Characters that fit across the wall at this scale with the margin on both sides.
const lineChars = (scale) => Math.floor((W - 2 * MARGIN + scale) / (6 * scale));
// Lines are 9 font pixels apart: the 8-row glyph plus one row of air.
const blockHeight = (lines, scale) => lines * 9 * scale - scale;

/** Characters in the longest word. Perf: one pass over the text. */
export function longestWord(text) {
  let longest = 0;
  for (const para of text.split('\n')) {
    for (const word of para.split(' ')) longest = Math.max(longest, [...word].length);
  }
  return longest;
}

/**
 * The largest scale (3, then 2, then 1) at which the whole text fits inside the margin,
 * above the dots when there are several announcements. Scales 3 and 2 only count as a fit
 * when no word has to be split: "PERSONALISED" whole at scale 2 reads better than split
 * at scale 3. At scale 1 anything still too long is cut, and the last line kept ends in
 * '~' (`cut` is true).
 *
 * Perf: at most three wraps of 200 characters.
 */
export function fitAnnouncement(text, count) {
  const bottom = count > 1 ? DOT_Y - DOT_GAP : H - MARGIN;
  const areaH = bottom - MARGIN;
  const longest = longestWord(text);
  for (const scale of SCALES) {
    if (longest > lineChars(scale)) continue;
    const lines = wrapLines(text, lineChars(scale));
    if (blockHeight(lines.length, scale) <= areaH) return { scale, lines, cut: false, top: MARGIN, areaH };
  }
  const maxChars = lineChars(1);
  const lines = wrapLines(text, maxChars);
  const maxLines = Math.floor((areaH + 1) / 9);
  if (lines.length <= maxLines) return { scale: 1, lines, cut: false, top: MARGIN, areaH };
  const kept = lines.slice(0, maxLines);
  const last = [...kept[maxLines - 1]];
  kept[maxLines - 1] = (last.length >= maxChars ? last.slice(0, maxChars - 1).join('') : last.join('')) + '~';
  return { scale: 1, lines: kept, cut: true, top: MARGIN, areaH };
}

/**
 * One announcement's screen: white text centred on teal, and when there are several, a
 * row of dots at the bottom with the current one white.
 *
 * Perf: fixed 256x96 output; O(pixels + characters).
 */
export function renderAnnouncement({ text, index, count }) {
  const s = createSurface();
  s.rect(0, 0, W - 1, H - 1, C.teal);
  const fit = fitAnnouncement(text, count);
  const { scale, lines } = fit;
  let y = fit.top + Math.floor((fit.areaH - blockHeight(lines.length, scale)) / 2);
  for (const line of lines) {
    s.text(line, Math.floor((W - textWidth(line, false, scale)) / 2), y, C.white, { scale });
    y += 9 * scale;
  }
  if (count > 1) {
    const dots = Math.min(count, MAX_ANNOUNCE_SCREENS);
    const x0 = Math.floor((W - (dots * DOT_PITCH - (DOT_PITCH - DOT))) / 2);
    for (let i = 0; i < dots; i++) {
      const x = x0 + i * DOT_PITCH;
      s.rect(x, DOT_Y, x + DOT - 1, DOT_Y + DOT - 1, i === index % dots ? C.white : C.dim);
    }
  }
  return s.buf;
}

// ---------------------------------------------------------------- console helpers

/**
 * The distinct characters the wall's 5x7 font can't draw (it shows '?' for each), in the
 * order they first appear. The heart is in the font; '\n' is a line break, not a glyph.
 *
 * Perf: O(characters x distinct unshown), both small (200 characters at most).
 */
export function unshownChars(text) {
  const out = [];
  for (const ch of text) {
    if (ch === '\n' || ch === '♥') continue;
    const c = ch.codePointAt(0);
    if (c >= 32 && c <= 126) continue;
    if (!out.includes(ch)) out.push(ch);
  }
  return out;
}

// What phones and word processors put in place of plain ASCII punctuation.
const PLAIN = [
  [/[‘’‚‛′]/g, "'"],
  [/[“”„‟″]/g, '"'],
  [/[‐-―−]/g, '-'],
  [/…/g, '...'],
  [/ /g, ' '],
];

/** Curly quotes, dashes, ellipses and non-breaking spaces swapped for plain characters. Perf: five passes over the text. */
export function plainText(text) {
  return PLAIN.reduce((t, [re, to]) => t.replace(re, to), text);
}
