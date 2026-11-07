// "New PRs" slide for the 256x96 wall: one athlete per screen, celebrating the lifts they
// set a PR in recently, with the new weight and how much it went up.
//
// Like layout.js and igslides.js, this file is the spec the firmware ports pixel for pixel.

import { W, LIFTS, createSurface, textWidth, formatKg, isRecentPr } from './layout.js';

/** At most this many athletes get a screen per pass, newest PR first; it is also the dot row's length. */
export const MAX_PR_SCREENS = 20;

const LIFT_NAMES = {
  snatch: 'Snatch',
  clean_jerk: 'Clean & Jerk',
  total: 'Total',
  back_squat: 'Back squat',
  front_squat: 'Front squat',
};

const C = {
  white: [255, 255, 255],
  gold: [255, 200, 40],
  green: [60, 255, 110],
  grey: [140, 140, 140],
  dim: [60, 60, 60],
  banner: [120, 80, 0],
};

const validDay = (d) => d instanceof Date && !Number.isNaN(d.getTime());

/**
 * Athletes with at least one PR inside the highlight window, newest PR first. Each entry
 * carries the lifts to show, newest first. Hidden athletes are left out.
 * today: the wall's local date; null or an invalid Date means the wall has no clock, and
 * then nothing is recent, as on the whiteboard.
 *
 * Perf: O(n x 5) to collect plus O(n log n) to sort; n is capped at 500 by the API.
 */
export function recentPrs(athletes, days, today = new Date()) {
  const out = [];
  if (!validDay(today)) return out;
  for (const a of athletes) {
    if (a.visible === false || !a.name.trim()) continue;
    const lifts = LIFTS.filter((l) => a[l] != null && isRecentPr(a, l, days, today)).map((l) => ({
      lift: l,
      kg: a[l],
      prev: a.pr_prev?.[l] ?? null,
      date: a.pr_dates[l],
    }));
    if (!lifts.length) continue;
    lifts.sort((x, y) => (x.date < y.date ? 1 : x.date > y.date ? -1 : LIFTS.indexOf(x.lift) - LIFTS.indexOf(y.lift)));
    out.push({ athlete: a, lifts, newest: lifts[0].date });
  }
  out.sort((x, y) => (x.newest < y.newest ? 1 : x.newest > y.newest ? -1 : 0));
  return out;
}

function daysAgo(date, today) {
  if (!validDay(today)) return '';
  const t = Date.parse(`${date}T00:00:00Z`);
  const now = Date.UTC(today.getFullYear(), today.getMonth(), today.getDate());
  const d = Math.round((now - t) / 86400000);
  return d <= 0 ? 'today' : d === 1 ? 'yesterday' : `${d} days ago`;
}

/**
 * One athlete's screen. entry: an item from recentPrs(). index/count: position among the
 * athletes being celebrated (drawn as dots).
 *
 * Perf: fixed 256x96 output; O(pixels + characters).
 */
export function renderPrSlide({ entry, index, count, today = new Date() }) {
  const s = createSurface();
  const { athlete, lifts } = entry;

  // Gold banner across the top with the athlete's handle.
  s.rect(0, 0, W - 1, 19, C.banner);
  s.rect(0, 20, W - 1, 20, C.gold);
  const label = 'NEW PR';
  s.text(label, 4, 6, C.gold);
  const nameX = 4 + textWidth(label) + 8;
  const room = W - nameX - 4;
  const big = textWidth(athlete.name, false, 2) <= room;
  let name = athlete.name;
  if (!big) {
    const chars = Math.floor((room + 1) / 6);
    if (name.length > chars) name = name.slice(0, chars - 1) + '~';
  }
  s.text(name, nameX, big ? 2 : 6, C.white, { scale: big ? 2 : 1 });

  // Lifts: one big line for a single PR, up to three lines for several.
  const shown = lifts.slice(0, 3);
  if (shown.length === 1) {
    const l = shown[0];
    const liftName = LIFT_NAMES[l.lift];
    const scale = textWidth(liftName, false, 2) <= W - 8 ? 2 : 1;
    s.text(liftName, Math.floor((W - textWidth(liftName, false, scale)) / 2), 28, C.white, { scale });
    const kg = `${formatKg(l.kg)} kg`;
    const kgW = textWidth(kg, true, 3);
    const up = l.prev != null && l.kg > l.prev ? `+${formatKg(l.kg - l.prev)}` : '';
    const upW = up ? textWidth(up, true, 2) + 8 : 0;
    const x = Math.floor((W - kgW - upW) / 2);
    s.text(kg, x, 48, C.gold, { scale: 3, narrowDot: true });
    if (up) s.text(up, x + kgW + 8, 56, C.green, { scale: 2, narrowDot: true });
  } else {
    // One column for every row's weight, so "+3" on one row and "+12.5" on the next don't
    // push their numbers out of line.
    const ups = shown.map((l) => (l.prev != null && l.kg > l.prev ? `+${formatKg(l.kg - l.prev)}` : ''));
    const upW = Math.max(...ups.map((up) => (up ? textWidth(up, true) + 6 : 0)));
    shown.forEach((l, i) => {
      const y = 26 + i * 18;
      s.text(LIFT_NAMES[l.lift], 6, y, C.white, { scale: 2 });
      const kg = formatKg(l.kg);
      s.text(kg, W - 6 - upW - textWidth(kg, true, 2), y, C.gold, { scale: 2, narrowDot: true });
      if (ups[i]) s.text(ups[i], W - upW, y + 4, C.green, { narrowDot: true });
    });
    if (lifts.length > 3) s.text(`+${lifts.length - 3} more`, 6, 80, C.grey);
  }

  // Footer: when, and which athlete of how many.
  const when = daysAgo(entry.newest, today);
  s.text(when, W - 4 - textWidth(when), 87, C.grey);
  if (count > 1) {
    const dots = Math.min(count, MAX_PR_SCREENS);
    for (let i = 0; i < dots; i++) {
      const x = 4 + i * 6;
      s.rect(x, 89, x + 3, 92, i === index % dots ? C.gold : C.dim);
    }
  }
  return s.buf;
}
