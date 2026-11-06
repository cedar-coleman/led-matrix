// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { W, H } from '../web/layout.js';
import { renderStorySlide, renderPostsSlide, formatAge, STORY_W, STORY_H, POST_SIZE } from '../web/igslides.js';

const solid = (w, h, c) => {
  const t = new Uint8ClampedArray(w * h * 4);
  for (let i = 0; i < t.length; i += 4) t.set([...c, 255], i);
  return t;
};
const px = (buf, x, y) => [...buf.slice((y * W + x) * 4, (y * W + x) * 4 + 3)];
const now = new Date('2026-11-06T20:00:00Z');

test('story tile fills the left 54x96 and the progress bar tracks the story', () => {
  const story = { tile: solid(STORY_W, STORY_H, [200, 10, 10]), timestamp: '2026-11-06T18:00:00+0000' };
  const buf = renderStorySlide({ username: 'thestrengthtank', story, index: 1, count: 3, progress: 0.5, now });
  assert.equal(buf.length, W * H * 4);
  assert.deepEqual(px(buf, 0, 0), [200, 10, 10]);
  assert.deepEqual(px(buf, STORY_W - 1, STORY_H - 1), [200, 10, 10]);
  assert.deepEqual(px(buf, STORY_W, 40), [0, 150, 150], 'teal divider');
  // 3 segments: first done (white), second half white, third dim.
  const row = [...Array(W).keys()].map((x) => px(buf, x, 88).join());
  const white = row.filter((c) => c === '255,255,255').length;
  const seg = Math.floor((W - 3 - (STORY_W + 4) + 1 - 4) / 3);
  assert.ok(Math.abs(white - (seg + Math.round(seg / 2))) <= 1, `white=${white} seg=${seg}`);
});

test('a 16-character handle is drawn at double size', () => {
  const buf = renderStorySlide({ username: 'thestrengthtank', story: null, index: 0, count: 1, now });
  let lit = 0;
  for (let y = 6; y < 22; y++) for (let x = STORY_W + 4; x < W; x++) if (px(buf, x, y).join() === '255,255,255') lit++;
  assert.ok(lit > 400, `lit=${lit}`);
});

test('posts: two tiles, reel badge, likes and age', () => {
  const posts = [
    { tile: solid(POST_SIZE, POST_SIZE, [10, 200, 10]), timestamp: '2026-11-05T20:00:00+0000', kind: 'reel', like_count: 214 },
    { tile: solid(POST_SIZE, POST_SIZE, [10, 10, 200]), timestamp: '2026-11-02T20:00:00+0000', kind: 'image', like_count: 87 },
  ];
  const buf = renderPostsSlide({ username: 'thestrengthtank', posts, now });
  assert.deepEqual(px(buf, 98, 9 + 40), [10, 200, 10]);
  assert.deepEqual(px(buf, 178 + 10, 9 + 40), [10, 10, 200]);
  assert.deepEqual(px(buf, 98 + POST_SIZE - 11, 9 + 2), [0, 0, 0], 'reel badge on the first post');
  assert.deepEqual(px(buf, 178 + POST_SIZE - 11, 9 + 2), [10, 10, 200], 'no badge on an image post');
  let pink = 0;
  for (let x = 98; x < 140; x++) for (let y = 0; y < 8; y++) if (px(buf, x, y).join() === '255,70,150') pink++;
  assert.ok(pink > 10, 'like count drawn above the first post');
});

test('ages read like Instagram', () => {
  assert.equal(formatAge('2026-11-06T19:59:40Z', now), 'just now');
  assert.equal(formatAge('2026-11-06T19:48:00Z', now), '12 min ago');
  assert.equal(formatAge('2026-11-06T17:00:00+0000', now), '3 h ago');
  assert.equal(formatAge('2026-11-04T20:00:00Z', now), '2 d ago');
  assert.equal(formatAge('2026-10-01T20:00:00Z', now), '5 wk ago');
  assert.equal(formatAge('garbage', now), '');
});
