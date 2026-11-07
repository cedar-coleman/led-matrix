// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { W, H, LIFTS, createSurface, textWidth } from '../web/layout.js';
import { recentPrs, renderPrSlide, MAX_PR_SCREENS } from '../web/prslides.js';

const TODAY = new Date(2026, 10, 6); // the wall's local date, 2026-11-06
const GOLD = '255,200,40';
const GREEN = '60,255,110';
const GREY = '140,140,140';
const BANNER = [120, 80, 0];

const athlete = (id, name, lifts = {}, dates = {}, prev, extra = {}) => {
  const a = { id, name, visible: true, pr_dates: dates, ...extra };
  for (const l of LIFTS) a[l] = lifts[l] ?? null;
  if (prev !== undefined) a.pr_prev = prev;
  return a;
};
const px = (buf, x, y) => [...buf.slice((y * W + x) * 4, (y * W + x) * 4 + 3)].join();
const count = (buf, color, x0 = 0, y0 = 0, x1 = W - 1, y1 = H - 1) => {
  let n = 0;
  for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) if (px(buf, x, y) === color) n++;
  return n;
};
// Rows y0..y1 of `buf` must equal what `draw` puts on a blank surface.
const sameRows = (buf, y0, y1, draw) => {
  const want = createSurface();
  draw(want);
  for (let y = y0; y <= y1; y++) {
    for (let x = 0; x < W; x++) {
      if (px(buf, x, y) !== px(want.buf, x, y)) return `differs at (${x},${y}): ${px(buf, x, y)} vs ${px(want.buf, x, y)}`;
    }
  }
  return '';
};
const slide = (a, opts = {}) => renderPrSlide({ entry: recentPrs([a], 7, TODAY)[0], index: 0, count: 1, today: TODAY, ...opts });

test('recentPrs: only lifts inside the window, newest athlete first, hidden and blank names left out', () => {
  const list = [
    athlete('a', 'Ann', { snatch: 80, total: 180 }, { snatch: '2026-11-03', total: '2026-11-05' }),
    athlete('b', 'Bo', { back_squat: 150 }, { back_squat: '2026-10-30' }), // 7 days: outside
    athlete('c', 'Cy', { clean_jerk: 100 }, { clean_jerk: '2026-11-06' }),
    athlete('d', 'Di', { snatch: 70 }, { snatch: '2026-11-06' }, undefined, { visible: false }),
    athlete('e', '   ', { snatch: 70 }, { snatch: '2026-11-06' }),
    athlete('f', 'Fe', { snatch: null }, { snatch: '2026-11-06' }), // cleared since: nothing to show
    athlete('g', 'Gus', { front_squat: 120 }, { front_squat: '2026-11-07' }), // tomorrow: not yet
    athlete('h', 'Hal', { total: 200 }, { total: '2026-11-05' }), // ties with Ann: board order kept
  ];
  const got = recentPrs(list, 7, TODAY);
  assert.deepEqual(got.map((e) => e.athlete.id), ['c', 'a', 'h']);
  assert.deepEqual(got[1].lifts.map((l) => l.lift), ['total', 'snatch'], 'newest lift first');
  assert.equal(got[1].newest, '2026-11-05');
  assert.deepEqual(recentPrs(list, 8, TODAY).map((e) => e.athlete.id), ['c', 'a', 'h', 'b']);
  assert.deepEqual(recentPrs(list, 1, TODAY).map((e) => e.athlete.id), ['c']);
});

test('recentPrs: same-day lifts follow the board order of lifts', () => {
  const all = Object.fromEntries(LIFTS.map((l) => [l, 100]));
  const dates = Object.fromEntries(LIFTS.map((l) => [l, '2026-11-06']));
  const [e] = recentPrs([athlete('a', 'Ann', all, dates)], 7, TODAY);
  assert.deepEqual(e.lifts.map((l) => l.lift), LIFTS);
});

test('recentPrs: nothing is recent with highlighting off or without a valid clock', () => {
  const list = [athlete('a', 'Ann', { snatch: 80 }, { snatch: '2026-11-06' })];
  assert.equal(recentPrs(list, 7, TODAY).length, 1);
  assert.deepEqual(recentPrs(list, 0, TODAY), []);
  assert.deepEqual(recentPrs(list, 7, null), []);
  assert.deepEqual(recentPrs(list, 7, new Date(NaN)), []);
});

test('recentPrs: the previous value, or null when the wall never recorded one', () => {
  const list = [
    athlete('a', 'Ann', { snatch: 95.5, total: 180 }, { snatch: '2026-11-06', total: '2026-11-06' }, { snatch: 93, total: null }),
    athlete('b', 'Bo', { snatch: 60 }, { snatch: '2026-11-06' }), // stored before pr_prev existed
  ];
  const [a, b] = recentPrs(list, 7, TODAY);
  assert.deepEqual(a.lifts.map((l) => [l.lift, l.kg, l.prev]), [['snatch', 95.5, 93], ['total', 180, null]]);
  assert.equal(b.lifts[0].prev, null);
});

test('one lift: name at double size, weight and a green increase in 0.5 kg steps', () => {
  const a = athlete('a', 'Brad B.', { snatch: 95.5 }, { snatch: '2026-11-06' }, { snatch: 93 });
  const buf = slide(a);
  assert.equal(buf.length, W * H * 4);
  assert.equal(px(buf, 0, 0), BANNER.join());
  assert.equal(px(buf, 100, 20), GOLD, 'gold rule under the banner');
  const banner = sameRows(buf, 0, 19, (s) => {
    s.rect(0, 0, W - 1, 19, BANNER);
    s.text('NEW PR', 4, 6, [255, 200, 40]);
    s.text('Brad B.', 47, 2, [255, 255, 255], { scale: 2 });
  });
  assert.equal(banner, '');
  // "95.5 kg" (scale 3) then "+2.5" (scale 2), centred together.
  const kgW = textWidth('95.5 kg', true, 3);
  const upW = textWidth('+2.5', true, 2) + 8;
  const x = Math.floor((W - kgW - upW) / 2);
  const body = sameRows(buf, 48, 71, (s) => {
    s.text('95.5 kg', x, 48, [255, 200, 40], { scale: 3, narrowDot: true });
    s.text('+2.5', x + kgW + 8, 56, [60, 255, 110], { scale: 2, narrowDot: true });
  });
  assert.equal(body, '');
  assert.ok(count(buf, GREEN) > 0);
});

test('an unknown previous value draws no "+"', () => {
  for (const prev of [undefined, { snatch: null }, {}]) {
    const a = athlete('a', 'Ann', { snatch: 80 }, { snatch: '2026-11-06' }, prev);
    assert.equal(count(slide(a), GREEN), 0, JSON.stringify(prev));
  }
  // A lift corrected down after its PR keeps the date but has no increase to show.
  const down = athlete('a', 'Ann', { snatch: 80 }, { snatch: '2026-11-06' }, { snatch: 85 });
  assert.equal(count(slide(down), GREEN), 0);
});

test('a name too long for double size drops to single size, and one too long for that is cut with ~', () => {
  const mid = 'the_peoples_master'; // 18 characters: 214 px at double size, 205 px of room
  assert.equal(sameRows(slide(athlete('a', mid, { total: 322 }, { total: '2026-11-05' })), 0, 19, (s) => {
    s.rect(0, 0, W - 1, 19, BANNER);
    s.text('NEW PR', 4, 6, [255, 200, 40]);
    s.text(mid, 47, 6, [255, 255, 255]);
  }), '');
  const fits = 'abcdefghijklmnopq'; // 17 characters: 202 px at double size
  assert.equal(sameRows(slide(athlete('a', fits, { total: 322 }, { total: '2026-11-05' })), 0, 19, (s) => {
    s.rect(0, 0, W - 1, 19, BANNER);
    s.text('NEW PR', 4, 6, [255, 200, 40]);
    s.text(fits, 47, 2, [255, 255, 255], { scale: 2 });
  }), '');
  const long = 'a_really_long_instagram_handle_that_wont_fit'; // 44 characters; 34 fit
  assert.equal(sameRows(slide(athlete('a', long, { total: 322 }, { total: '2026-11-05' })), 0, 19, (s) => {
    s.rect(0, 0, W - 1, 19, BANNER);
    s.text('NEW PR', 4, 6, [255, 200, 40]);
    s.text(`${long.slice(0, 33)}~`, 47, 6, [255, 255, 255]);
  }), '');
});

test('several lifts: one line each, weights in one column, and "+N more" past three', () => {
  const lifts = { snatch: 153, clean_jerk: 175.5, total: 328.5, back_squat: 200, front_squat: 170 };
  const dates = { snatch: '2026-11-05', clean_jerk: '2026-11-05', total: '2026-11-05', back_squat: '2026-11-01', front_squat: '2026-11-02' };
  const prev = { snatch: 150, clean_jerk: 163, total: 313, back_squat: null, front_squat: 169.5 };
  const a = athlete('a', 'the_peoples_master', lifts, dates, prev);
  const [entry] = recentPrs([a], 7, TODAY);
  assert.deepEqual(entry.lifts.map((l) => l.lift), ['snatch', 'clean_jerk', 'total', 'front_squat', 'back_squat']);
  const buf = renderPrSlide({ entry, index: 0, count: 1, today: TODAY });
  // Right edge of the gold digits on each of the three rows.
  const right = [26, 44, 62].map((y) => {
    for (let x = W - 1; x >= 0; x--) for (let dy = 0; dy < 16; dy++) if (px(buf, x, y + dy) === GOLD) return x;
    return -1;
  });
  assert.equal(new Set(right).size, 1, `weights line up: ${right}`);
  assert.ok(count(buf, GREEN, 0, 26, W - 1, 77) > 0);
  assert.equal(sameRows(buf, 80, 86, (s) => s.text('+2 more', 6, 80, [140, 140, 140])), '');
  // Exactly three lifts: no "more" line.
  const three = renderPrSlide({ entry: { ...entry, lifts: entry.lifts.slice(0, 3) }, index: 0, count: 1, today: TODAY });
  assert.equal(count(three, GREY, 0, 78, 120, 86), 0);
});

test('footer: when, and one dot per athlete up to the cap', () => {
  const at = (d) => athlete('a', 'Ann', { snatch: 80 }, { snatch: d });
  const when = (buf, label) => sameRows(buf, 87, 87, (s) => s.text(label, W - 4 - textWidth(label), 87, [140, 140, 140]));
  assert.equal(when(slide(at('2026-11-06')), 'today'), '');
  assert.equal(when(slide(at('2026-11-05')), 'yesterday'), '');
  assert.equal(when(slide(at('2026-11-01')), '5 days ago'), '');

  const one = slide(at('2026-11-06'));
  assert.equal(count(one, '60,60,60', 0, 89, 130, 92) + count(one, GOLD, 0, 89, 130, 92), 0, 'no dots for one athlete');
  const three = slide(at('2026-11-06'), { index: 1, count: 3 });
  assert.equal(px(three, 4, 89), '60,60,60');
  assert.equal(px(three, 10, 89), GOLD);
  assert.equal(px(three, 16, 92), '60,60,60');
  const many = slide(at('2026-11-06'), { index: MAX_PR_SCREENS - 1, count: 35 });
  assert.equal(count(many, GOLD, 0, 89, 130, 92) / 16, 1);
  assert.equal(count(many, '60,60,60', 0, 89, 130, 92) / 16, MAX_PR_SCREENS - 1);
  assert.equal(px(many, 4 + (MAX_PR_SCREENS - 1) * 6, 90), GOLD);
});
