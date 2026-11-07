// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { W, H, textWidth } from '../web/layout.js';
import {
  wrapLines, fitAnnouncement, renderAnnouncement, announcementActive, announcementScreens, unshownChars, plainText,
  MAX_ANNOUNCE_SCREENS,
} from '../web/announceslides.js';

const TEAL = '0,150,150';
const WHITE = '255,255,255';
const DIM = '0,90,90';
const px = (buf, x, y) => [...buf.slice((y * W + x) * 4, (y * W + x) * 4 + 3)].join();
// Bounding box of every pixel of one colour.
const box = (buf, color, y0 = 0, y1 = H - 1) => {
  let x0 = W, x1 = -1, top = H, bottom = -1;
  for (let y = y0; y <= y1; y++) {
    for (let x = 0; x < W; x++) {
      if (px(buf, x, y) !== color) continue;
      x0 = Math.min(x0, x);
      x1 = Math.max(x1, x);
      top = Math.min(top, y);
      bottom = Math.max(bottom, y);
    }
  }
  return { x0, x1, top, bottom };
};

test('wrapLines: wraps at spaces, honours line breaks, splits a word longer than a line', () => {
  assert.deepEqual(wrapLines('Welcome to The Strength Tank', 13), ['Welcome to', 'The Strength', 'Tank']);
  assert.deepEqual(wrapLines('Open gym\nSaturday', 20), ['Open gym', 'Saturday']);
  assert.deepEqual(wrapLines('a\n\nb', 20), ['a', '', 'b'], 'two breaks leave a blank line');
  assert.deepEqual(wrapLines('a    b', 20), ['a b'], 'runs of spaces collapse');
  assert.deepEqual(wrapLines('go SUPERCALIFRAGILISTIC now', 8), ['go', 'SUPERCAL', 'IFRAGILI', 'STIC now']);
  assert.deepEqual(wrapLines('abcdefgh', 8), ['abcdefgh'], 'a word exactly a line long stays whole');
  assert.deepEqual(wrapLines('ab 💪💪💪', 4), ['ab', '💪💪💪'], 'an emoji is one character');
});

test('fitAnnouncement: largest scale that fits, 3 then 2 then 1, with a 4 px margin', () => {
  const short = fitAnnouncement('Welcome to The Strength Tank', 1);
  assert.equal(short.scale, 3);
  assert.deepEqual(short.lines, ['Welcome to', 'The Strength', 'Tank']);
  const medium = fitAnnouncement('Gym closed Thursday for Thanksgiving. Open gym Friday 8 to 12.', 1);
  assert.equal(medium.scale, 2);
  const long = fitAnnouncement('Bring a friend week! Every member can bring one guest to any class from Monday to Saturday. Sign them in at the desk first and make sure they fill in the waiver.', 1);
  assert.equal(long.scale, 1);
  assert.equal(long.cut, false);
  // A 13-character word fills a scale-3 line; 14 drops to scale 2 rather than split, and a
  // word too long even for scale 2 is split at scale 1.
  assert.equal(fitAnnouncement('ABCDEFGHIJKLM', 1).scale, 3);
  assert.equal(fitAnnouncement('ABCDEFGHIJKLMN', 1).scale, 2);
  assert.deepEqual(fitAnnouncement('x'.repeat(50), 1).lines, ['x'.repeat(41), 'x'.repeat(9)]);
  // Four lines don't fit at scale 3 (4 x 27 - 3 > 88); five fit at scale 2 alone, four with dots.
  assert.equal(fitAnnouncement('a\nb\nc', 1).scale, 3);
  assert.equal(fitAnnouncement('a\nb\nc\nd', 1).scale, 2);
  assert.equal(fitAnnouncement('a\nb\nc\nd\ne', 1).scale, 2);
  assert.equal(fitAnnouncement('a\nb\nc\nd\ne', 2).scale, 1);
  for (const f of [short, medium, long]) {
    for (const line of f.lines) assert.ok(textWidth(line, false, f.scale) <= W - 8, `${line} at ${f.scale}`);
  }
});

test('fitAnnouncement: the 200-character maximum fits at scale 1; past nine lines the end is cut with ~', () => {
  const max = 'word '.repeat(40).trim().slice(0, 200);
  const fit = fitAnnouncement(max, 1);
  assert.equal(fit.scale, 1);
  assert.equal(fit.cut, false);
  const tall = ['x'.repeat(45), 'y'.repeat(45), 'z'.repeat(45), 'q', 'r', 'w', 'v'].join('\n'); // 10 lines at 41 a line
  const cut = fitAnnouncement(tall, 1);
  assert.equal(cut.cut, true);
  assert.equal(cut.lines.length, 9);
  assert.equal(cut.lines[8], 'w~');
  const cutFull = fitAnnouncement('x'.repeat(41 * 10), 1);
  assert.equal(cutFull.lines[8], 'x'.repeat(40) + '~', 'a full last line loses a character for the ~');
});

test('renderAnnouncement: teal everywhere, white text centred both ways, no dots for one', () => {
  const buf = renderAnnouncement({ text: 'Welcome to The Strength Tank', index: 0, count: 1 });
  let other = 0;
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) if (![TEAL, WHITE].includes(px(buf, x, y))) other++;
  assert.equal(other, 0, 'only teal and white');
  const b = box(buf, WHITE);
  assert.ok(b.x0 >= 4 && b.x1 <= W - 5 && b.top >= 4 && b.bottom <= H - 5, JSON.stringify(b));
  // Centred: the widest line ("The Strength", 12 characters at scale 3 = 213 px) sits mid-wall.
  assert.ok(Math.abs(b.x0 - (W - 1 - b.x1)) <= 3, JSON.stringify(b));
  // Three lines at scale 3 are 78 px tall in an 88 px area: 5 px above. Glyph tops start at
  // the line's top pixel (the 'W' of Welcome).
  assert.equal(b.top, 9);
});

test('renderAnnouncement: a dot per announcement under the text, the current one white', () => {
  const two = renderAnnouncement({ text: 'Hi', index: 1, count: 2 });
  const dots = (buf, color) => box(buf, color, 88, 91);
  // Two 4 px dots 7 px apart are 11 px wide, centred: x 122 to 132.
  assert.deepEqual(dots(two, DIM), { x0: 122, x1: 125, top: 88, bottom: 91 });
  assert.deepEqual(dots(two, WHITE), { x0: 129, x1: 132, top: 88, bottom: 91 });
  const twenty = renderAnnouncement({ text: 'Hi', index: 19, count: 20 });
  assert.deepEqual(dots(twenty, WHITE), { x0: 192, x1: 195, top: 88, bottom: 91 }, 'the last of 20 (the row is 137 px, centred)');
  const capped = renderAnnouncement({ text: 'Hi', index: 0, count: 30 });
  assert.deepEqual(capped, renderAnnouncement({ text: 'Hi', index: 0, count: MAX_ANNOUNCE_SCREENS }), 'never more than 20 dots');
  // Text stays above the dots.
  assert.ok(box(two, WHITE, 0, 84).bottom <= 84);
  const tall = renderAnnouncement({ text: 'a\nb\nc\nd\ne\nf\ng', index: 0, count: 3 });
  assert.ok(box(tall, WHITE, 0, 87).bottom <= 84, 'seven lines and dots');
});

test('renderAnnouncement: characters the font lacks draw as ?', () => {
  assert.deepEqual(renderAnnouncement({ text: '“Hi”', index: 0, count: 1 }), renderAnnouncement({ text: '?Hi?', index: 0, count: 1 }));
  assert.deepEqual(renderAnnouncement({ text: 'PR 💪', index: 0, count: 1 }), renderAnnouncement({ text: 'PR ?', index: 0, count: 1 }));
});

test('announcementActive: enabled, and dated ones only inside their range, end day included', () => {
  const a = (start_date = null, end_date = null, enabled = true) => ({ id: 'n', text: 'x', enabled, start_date, end_date });
  const today = '2026-11-06';
  assert.equal(announcementActive(a(), today), true);
  assert.equal(announcementActive(a(null, null, false), today), false);
  assert.equal(announcementActive(a('2026-11-06'), today), true, 'starts today');
  assert.equal(announcementActive(a('2026-11-07'), today), false, 'starts tomorrow');
  assert.equal(announcementActive(a(null, '2026-11-06'), today), true, 'ends today: still on');
  assert.equal(announcementActive(a(null, '2026-11-05'), today), false, 'ended yesterday');
  assert.equal(announcementActive(a('2026-11-01', '2026-11-30'), today), true);
  assert.equal(announcementActive(a('2026-11-06', '2026-11-06'), today), true, 'a one-day range');
  assert.equal(announcementActive(a('2026-10-31', '2026-11-01'), '2026-11-01'), true, 'across a month end');
  assert.equal(announcementActive(a('2026-12-31', '2027-01-01'), '2027-01-01'), true, 'across a year end');
  // No clock: undated still show, dated don't.
  for (const noClock of [null, '', 'garbage']) {
    assert.equal(announcementActive(a(), noClock), true);
    assert.equal(announcementActive(a('2026-11-01'), noClock), false);
    assert.equal(announcementActive(a(null, '2026-12-01'), noClock), false);
  }
});

test('announcementScreens: list order, active only, at most 20', () => {
  const list = Array.from({ length: 30 }, (_, i) => ({ id: `n${i}`, text: `x${i}`, enabled: i !== 1, start_date: i === 2 ? '2027-01-01' : null, end_date: null }));
  const shown = announcementScreens(list, '2026-11-06');
  assert.equal(shown.length, MAX_ANNOUNCE_SCREENS);
  assert.deepEqual(shown.slice(0, 3).map((a) => a.id), ['n0', 'n3', 'n4']);
  assert.equal(shown.at(-1).id, 'n21');
  assert.equal(announcementScreens([], '2026-11-06').length, 0);
});

test('unshownChars and plainText: warn about what the wall can’t draw and fix the common ones', () => {
  assert.deepEqual(unshownChars('Plain ASCII\nwith a heart ♥'), []);
  assert.deepEqual(unshownChars('Café “PR” day — 💪💪\ttab'), ['é', '“', '”', '—', '💪', '\t']);
  assert.equal(plainText('“Don’t” – wait… ‘ok’ — café'), '"Don\'t" - wait... \'ok\' - café');
  assert.deepEqual(unshownChars(plainText('“Hi” — …')), []);
});
