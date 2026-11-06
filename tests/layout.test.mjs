// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import {
  W, H, LIFTS, MAX_ROWS, fitWhiteboard, renderWhiteboardPage, ruleX, sortAthletes,
  formatKg, parseKg, validSeconds, textWidth,
} from '../web/layout.js';

const athlete = (name, lifts = {}, extra = {}) => ({
  id: name, name, visible: true, pr_dates: {}, notes: '',
  snatch: null, clean_jerk: null, total: null, back_squat: null, front_squat: null, ...lifts, ...extra,
});
const many = (n) => Array.from({ length: n }, (_, i) => athlete(`lifter${i}`, { snatch: 80, clean_jerk: 100, total: 180 }));
const px = (buf, x, y) => [...buf.slice((y * W + x) * 4, (y * W + x) * 4 + 3)];
const GRID = [0, 70, 170];
const GREEN = [60, 255, 110];
const WHITE = [255, 255, 255];

test('pages are as few as possible and balanced', () => {
  const cases = [
    [0, [0]], [1, [1]], [9, [9]], [10, [5, 5]], [18, [9, 9]], [19, [7, 6, 6]], [31, [8, 8, 8, 7]], [100, [9, 9, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8]],
  ];
  for (const [n, sizes] of cases) {
    const fit = fitWhiteboard(many(n));
    assert.deepEqual(fit.pages.map((p) => p.length), sizes, `n=${n}`);
    assert.ok(fit.pages.every((p) => p.length <= MAX_ROWS));
    assert.equal(fit.pageCount, Math.max(1, Math.ceil(n / MAX_ROWS)), `n=${n} uses the fewest pages`);
  }
});

test('rows stretch to fill short pages, within 8 to 10 px', () => {
  assert.equal(fitWhiteboard(many(9)).rowPitch, 8);
  assert.equal(fitWhiteboard(many(31)).rowPitch, 9); // 8 per page
  assert.equal(fitWhiteboard(many(7)).rowPitch, 10);
  assert.equal(fitWhiteboard(many(2)).rowPitch, 10);
});

test('hidden athletes are left off the wall', () => {
  const list = many(10);
  list[3].visible = false;
  const fit = fitWhiteboard(list);
  assert.equal(fit.count, 9);
  assert.equal(fit.pageCount, 1);
});

test('columns always fill the width exactly', () => {
  for (const list of [many(5), [athlete('x')], [athlete('a'.repeat(40), { back_squat: 250.5 })]]) {
    const fit = fitWhiteboard(list);
    assert.equal(fit.nameW + 5 * fit.numW, W - 7);
    assert.equal(ruleX(fit, 6), W - 1, 'last rule is the right frame');
  }
});

test('half-kilo values fit their column with a pixel clear of each rule', () => {
  const fit = fitWhiteboard([athlete('the_peoples_master', { snatch: 153, clean_jerk: 175, total: 322, back_squat: 250.5, front_squat: 210 })]);
  assert.equal(textWidth('250.5', true), 25);
  assert.ok(fit.numW >= 27);
  assert.equal(fit.nameChars >= 18, true, 'the longest chalkboard handle fits');
  assert.deepEqual(fit.truncated, []);
});

test('short names give the spare width to the number columns', () => {
  const short = fitWhiteboard([athlete('bob', { snatch: 90 })]);
  const long = fitWhiteboard([athlete('the_peoples_master', { snatch: 90 })]);
  assert.ok(short.numW > long.numW);
});

test('names too long to fit are cut with ~ and reported', () => {
  const name = 'an_extremely_long_instagram_handle_x';
  const fit = fitWhiteboard([athlete(name, { back_squat: 250.5 })]);
  assert.deepEqual(fit.truncated, [name]);
  const buf = renderWhiteboardPage(fit, 0, { title: 'T' });
  // Nothing from the name spills over the first column rule.
  const rx = ruleX(fit, 1);
  for (let y = 21; y < 29; y++) assert.deepEqual(px(buf, rx, y), GRID, `rule intact at y=${y}`);
});

test('renders column rules, header and values where the fit says', () => {
  const fit = fitWhiteboard([athlete('rytownusa', { snatch: 90, clean_jerk: 112, total: 200, back_squat: 172, front_squat: 137 })]);
  const buf = renderWhiteboardPage(fit, 0);
  for (let i = 1; i <= 5; i++) assert.deepEqual(px(buf, ruleX(fit, i), 50), GRID);
  // Some white pixels inside every lift cell of row 1.
  for (let j = 0; j < 5; j++) {
    let lit = 0;
    for (let x = ruleX(fit, j + 1) + 1; x < ruleX(fit, j + 2); x++) for (let y = 21; y < 29; y++) if (px(buf, x, y).join() === WHITE.join()) lit++;
    assert.ok(lit > 5, `${LIFTS[j]} drawn`);
  }
});

test('blank lifts draw nothing', () => {
  const fit = fitWhiteboard([athlete('alli_garver', { snatch: 61, clean_jerk: 77, total: 138 })]);
  const buf = renderWhiteboardPage(fit, 0);
  for (let x = ruleX(fit, 4) + 1; x < ruleX(fit, 5); x++) for (let y = 21; y < 29; y++) assert.notDeepEqual(px(buf, x, y), WHITE);
});

test('a recent PR is green, an old one is not', () => {
  const today = new Date(2026, 10, 6);
  const fresh = athlete('a', { snatch: 90 }, { pr_dates: { snatch: '2026-11-03' } });
  const stale = athlete('b', { snatch: 90 }, { pr_dates: { snatch: '2026-10-01' } });
  const countGreen = (a) => {
    const fit = fitWhiteboard([a]);
    const buf = renderWhiteboardPage(fit, 0, { prDays: 7, today });
    let n = 0;
    for (let i = 0; i < buf.length; i += 4) if (buf[i] === GREEN[0] && buf[i + 1] === GREEN[1] && buf[i + 2] === GREEN[2]) n++;
    return n;
  };
  assert.ok(countGreen(fresh) > 0);
  assert.equal(countGreen(stale), 0);
});

test('empty board says so instead of drawing an empty table', () => {
  const fit = fitWhiteboard([]);
  const buf = renderWhiteboardPage(fit, 0);
  assert.equal(buf.length, W * H * 4);
  let grey = 0;
  for (let i = 0; i < buf.length; i += 4) if (buf[i] === 120 && buf[i + 1] === 120) grey++;
  assert.ok(grey > 20);
});

test('sorting', () => {
  const list = [athlete('b', { total: 150 }), athlete('A', { total: null }), athlete('_c', { total: 200 })];
  assert.deepEqual(sortAthletes(list, 'total').map((a) => a.name), ['_c', 'b', 'A']);
  assert.deepEqual(sortAthletes(list, 'name').map((a) => a.name), ['A', 'b', '_c']);
  assert.deepEqual(sortAthletes(list, 'board').map((a) => a.name), ['b', 'A', '_c']);
});

test('weights: 0.5 kg steps, blanks allowed', () => {
  assert.deepEqual(parseKg(''), { value: null });
  assert.deepEqual(parseKg('136.5'), { value: 136.5 });
  assert.deepEqual(parseKg('136,5'), { value: 136.5 });
  assert.ok(parseKg('136.3').error);
  assert.ok(parseKg('>152').error);
  assert.ok(parseKg('0').error);
  assert.ok(parseKg('501').error);
  assert.equal(formatKg(120), '120');
  assert.equal(formatKg(136.5), '136.5');
  assert.equal(formatKg(null), '');
});

test('screen time is whole seconds from 5 to 999', () => {
  for (const ok of [5, 6, 999]) assert.ok(validSeconds(ok), String(ok));
  for (const bad of [4, 1000, 7.5, NaN, '10', 0]) assert.ok(!validSeconds(bad), String(bad));
});
