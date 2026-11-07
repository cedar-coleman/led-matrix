// Writes the pixel-parity fixtures for the native tests (test/test_parity).
//
// The renderers in web/ are the spec. This script runs them on a fixed set of inputs and
// records what they draw; the C++ port in lib/render must draw the same bytes.
//
// Run from firmware/:  node test/parity/gen_golden.mjs
// Rerun whenever web/layout.js, web/igslides.js, web/prslides.js, web/announceslides.js, web/power.js
// or web/font5x7.js change, then `pio test -e native`.
//
// Output: test/parity/fixtures/manifest.json (inputs and expected values) and one
// run-length-encoded RGB frame per screen in fixtures/frames/. Run-length encoding keeps
// ~100 frames, mostly black, to a few hundred KB instead of 7 MB.

// Date-times without an offset are local time in JS. The wall treats them as UTC, so the
// fixtures are made in UTC. Set before the first Date is built.
process.env.TZ = 'UTC';

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '../../..');
const web = (f) => path.join(root, 'web', f);

const layout = await import(web('layout.js'));
const ig = await import(web('igslides.js'));
const pr = await import(web('prslides.js'));
const ann = await import(web('announceslides.js'));
const power = await import(web('power.js'));
const { FONT5X7 } = await import(web('font5x7.js'));

const { W, H, LIFTS, fitWhiteboard, renderWhiteboardPage, sortAthletes } = layout;
const { renderStorySlide, renderPostsSlide, STORY_W, STORY_H, POST_SIZE } = ig;

const out = path.join(here, 'fixtures');
fs.rmSync(out, { recursive: true, force: true });
fs.mkdirSync(path.join(out, 'frames'), { recursive: true });
fs.mkdirSync(path.join(out, 'tiles'), { recursive: true });

let frameCount = 0;
let rawBytes = 0;
let rleBytes = 0;

/** RGBA frame -> RLE file of [run 1..255][r][g][b]. Returns the file name. */
function writeFrame(name, rgba) {
  if (rgba.length !== W * H * 4) throw new Error(`${name}: frame is ${rgba.length} bytes`);
  const bytes = [];
  for (let i = 0; i < W * H; ) {
    const o = i * 4;
    if (rgba[o + 3] !== 255) throw new Error(`${name}: alpha ${rgba[o + 3]} at pixel ${i}`);
    let run = 1;
    while (run < 255 && i + run < W * H) {
      const p = (i + run) * 4;
      if (rgba[p] !== rgba[o] || rgba[p + 1] !== rgba[o + 1] || rgba[p + 2] !== rgba[o + 2]) break;
      run++;
    }
    bytes.push(run, rgba[o], rgba[o + 1], rgba[o + 2]);
    i += run;
  }
  const file = `${name}.rle`;
  fs.writeFileSync(path.join(out, 'frames', file), Uint8Array.from(bytes));
  frameCount++;
  rawBytes += W * H * 3;
  rleBytes += bytes.length;
  return file;
}

/** Doubles go through the manifest as their exact bits; no JSON float parser in the loop. */
function bits(x) {
  const b = new DataView(new ArrayBuffer(8));
  b.setFloat64(0, x);
  return b.getBigUint64(0).toString(16).padStart(16, '0');
}

// ------------------------------------------------------------------ tiles

/** RGB565 LE tile with a pattern that touches every bit of every channel. */
function makeTile(name, w, h, seed) {
  const bytes = new Uint8Array(w * h * 2);
  let s = seed >>> 0;
  for (let i = 0; i < w * h; i++) {
    s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
    const x = i % w;
    const y = Math.floor(i / w);
    // Smooth gradients with noise in the low bits, like a photo.
    const r = (x * 31 / (w - 1)) | 0;
    const g = ((y * 63) / (h - 1)) ^ (s >>> 29);
    const b = (s >>> 27) & 31;
    const v = (r << 11) | ((g & 63) << 5) | b;
    bytes[i * 2] = v & 255;
    bytes[i * 2 + 1] = v >> 8;
  }
  fs.writeFileSync(path.join(out, 'tiles', name), bytes);
  return { name, bytes, w, h };
}

/** Same expansion as rgbFrom565 in lib/render/src/surface.h. */
function tileRgba({ bytes, w, h }) {
  const rgba = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < w * h; i++) {
    const v = bytes[i * 2] | (bytes[i * 2 + 1] << 8);
    const r = (v >> 11) & 31;
    const g = (v >> 5) & 63;
    const b = v & 31;
    rgba.set([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255], i * 4);
  }
  return rgba;
}

const tiles = {};
for (let i = 0; i < 5; i++) tiles[`story${i}.565`] = makeTile(`story${i}.565`, STORY_W, STORY_H, 100 + i);
for (let i = 0; i < 3; i++) tiles[`post${i}.565`] = makeTile(`post${i}.565`, POST_SIZE, POST_SIZE, 200 + i);
const tile = (name) => (name ? tileRgba(tiles[name]) : null);

// ------------------------------------------------------------------ athletes

function parseCsv(text) {
  const rows = [];
  let row = [];
  let cell = '';
  let quoted = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text[i];
    if (quoted) {
      if (ch === '"' && text[i + 1] === '"') { cell += '"'; i++; }
      else if (ch === '"') quoted = false;
      else cell += ch;
    } else if (ch === '"') quoted = true;
    else if (ch === ',') { row.push(cell); cell = ''; }
    else if (ch === '\n') { row.push(cell); rows.push(row); row = []; cell = ''; }
    else if (ch !== '\r') cell += ch;
  }
  if (cell || row.length) { row.push(cell); rows.push(row); }
  return rows;
}

const athlete = (id, name, kg = [], extra = {}) => {
  const a = { id, name, visible: true, pr_dates: {} };
  LIFTS.forEach((lift, i) => (a[lift] = kg[i] ?? null));
  return Object.assign(a, extra);
};

const csv = parseCsv(fs.readFileSync(path.join(root, 'athletes-from-chalkboard.csv'), 'utf8'));
const chalkboard = csv.slice(1).filter((r) => r[0]).map((r, i) =>
  athlete(`cb${i}`, r[0], r.slice(1, 6).map((v) => (v.trim() === '' ? null : Number(v)))));

const TODAY = '2026-11-06';
// PR dates around the 7-day window, including V8's lenient dates.
const withPrs = chalkboard.map((a) => ({ ...a, pr_dates: {} }));
const prDates = [
  [0, 'snatch', TODAY], [0, 'total', '2026-11-05'], [1, 'clean_jerk', '2026-10-31'], // 6 days: green
  [2, 'snatch', '2026-10-30'], // 7 days: not green
  [3, 'back_squat', '2026-11-07'], // tomorrow: not green
  [4, 'front_squat', '2026-11-02'], [5, 'total', 'garbage'], [6, 'snatch', '2026-02-30'],
  [9, 'clean_jerk', '2026-11'], [10, 'snatch', '2026-11-06'], [10, 'clean_jerk', '2026-11-06'],
  [10, 'total', '2026-11-06'], [10, 'back_squat', '2026-11-06'], [10, 'front_squat', '2026-11-06'],
  [20, 'total', '2026-11-03'], [30, 'front_squat', '2026-11-01'],
];
for (const [i, lift, d] of prDates) withPrs[i].pr_dates[lift] = d;

const lifter = (i) => athlete(`l${i}`, `lifter${i}`, [
  40 + (i % 90), 50 + ((i * 7) % 120), i % 5 === 0 ? null : 100 + ((i * 3) % 200) + (i % 2 ? 0.5 : 0),
  i % 11 === 0 ? null : 60 + ((i * 13) % 250), 0.5 + (i % 3),
]);
const many = (n) => Array.from({ length: n }, (_, i) => lifter(i));

const edgy = [
  athlete('e0', 'a_really_long_instagram_handle_that_will_not_fit', [499.5, 500, 0.5, null, 136.5]),
  athlete('e1', 'Zoë Ångström ♥', [0.5, 1, 1.5, 2, 2.5]),
  athlete('e2', '💪 strong one', [100, null, null, null, 250.5]),
  athlete('e3', 'hidden lifter', [90, 90, 180, 150, 120], { visible: false }),
  athlete('e4', '   ', [90, 90, 180, 150, 120]), // blank name: left off, as in app.js
  athlete('e5', 'no lifts at all'),
  athlete('e6', 'x', [72.5, 0.5, 73, 999 / 2, 12.5]),
  athlete('e7', 'tab\tand control\u0001', [1, 2, 3, 4, 5]),
];

const sortNames = [
  '@alpha', '_beta', '.gamma', '@_.delta', 'Alpha2', 'alpha-1', 'alpha 1', 'alpha_1', 'alpha.1', 'alpha1',
  'Ábc', 'abc', 'Brad B.', 'brad b.', 'zed', 'ZED', '9lives', '$money', '~tilde', 'a(b)', 'a[b]', 'a{b}',
  'a♥b', 'a💪b', 'aéb', 'aeb', 'a×b', 'a\u00a0b', 'µ', 'ß', 'αβ', 'þorn', 'a¢b', 'a£b', 'a¡b', 'a¿b', 'a«b', 'a§b', 'a°b', 'a—b', 'a’b', 'a€b', 'a→b', 'a★b',
  'a!b', 'a?b', 'a#b', 'a%b', 'a&b', 'a*b', 'a+b', 'a=b', 'a<b', 'a>b', 'a|b', 'a/b', 'a\\b', 'a^b', 'a`b',
  "a'b", 'a"b', 'a,b', 'a;b', 'a:b', 'a@b', 'ab',
].map((n, i) => athlete(`s${i}`, n, [i, i, (i * 37) % 11 === 0 ? null : (i * 37) % 11, i, i]));

const totals = [
  athlete('t0', 'first', [1, 1, 200]), athlete('t1', 'blank total', [1, 1, null]), athlete('t2', 'tie a', [1, 1, 150.5]),
  athlete('t3', 'top', [1, 1, 322]), athlete('t4', 'tie b', [1, 1, 150.5]), athlete('t5', 'blank too', [1, 1, null]),
  athlete('t6', 'tiny', [1, 1, 0.5]),
];

// ------------------------------------------------------------------ whiteboard

const whiteboard = [];
function wbCase(name, athletes, { sort = 'board', title = 'THE STRENGTH TANK', prDays = 7, today = TODAY } = {}) {
  // Wall time invalid ("" today) must look like highlighting off.
  const jsPrDays = today === '' ? 0 : prDays;
  const named = athletes.filter((a) => a.name.trim());
  const fit = fitWhiteboard(sortAthletes(named, sort));
  const [y, m, d] = (today || '2000-01-01').split('-').map(Number);
  const opts = { title, prDays: jsPrDays, today: new Date(y, m - 1, d) };
  const frames = [];
  for (let p = 0; p < fit.pageCount; p++) frames.push(writeFrame(`wb_${name}_p${p}`, renderWhiteboardPage(fit, p, opts)));
  // One page past the end draws the frame with no rows, as `pages[i] ?? []` does.
  const beyond = writeFrame(`wb_${name}_beyond`, renderWhiteboardPage(fit, fit.pageCount, opts));
  whiteboard.push({
    name, athletes, sort, title, prDays, today, frames, beyond,
    fit: {
      pageCount: fit.pageCount, perPage: fit.perPage, rowPitch: fit.rowPitch, valW: fit.valW, numW: fit.numW,
      nameW: fit.nameW, nameChars: fit.nameChars, truncated: fit.truncated, count: fit.count,
      pages: fit.pages.map((pg) => pg.map((a) => a.id)),
    },
  });
}

for (const sort of ['board', 'total', 'name']) wbCase(`chalkboard_${sort}`, withPrs, { sort });
wbCase('chalkboard_no_clock', withPrs, { today: '' });
wbCase('chalkboard_prs_off', withPrs, { prDays: 0 });
wbCase('chalkboard_prs_30d', withPrs, { prDays: 30, today: '2026-11-30' });
for (const n of [0, 1, 9, 10, 31, 100]) wbCase(`count_${n}`, many(n));
wbCase('count_100_total', many(100), { sort: 'total' });
wbCase('edges', edgy);
wbCase('edges_name', edgy, { sort: 'name' });
wbCase('sort_names', sortNames, { sort: 'name' });
wbCase('sort_totals', totals, { sort: 'total' });
wbCase('long_title', chalkboard.slice(0, 12), { title: 'THE STRENGTH TANK · SAN DIEGO · PR BOARD 2026 EDITION ♥' });
wbCase('empty_title', chalkboard.slice(0, 3), { title: '' });
wbCase('all_hidden', chalkboard.slice(0, 5).map((a) => ({ ...a, visible: false })));

// ------------------------------------------------------------------ instagram

const NOW = Date.parse('2026-11-06T20:00:00Z') / 1000;
const ages = [
  '2026-11-06T19:59:40Z', '2026-11-06T19:48:00Z', '2026-11-06T17:00:00+0000', '2026-11-04T20:00:00Z',
  '2026-10-01T20:00:00Z', '2026-11-07T20:00:00Z', 'garbage', '2026-11-06T18:30:00.250-01:30', '2026-11-06',
  '2026-11-06 12:00:00',
];

const stories = [];
function storyCase(name, { username = 'thestrengthtank', story = null, index = 0, count = 1, progress = 0, now = NOW }) {
  const jsStory = story ? { tile: tile(story.tile), timestamp: story.timestamp } : null;
  const frame = writeFrame(`story_${name}`, renderStorySlide({ username, story: jsStory, index, count, progress, now: new Date(now * 1000) }));
  stories.push({ name, username, story, index, count, progress: bits(progress), now, frame });
}

storyCase('one', { story: { tile: 'story0.565', timestamp: ages[1] } });
for (let i = 0; i < 5; i++) {
  storyCase(`five_${i}`, { story: { tile: `story${i}.565`, timestamp: ages[i] }, index: i, count: 5, progress: i / 4 });
}
storyCase('three_mid', { story: { tile: 'story1.565', timestamp: ages[2] }, index: 1, count: 3, progress: 0.37 });
storyCase('progress_over', { story: { tile: 'story2.565', timestamp: ages[3] }, index: 2, count: 3, progress: 1.7 });
storyCase('progress_under', { story: { tile: 'story2.565', timestamp: ages[3] }, index: 0, count: 3, progress: -0.2 });
storyCase('progress_round', { story: { tile: 'story2.565', timestamp: ages[0] }, index: 0, count: 2, progress: 0.5 });
storyCase('no_tile', { story: { tile: null, timestamp: ages[5] }, index: 0, count: 2, progress: 0.9 });
storyCase('null_story', { story: null, index: 3, count: 2 });
storyCase('zero_count', { story: null, index: 0, count: 0 });
storyCase('many', { story: { tile: 'story3.565', timestamp: ages[7] }, index: 40, count: 90, progress: 0.5 });
storyCase('long_user', { username: 'a_very_long_account_name_here', story: { tile: 'story4.565', timestamp: ages[6] } });
storyCase('ages_tz', { story: { tile: 'story4.565', timestamp: ages[8] }, count: 1 });
storyCase('ages_space', { story: { tile: 'story4.565', timestamp: ages[9] }, count: 1 });

const posts = [];
function postsCase(name, list, { username = 'thestrengthtank', now = NOW } = {}) {
  const jsPosts = list.map((p) => {
    const q = { tile: tile(p.tile), timestamp: p.timestamp, kind: p.kind };
    if (p.like_count !== undefined) q.like_count = p.like_count;
    return q;
  });
  const frame = writeFrame(`posts_${name}`, renderPostsSlide({ username, posts: jsPosts, now: new Date(now * 1000) }));
  posts.push({ name, username, posts: list, now, frame });
}

const post = (tileName, kind, like_count, timestamp) => ({ tile: tileName, kind, like_count, timestamp });
postsCase('reel_image', [post('post0.565', 'reel', 214, ages[3]), post('post1.565', 'image', 87, ages[4])]);
postsCase('video_carousel', [post('post2.565', 'video', 999, ages[1]), post('post0.565', 'carousel', 1000, ages[2])]);
postsCase('likes_k', [post('post1.565', 'image', 1050, ages[0]), post('post2.565', 'carousel', 1250, ages[5])]);
postsCase('likes_big', [post('post0.565', 'reel', 9999, ages[6]), post('post1.565', 'image', 15500, ages[7])]);
postsCase('likes_round', [post('post0.565', 'image', 10499, ages[8]), post('post1.565', 'image', 0, ages[9])]);
postsCase('unknown_likes', [post('post2.565', 'video', undefined, ages[2]), post(null, 'image', 5, ages[3])]);
postsCase('one_post', [post('post1.565', 'carousel', 42, ages[1])]);
postsCase('three_posts', [post('post0.565', 'image', 1, ages[1]), post('post1.565', 'image', 2, ages[2]), post('post2.565', 'image', 3, ages[3])]);
postsCase('no_posts', []);
postsCase('user_15', [post('post0.565', 'image', 1150, ages[1])], { username: 'abcdefghijklmno' });
postsCase('user_16', [post('post0.565', 'image', 1150, ages[1])], { username: 'abcdefghijklmnop' });
postsCase('user_long', [post('post0.565', 'image', 1150, ages[1])], { username: 'abcdefghijklmnopqrstu' });
postsCase('user_emoji', [post('post0.565', 'image', 1150, ages[1])], { username: 'gym💪tank' });

// ------------------------------------------------------------------ new PRs

const prs = [];
const day = (s) => {
  if (!s) return null; // the wall without a clock
  const [y, m, d] = s.split('-').map(Number);
  return new Date(y, m - 1, d);
};
// recentPrs on the inputs, then a frame per entry (index i of the capped count), plus any
// `extra` renders with their own index and count for the dot row.
function prCase(name, athletes, { days = 7, today = TODAY, extra = [] } = {}) {
  const list = pr.recentPrs(athletes, days, day(today));
  const count = Math.min(list.length, pr.MAX_PR_SCREENS);
  const render = (entry, index, n, tag) =>
    ({ entry, index, count: n, frame: writeFrame(`prs_${name}_${tag}`, pr.renderPrSlide({ entry: list[entry], index, count: n, today: day(today) })) });
  const frames = list.slice(0, count).map((_, i) => render(i, i, count, i));
  extra.forEach(([entry, index, n], k) => frames.push(render(entry, index, n, `x${k}`)));
  prs.push({
    name, athletes, days, today, frames,
    list: list.map((e) => ({ id: e.athlete.id, newest: e.newest, lifts: e.lifts.map((l) => ({ lift: l.lift, date: l.date })) })),
  });
}

const withPrev = (a, prev) => ({ ...a, pr_prev: prev });
const lifted = (id, name, kg, dates, prev, extra = {}) => withPrev(athlete(id, name, kg, { pr_dates: dates, ...extra }), prev);

prCase('single', [lifted('p0', 'Brad B.', [95.5, 130, 225, 180, 155], { snatch: TODAY }, { snatch: 93 })]);
prCase('multiple', [lifted('p0', 'the_peoples_master', [153, 175, 322, 250.5, 210],
  { snatch: '2026-11-05', clean_jerk: '2026-11-05', total: '2026-11-05' }, { snatch: 150, clean_jerk: 172, total: 318 })]);
prCase('mixed_widths', [lifted('p0', 'jedthefred', [92, 114.5, 206, 182, 145],
  { snatch: TODAY, clean_jerk: TODAY, back_squat: '2026-11-04' }, { snatch: 89, clean_jerk: 102, back_squat: 177.5 })]);
prCase('two_lifts', [lifted('p0', 'Josh S.', [122, 151, 267, 180, 155], { total: TODAY, front_squat: TODAY }, { total: 265.5, front_squat: null })]);
prCase('more_than_3', [lifted('p0', 'chrisajuan', [112, 140, 252, 235, 195],
  { snatch: '2026-11-02', clean_jerk: '2026-11-05', total: '2026-11-05', back_squat: TODAY, front_squat: '2026-11-03' },
  { snatch: 110, clean_jerk: 137.5, total: 247.5, back_squat: 230, front_squat: 190 })]);
prCase('long_names', [
  lifted('n0', 'abcdefghijklmnopq', [80], { snatch: TODAY }, { snatch: 79.5 }), // 17: still double size
  lifted('n1', 'abcdefghijklmnopqr', [80], { snatch: TODAY }, { snatch: 79.5 }), // 18: single size
  lifted('n2', 'a_really_long_instagram_handle_that_wont_fit', [80], { snatch: TODAY }, { snatch: 79.5 }),
  lifted('n3', 'Zoë Ångström ♥ 💪', [80, 90], { snatch: TODAY, clean_jerk: TODAY }, { snatch: 70, clean_jerk: 85 }),
  lifted('n4', 'a💪b💪c💪d💪e💪f💪g💪h💪i💪j💪k💪l💪m💪n💪o💪p💪q💪', [80], { snatch: TODAY }, {}), // cut mid-surrogate
]);
prCase('unknown_prev', [
  lifted('u0', 'no record', [80], { snatch: TODAY }, undefined),
  lifted('u1', 'was blank', [80], { snatch: TODAY }, { snatch: null }),
  lifted('u2', 'corrected down', [80], { snatch: TODAY }, { snatch: 85 }),
  lifted('u3', 'equal', [80], { snatch: TODAY }, { snatch: 80 }),
  lifted('u4', 'some known', [80, 100, 180], { snatch: TODAY, clean_jerk: TODAY, total: TODAY }, { clean_jerk: 97.5 }),
]);
prCase('half_kilo', [
  lifted('h0', 'tiny', [0.5], { snatch: TODAY }, { snatch: null }),
  lifted('h1', 'half up', [72.5, 250.5, 499.5], { snatch: TODAY, clean_jerk: TODAY, total: TODAY },
    { snatch: 72, clean_jerk: 249.5, total: 450 }),
  lifted('h2', 'big half', [null, null, null, 499.5], { back_squat: TODAY }, { back_squat: 499 }),
  lifted('h3', 'max', [null, null, null, null, 500], { front_squat: TODAY }, { front_squat: 0.5 }),
]);
prCase('window_edges', [
  lifted('w0', 'six days', [80], { snatch: '2026-10-31' }, { snatch: 78 }),
  lifted('w1', 'seven days', [80], { snatch: '2026-10-30' }, { snatch: 78 }),
  lifted('w2', 'tomorrow', [80], { snatch: '2026-11-07' }, { snatch: 78 }),
  lifted('w3', 'hidden', [80], { snatch: TODAY }, { snatch: 78 }, { visible: false }),
  lifted('w4', '   ', [80], { snatch: TODAY }, { snatch: 78 }),
  lifted('w5', 'cleared', [null], { snatch: TODAY }, { snatch: 78 }),
  lifted('w6', 'lenient date', [80, 90], { snatch: '2026-02-30', clean_jerk: '2026-11-01' }, {}),
  lifted('w7', 'garbage date', [80], { snatch: 'garbage' }, {}),
]);
prCase('window_edges_1day', prs.at(-1).athletes, { days: 1 });
prCase('window_edges_60', prs.at(-1).athletes, { days: 60, today: '2026-12-15' });
prCase('no_clock', prs[0].athletes, { today: '' });
prCase('prs_off', prs[0].athletes, { days: 0 });
prCase('chalkboard', withPrs.map((a, i) => withPrev(a, i % 3 ? { snatch: 50, total: 100, clean_jerk: null } : undefined)));
const crowd = Array.from({ length: 25 }, (_, i) => lifted(`m${i}`, `lifter${i}`, [40 + i, 60 + i],
  { snatch: `2026-11-0${6 - (i % 6)}`, ...(i % 4 ? {} : { clean_jerk: '2026-11-01' }) }, { snatch: 39.5 + i - (i % 3) }));
prCase('many', crowd, { extra: [[3, 22, 25], [0, 0, 2], [5, 39, 40]] });

// ------------------------------------------------------------------ power

const powerCases = [];
function solid(c, pick = () => true) {
  const f = new Uint8ClampedArray(W * H * 4);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4;
      f.set(pick(x, y) ? [...c, 255] : [0, 0, 0, 255], o);
    }
  }
  return f;
}
function noise(seed) {
  const f = new Uint8ClampedArray(W * H * 4);
  let s = seed;
  for (let i = 0; i < W * H; i++) {
    s = (Math.imul(s, 1103515245) + 12345) >>> 0;
    f.set([s >>> 24, (s >>> 16) & 255, (s >>> 8) & 255, 255], i * 4);
  }
  return f;
}
const powerFrames = {
  white: solid([255, 255, 255]),
  supply_a_only: solid([255, 255, 255], (x, y) => y < 32 || (y < 64 && x >= 128)),
  supply_b_only: solid([255, 255, 255], (x, y) => !(y < 32 || (y < 64 && x >= 128))),
  teal: solid([0, 150, 150]),
  noise: noise(7),
  whiteboard: renderWhiteboardPage(fitWhiteboard(chalkboard), 0, { today: new Date(2026, 10, 6) }),
  posts: renderPostsSlide({ username: 'thestrengthtank', posts: [{ tile: tile('post0.565'), timestamp: ages[1], kind: 'reel', like_count: 3 }], now: new Date(NOW * 1000) }),
};
for (const [name, rgba] of Object.entries(powerFrames)) {
  const frame = writeFrame(`power_${name}`, rgba);
  const estimates = [100, 75, 50, 25, 5, 33.3].map((pct) => {
    const { a, b } = power.estimateSupplyAmps(rgba, pct);
    return { pct: bits(pct), a: bits(a), b: bits(b) };
  });
  const limited = [100, 90, 52, 50, 25, 5].map((pct) => ({ pct, value: power.limitedBrightness(rgba, pct) }));
  const peak = Math.max(...Object.values(power.estimateSupplyAmps(rgba, 100)));
  const slide = [100, 60, 30, 5].map((pct) => ({ pct, value: power.slideBrightness(peak, pct) }));
  powerCases.push({ name, frame, estimates, limited, peak: bits(peak), slide });
}
// settings.power: the same frames on one supply and on other ratings, plus the default
// written out, which must match the two-supply cases above exactly.
const powerConfigs = [
  { supplies: 2, rated_a: 18, budget_pct: 85 },
  { supplies: 1, rated_a: 80, budget_pct: 85 },
  { supplies: 1, rated_a: 40, budget_pct: 90 },
  { supplies: 1, rated_a: 18, budget_pct: 85 },
  { supplies: 1, rated_a: 5, budget_pct: 50 },
  { supplies: 2, rated_a: 30.5, budget_pct: 70 },
];
const powerConfigCases = powerConfigs.map((cfg) => ({
  supplies: cfg.supplies,
  rated_a: bits(cfg.rated_a),
  budget_pct: cfg.budget_pct,
  budget: bits(power.supplyBudgetA(cfg)),
  frames: Object.entries(powerFrames).map(([name, rgba]) => {
    const estimates = [100, 50, 33.3, 5].map((pct) => {
      const { a, b } = power.estimateSupplyAmps(rgba, pct, cfg);
      return { pct: bits(pct), a: bits(a), b: bits(b) };
    });
    const limited = [100, 90, 61, 52, 26, 5].map((pct) => ({ pct, value: power.limitedBrightness(rgba, pct, cfg) }));
    // peak_a is always the busier half on the two-supply split, whatever the supplies.
    const peak = Math.max(...Object.values(power.estimateSupplyAmps(rgba, 100)));
    const slide = [100, 60, 30, 5].map((pct) => ({ pct, value: power.slideBrightness(peak, pct, cfg) }));
    return { name, frame: `power_${name}.rle`, estimates, limited, slide };
  }),
  slideOnly: [[0, 80], [1e-9, 80], [7.65, 100], [14.7, 100], [29.4, 100], [34, 100], [3.1, 37]]
    .map(([peak, pct]) => ({ peak: bits(peak), pct, value: power.slideBrightness(peak, pct, cfg) })),
}));

const slideOnly = [
  [0, 80], [-1, 80], [1e-9, 80], [1, 75], [power.SUPPLY_BUDGET_A, 100], [power.SUPPLY_BUDGET_A * 2, 80],
  [29.4, 100], [14.7, 100], [15.300000000000001, 100], [7.65, 100], [3.1, 37],
].map(([peak, pct]) => ({ peak: bits(peak), pct, value: power.slideBrightness(peak, pct) }));

// ------------------------------------------------------------------ announcements

const announce = [];
// One frame per case, with the fit the JS chose, so a mismatch says whether the scale, the
// wrapping or the drawing differs.
function annCase(name, text, { index = 0, count = 1 } = {}) {
  const fit = ann.fitAnnouncement(text, count);
  const frame = writeFrame(`ann_${name}`, ann.renderAnnouncement({ text, index, count }));
  announce.push({ name, text, index, count, frame, scale: fit.scale, lines: fit.lines, cut: fit.cut });
}
annCase('short_scale3', 'Welcome to The Strength Tank');
annCase('medium_scale2', 'Gym closed Thursday for Thanksgiving. Open gym Friday 8 to 12.');
annCase('long_scale1', 'Bring a friend week! Every member can bring one guest to any class from Monday to Saturday. Sign them in at the desk first and make sure they fill in the waiver.');
annCase('line_breaks', 'OPEN GYM\nSaturday 8 to 12\n\nBring a friend');
annCase('line_breaks_dots', 'OPEN GYM\nSaturday 8 to 12\n\nBring a friend', { index: 1, count: 3 });
annCase('six_breaks', 'Mon\nTue\nWed\nThu\nFri\nSat\nSun');
annCase('word_13', 'INTERNATIONAL day'); // 13 letters: still scale 3
annCase('word_14', 'CONGRATULATION'); // 14: scale 2 rather than split
annCase('word_split', 'Pneumonoultramicroscopicsilicovolcanoconiosis is a long word'); // 45: split at scale 1
annCase('word_split_mid', 'go ' + 'x'.repeat(90) + ' now');
annCase('max_200', 'Huge thanks to everyone who came out for the charity deadlift on Saturday. We raised over four thousand dollars for the food bank, and eleven of you set lifetime PRs. Photos are up on our Instagram now.');
annCase('max_200_one_word', 'W'.repeat(200));
annCase('cut', ['x'.repeat(45), 'y'.repeat(45), 'z'.repeat(45), 'q', 'r', 'w', 'v'].join('\n'));
annCase('cut_dots', 'word '.repeat(40).trim(), { index: 2, count: 4 });
annCase('non_ascii', 'Caf\u00e9 \u201cPR\u201d day \u2014 \ud83d\udcaa \u2665 Zo\u00eb\tok');
annCase('emoji_long', '\ud83d\udcaa'.repeat(60));
annCase('spaces', '   lots    of   spaces   ');
annCase('empty', '');
annCase('dots_1', 'One of one', { index: 0, count: 1 });
annCase('dots_2', 'Second of two', { index: 1, count: 2 });
annCase('dots_20_first', 'First of twenty', { index: 0, count: 20 });
annCase('dots_20_last', 'Last of twenty', { index: 19, count: 20 });
annCase('dots_capped', 'More than twenty', { index: 22, count: 25 });

// Which announcements show, for a range of dates and no clock.
const annList = [
  ['undated', null, null], ['off', null, null, false], ['from_6th', '2026-11-06', null], ['from_7th', '2026-11-07', null],
  ['to_6th', null, '2026-11-06'], ['to_5th', null, '2026-11-05'], ['nov', '2026-11-01', '2026-11-30'],
  ['one_day', '2026-11-06', '2026-11-06'], ['new_year', '2026-12-31', '2027-01-01'],
].map(([id, start_date, end_date, enabled = true]) => ({ id, text: id, enabled, start_date, end_date }));
const crowdAnn = Array.from({ length: 25 }, (_, i) => ({ id: `c${i}`, text: `c${i}`, enabled: i % 7 !== 3, start_date: null, end_date: null }));
const announceActive = [
  ...['2026-11-06', '2026-11-05', '2026-11-07', '2026-11-30', '2026-12-01', '2026-12-31', '2027-01-01', '2027-01-02', '', 'garbage', '2026-11-6']
    .map((today) => ({ today, list: annList, ids: ann.announcementScreens(annList, today || null).map((a) => a.id) })),
  { today: '2026-11-06', list: crowdAnn, ids: ann.announcementScreens(crowdAnn, '2026-11-06').map((a) => a.id) },
];

// ------------------------------------------------------------------ small functions

const formatKg = [0.5, 1, 72.5, 136.5, 250.5, 499.5, 500, 100.25, 72.3, 0.05, 1.15, 1.25]
  .map((v) => ({ v: bits(v), s: layout.formatKg(v) }));
const parse = [
  ...ages, '2026-02-30T00:00:00Z', '2026-02-31T00:00:00Z', '2026-13-01T00:00:00Z', '2026-1-05T00:00:00Z',
  '2026-11T00:00:00Z', 'T00:00:00Z', '', '2026', '2026-11-05T14:03', '2026-11-05T14:03:22.1Z',
  '2026-11-05T14:03:22.123456Z', '2026-11-05T24:00:00Z', '2026-11-05T14:03:22+05:45', '1969-12-31T23:59:59Z',
  '2026-11-05T14:03:22+0000x',
].map((s) => ({ s, ms: Number.isNaN(Date.parse(s)) ? null : Date.parse(s) }));
const formatAge = ages.flatMap((t) => [NOW, NOW + 86400 * 400, NOW - 3600].map((now) => ({ t, now, s: ig.formatAge(t, new Date(now * 1000)) })));
const textWidths = ['', 'a', 'Name', '250.5', '♥ 1.2k', '💪', 'Zoë', 'tab\t'].flatMap((s) =>
  [[false, 1], [true, 1], [false, 2]].map(([narrow, scale]) => ({ s, narrow, scale, w: layout.textWidth(s, narrow, scale) })));

const printable = [];
for (let c = 32; c < 127; c++) printable.push(String.fromCharCode(c).toLowerCase());
const collation = [...new Set(printable)].sort((a, b) => a.localeCompare(b)).join('');

const manifest = {
  generated_by: 'test/parity/gen_golden.mjs',
  font: Array.from(FONT5X7),
  collation,
  constants: {
    W, H, PANEL_FULL_WHITE_A: bits(power.PANEL_FULL_WHITE_A), SUPPLY_RATED_A: bits(power.SUPPLY_RATED_A),
    SUPPLY_BUDGET_A: bits(power.SUPPLY_BUDGET_A), STORY_W, STORY_H, POST_SIZE, MAX_ROWS: layout.MAX_ROWS,
    MIN_SECONDS: layout.MIN_SECONDS, MAX_SECONDS: layout.MAX_SECONDS, MAX_PR_SCREENS: pr.MAX_PR_SCREENS,
    MAX_ANNOUNCE_SCREENS: ann.MAX_ANNOUNCE_SCREENS,
  },
  whiteboard, stories, posts, prs, power: powerCases, slideOnly, powerConfigs: powerConfigCases, formatKg, parse, formatAge,
  textWidths, announce, announceActive,
};
fs.writeFileSync(path.join(out, 'manifest.json'), JSON.stringify(manifest));

const counts = {
  frames: frameCount, whiteboard: whiteboard.length, stories: stories.length, posts: posts.length, prs: prs.length,
  announce: announce.length, announceActive: announceActive.length,
  power: powerCases.length, slideOnly: slideOnly.length, powerConfigs: powerConfigCases.length,
};
console.log(`fixtures: ${JSON.stringify(counts)}; frames ${(rleBytes / 1024).toFixed(0)} KB RLE (raw ${(rawBytes / 1048576).toFixed(1)} MB)`);
