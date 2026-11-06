// Turns an uploaded image or video into a 256x96 animated GIF, entirely in the browser.
// The board only has to play GIFs, which keeps the firmware to one decoder.
//
// Avoids WebCodecs (ImageDecoder) and crypto.randomUUID on purpose: the board serves
// plain http on the gym network, which is not a secure context, so those APIs are missing.
// Animated GIF input is decoded with omggif (vendor/omggif.js, loaded as a classic script).

import { GIFEncoder, quantize, applyPalette } from './vendor/gifenc.esm.js';
import { W, H } from './layout.js';

export const MAX_FRAMES = 900;
export const MAX_CLIP_SECONDS = 60;
export const FPS_CHOICES = [10, 12, 15, 20, 25];

/**
 * Where the source lands on the 256x96 canvas.
 * fit 'cover' fills the wall and crops; 'contain' shows the whole source with black bars.
 * zoom >= 1 scales further; panX/panY (0..1) pick which part shows.
 */
export function placement(srcW, srcH, { fit = 'cover', zoom = 1, panX = 0.5, panY = 0.5 } = {}) {
  const base = fit === 'contain' ? Math.min(W / srcW, H / srcH) : Math.max(W / srcW, H / srcH);
  const s = base * zoom;
  const dw = srcW * s;
  const dh = srcH * s;
  return { dx: (W - dw) * panX, dy: (H - dh) * panY, dw, dh };
}

export function drawPlaced(ctx, source, srcW, srcH, opts) {
  const p = placement(srcW, srcH, opts);
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, W, H);
  ctx.imageSmoothingEnabled = true;
  ctx.imageSmoothingQuality = 'high';
  ctx.drawImage(source, p.dx, p.dy, p.dw, p.dh);
}

function frameCanvas() {
  const c = document.createElement('canvas');
  c.width = W;
  c.height = H;
  return { canvas: c, ctx: c.getContext('2d', { willReadFrequently: true }) };
}

/**
 * Open a file the user picked. Returns a source object the editor can preview and convert:
 *   { kind: 'image' | 'gif' | 'video', width, height, duration?, frames?, drawPreview(ctx, opts, t?) }
 */
export async function openSource(file) {
  const type = file.type || '';
  if (type === 'image/gif') {
    const gif = await openGif(file);
    if (gif.frames.length > 1) return gif;
  }
  if (type.startsWith('image/')) {
    const bmp = await createImageBitmap(file);
    return {
      kind: 'image',
      width: bmp.width,
      height: bmp.height,
      drawPreview: (ctx, opts) => drawPlaced(ctx, bmp, bmp.width, bmp.height, opts),
      close: () => bmp.close(),
    };
  }
  if (type.startsWith('video/') || /\.(mp4|m4v|mov|webm)$/i.test(file.name)) {
    return openVideo(file);
  }
  throw new Error(`This file type (${type || 'unknown'}) isn't supported. Use an image or a video.`);
}

async function openVideo(file) {
  const url = URL.createObjectURL(file);
  const video = document.createElement('video');
  video.muted = true;
  video.playsInline = true;
  video.preload = 'auto';
  video.src = url;
  await new Promise((resolve, reject) => {
    video.onloadeddata = resolve;
    video.onerror = () => reject(new Error("This browser can't play that video. Try MP4 (H.264) or WebM."));
  });
  return {
    kind: 'video',
    width: video.videoWidth,
    height: video.videoHeight,
    duration: video.duration,
    video,
    async drawPreview(ctx, opts, t = 0) {
      await seek(video, t);
      drawPlaced(ctx, video, video.videoWidth, video.videoHeight, opts);
    },
    close: () => URL.revokeObjectURL(url),
  };
}

function seek(video, t) {
  return new Promise((resolve) => {
    const target = Math.min(Math.max(0, t), Math.max(0, video.duration - 0.001));
    if (Math.abs(video.currentTime - target) < 0.0005 && video.readyState >= 2) return resolve();
    const done = () => {
      clearTimeout(timer);
      resolve();
    };
    const timer = setTimeout(done, 3000); // never hang on a frame the decoder skips
    video.addEventListener('seeked', done, { once: true });
    video.currentTime = target;
  });
}

/** Decode every frame of an animated GIF into full-size RGBA canvases, honouring disposal. */
async function openGif(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  const reader = new globalThis.GifReader(bytes);
  const { width, height } = reader;
  const count = Math.min(reader.numFrames(), MAX_FRAMES);
  const pixels = new Uint8ClampedArray(width * height * 4);
  const frames = [];
  let saved = null;
  let prev = null;
  for (let i = 0; i < count; i++) {
    const info = reader.frameInfo(i);
    if (prev?.disposal === 2) clearRect(pixels, width, prev);
    if (prev?.disposal === 3 && saved) pixels.set(saved);
    saved = info.disposal === 3 ? pixels.slice() : null;
    reader.decodeAndBlitFrameRGBA(i, pixels);
    const c = document.createElement('canvas');
    c.width = width;
    c.height = height;
    c.getContext('2d').putImageData(new ImageData(pixels.slice(), width, height), 0, 0);
    frames.push({ canvas: c, delayMs: Math.max(20, (info.delay || 10) * 10) });
    prev = info;
  }
  return {
    kind: 'gif',
    width,
    height,
    frames,
    duration: frames.reduce((s, f) => s + f.delayMs, 0) / 1000,
    drawPreview: (ctx, opts) => drawPlaced(ctx, frames[0].canvas, width, height, opts),
    close: () => {},
  };
}

function clearRect(pixels, width, r) {
  for (let y = r.y; y < r.y + r.height; y++) {
    pixels.fill(0, (y * width + r.x) * 4, (y * width + r.x + r.width) * 4);
  }
}

/**
 * Convert to a 256x96 GIF. Returns { blob, frames, peakFrame } where peakFrame is the
 * brightest frame's RGBA (used for the power estimate).
 * opts: placement options plus { start, end, fps } for video.
 *
 * Perf: O(frames x 24,576 px) for quantising; frames capped at MAX_FRAMES (900). Video
 * frames are fetched by seeking, one decode per frame, so a 60 s clip at 15 fps is 900
 * seeks; memory stays at one frame plus the growing GIF byte stream.
 */
export async function convert(source, opts, onProgress = () => {}, signal) {
  const { canvas, ctx } = frameCanvas();
  const gif = GIFEncoder();
  let peak = { load: -1, rgba: null };
  let written = 0;

  const addFrame = (delayMs) => {
    const rgba = ctx.getImageData(0, 0, W, H).data;
    const palette = quantize(rgba, 256, { format: 'rgb565' });
    const index = applyPalette(rgba, palette, 'rgb565');
    gif.writeFrame(index, W, H, { palette, delay: delayMs, repeat: 0 });
    let load = 0;
    for (let i = 0; i < rgba.length; i += 4) load += rgba[i] + rgba[i + 1] + rgba[i + 2];
    if (load > peak.load) peak = { load, rgba };
    written++;
  };

  if (source.kind === 'image') {
    source.drawPreview(ctx, opts);
    addFrame(1000);
    onProgress(1);
  } else if (source.kind === 'gif') {
    const n = source.frames.length;
    for (let i = 0; i < n; i++) {
      if (signal?.aborted) throw new DOMException('Cancelled', 'AbortError');
      drawPlaced(ctx, source.frames[i].canvas, source.width, source.height, opts);
      addFrame(source.frames[i].delayMs);
      onProgress((i + 1) / n);
      if (i % 10 === 9) await new Promise((r) => setTimeout(r));
    }
  } else {
    const start = Math.max(0, opts.start ?? 0);
    const end = Math.min(source.duration, opts.end ?? source.duration, start + MAX_CLIP_SECONDS);
    const fps = opts.fps ?? 15;
    const n = Math.max(1, Math.min(MAX_FRAMES, Math.floor((end - start) * fps)));
    const delay = Math.round(1000 / fps);
    for (let i = 0; i < n; i++) {
      if (signal?.aborted) throw new DOMException('Cancelled', 'AbortError');
      await seek(source.video, start + i / fps);
      drawPlaced(ctx, source.video, source.width, source.height, opts);
      addFrame(delay);
      onProgress((i + 1) / n);
    }
  }
  gif.finish();
  canvas.width = 0;
  return { blob: new Blob([gif.bytes()], { type: 'image/gif' }), frames: written, peakFrame: peak.rgba };
}

/** Frame count a video clip will produce with these settings. */
export function clipFrames(start, end, fps) {
  const len = Math.max(0, Math.min(end - start, MAX_CLIP_SECONDS));
  return Math.max(1, Math.min(MAX_FRAMES, Math.floor(len * fps)));
}

/** Turn a display name into the file name the board stores: lowercase, safe characters. */
export function fileNameFor(name, taken) {
  let base = name
    .toLowerCase()
    .replace(/\.[a-z0-9]+$/, '')
    .replace(/[^a-z0-9._-]+/g, '-')
    .replace(/^[^a-z0-9]+|-+$/g, '')
    .slice(0, 40) || 'media';
  let candidate = `${base}.gif`;
  for (let i = 2; taken.has(candidate); i++) candidate = `${base}-${i}.gif`;
  return candidate;
}
