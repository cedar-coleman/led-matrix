// Instagram slides for the 256x96 wall: the current story, and the latest two posts.
//
// The board fetches @thestrengthtank's stories and posts, crops each image to a tile
// (stories 54x96, posts 78x78) and stores the tiles. These renderers lay the tiles out
// with the wall's font. Like layout.js, this file is the spec the firmware ports.

import { W, H, createSurface, textWidth } from './layout.js';

export const STORY_W = 54;
export const STORY_H = 96;
export const POST_SIZE = 78;
export const MAX_STORIES = 5;
export const POST_COUNT = 2;

const C = {
  white: [255, 255, 255],
  grey: [140, 140, 140],
  dim: [60, 60, 60],
  pink: [255, 70, 150], // Instagram magenta
  teal: [0, 150, 150],
  badgeBg: [0, 0, 0],
};

const POST_X = [98, 178];
const POST_Y = 9;

/** "just now", "12 min ago", "3 h ago", "2 d ago", "5 wk ago". */
export function formatAge(timestamp, now = new Date()) {
  const t = Date.parse(timestamp);
  if (Number.isNaN(t)) return '';
  const min = Math.max(0, Math.floor((now.getTime() - t) / 60000));
  if (min < 1) return 'just now';
  if (min < 60) return `${min} min ago`;
  const h = Math.floor(min / 60);
  if (h < 24) return `${h} h ago`;
  const d = Math.floor(h / 24);
  if (d < 14) return `${d} d ago`;
  return `${Math.floor(d / 7)} wk ago`;
}

/**
 * One story at a time: the portrait story on the left, the account and a progress bar
 * (one segment per story, like Instagram) on the right.
 * story: { tile: RGBA STORY_W x STORY_H, timestamp }, index/count: which of the
 * current stories this is, progress: 0..1 through this story's screen time.
 *
 * Perf: fixed 256x96 output; O(pixels).
 */
export function renderStorySlide({ username, story, index, count, progress = 0, now = new Date() }) {
  const s = createSurface();
  if (story?.tile) s.blit(story.tile, STORY_W, STORY_H, 0, 0);
  s.rect(STORY_W, 0, STORY_W, H - 1, C.teal);

  const left = STORY_W + 4;
  const handle = `@${username}`;
  const big = textWidth(handle, false, 2) <= W - left - 2;
  s.text(handle, left, 6, C.white, { scale: big ? 2 : 1 });

  s.icon(left, 30);
  s.text('Story', left + 13, 31, C.pink);
  const age = story ? formatAge(story.timestamp, now) : '';
  if (age) s.text(age, left + 13 + textWidth('Story') + 8, 31, C.grey);
  if (count > 1) {
    const label = `${index + 1}/${count}`;
    s.text(label, W - 2 - textWidth(label), 31, C.grey);
  }
  s.text('Follow along on Instagram', left, 56, C.grey);

  // Progress segments across the bottom.
  const x0 = left;
  const x1 = W - 3;
  const gap = 2;
  const seg = Math.floor((x1 - x0 + 1 - gap * (count - 1)) / Math.max(1, count));
  for (let i = 0; i < count; i++) {
    const sx = x0 + i * (seg + gap);
    s.rect(sx, 88, sx + seg - 1, 89, C.dim);
    const fill = i < index ? seg : i === index ? Math.round(seg * Math.min(1, Math.max(0, progress))) : 0;
    if (fill) s.rect(sx, 88, sx + fill - 1, 89, C.white);
  }
  return s.buf;
}

/**
 * The latest posts as two square tiles, with like count above and age below each,
 * and the account on the left.
 * posts: [{ tile: RGBA POST_SIZE x POST_SIZE, timestamp, like_count?, kind }] where kind
 * is 'image' | 'video' | 'reel' | 'carousel'.
 *
 * Perf: fixed 256x96 output; O(pixels).
 */
export function renderPostsSlide({ username, posts, now = new Date() }) {
  const s = createSurface();

  // Left panel: icon, label, handle.
  s.icon(39, 8, 2);
  const centre = (str, y, c) => s.text(str, Math.max(1, Math.floor((96 - textWidth(str)) / 2)), y, c);
  centre('Latest posts', 34, C.white);
  // 15 characters fit the 96 px panel; drop the @ before cutting anything off.
  let handle = `@${username}`;
  if (textWidth(handle) > 94) handle = username;
  if (textWidth(handle) > 94) handle = handle.slice(0, 14) + '~';
  centre(handle, 50, C.pink);
  centre('Follow us!', 70, C.grey);
  s.rect(96, 0, 96, H - 1, C.teal);

  posts.slice(0, POST_COUNT).forEach((p, i) => {
    const x = POST_X[i];
    if (p.tile) s.blit(p.tile, POST_SIZE, POST_SIZE, x, POST_Y);
    if (p.kind === 'reel' || p.kind === 'video') playBadge(s, x + POST_SIZE - 11, POST_Y + 2);
    if (p.kind === 'carousel') carouselBadge(s, x + POST_SIZE - 11, POST_Y + 2);
    if (Number.isInteger(p.like_count)) {
      const likes = `♥ ${compact(p.like_count)}`;
      s.text(likes, x, 0, C.pink);
    }
    const age = formatAge(p.timestamp, now);
    s.text(age, x + Math.floor((POST_SIZE - textWidth(age)) / 2), POST_Y + POST_SIZE + 1, C.grey);
  });
  return s.buf;
}

function compact(n) {
  if (n < 1000) return String(n);
  if (n < 10000) return `${(n / 1000).toFixed(1).replace(/\.0$/, '')}k`;
  return `${Math.round(n / 1000)}k`;
}

// 9x9 black box with a white play triangle.
function playBadge(s, x, y) {
  s.rect(x, y, x + 8, y + 8, C.badgeBg);
  s.rect(x + 3, y + 2, x + 3, y + 6, C.white);
  s.rect(x + 4, y + 3, x + 4, y + 5, C.white);
  s.rect(x + 5, y + 4, x + 5, y + 4, C.white);
}

// 9x9 black box with two overlapping squares.
function carouselBadge(s, x, y) {
  s.rect(x, y, x + 8, y + 8, C.badgeBg);
  const box = (bx, by) => {
    s.rect(bx, by, bx + 4, by, C.white);
    s.rect(bx, by + 4, bx + 4, by + 4, C.white);
    s.rect(bx, by, bx, by + 4, C.white);
    s.rect(bx + 4, by, bx + 4, by + 4, C.white);
  };
  box(x + 3, y + 1);
  box(x + 1, y + 3);
}
