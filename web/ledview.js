// Draws a 256x96 image as round LED dots, the way the wall looks up close.
// The dot mask is drawn once; each frame is then two drawImage calls, fast enough
// to play video previews.

import { W, H } from './layout.js';

const masks = new Map();

function dotMask(scale) {
  if (masks.has(scale)) return masks.get(scale);
  const m = document.createElement('canvas');
  m.width = W * scale;
  m.height = H * scale;
  const ctx = m.getContext('2d');
  ctx.fillStyle = 'rgb(8,8,8)';
  ctx.fillRect(0, 0, m.width, m.height);
  ctx.globalCompositeOperation = 'destination-out';
  const r = scale * 0.42;
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      ctx.beginPath();
      ctx.arc(x * scale + scale / 2, y * scale + scale / 2, r, 0, Math.PI * 2);
      ctx.fill();
    }
  }
  masks.set(scale, m);
  return m;
}

export class LedView {
  constructor(canvas, scale = 4) {
    this.canvas = canvas;
    this.scale = scale;
    canvas.width = W * scale;
    canvas.height = H * scale;
    this.ctx = canvas.getContext('2d');
    this.src = document.createElement('canvas');
    this.src.width = W;
    this.src.height = H;
    this.srcCtx = this.src.getContext('2d', { willReadFrequently: true });
  }

  /** Show an RGBA buffer (Uint8ClampedArray, 256x96). */
  showBuffer(rgba) {
    this.srcCtx.putImageData(new ImageData(rgba, W, H), 0, 0);
    this.present();
  }

  /** Show anything drawImage accepts (an <img> playing a GIF, a canvas). */
  showImage(img) {
    this.srcCtx.fillStyle = '#000';
    this.srcCtx.fillRect(0, 0, W, H);
    this.srcCtx.drawImage(img, 0, 0, W, H);
    this.present();
  }

  pixels() {
    return this.srcCtx.getImageData(0, 0, W, H).data;
  }

  present() {
    const { ctx, scale } = this;
    ctx.fillStyle = 'rgb(22,22,22)'; // unlit LEDs read as dark grey dots
    ctx.fillRect(0, 0, W * scale, H * scale);
    ctx.imageSmoothingEnabled = false;
    ctx.globalCompositeOperation = 'lighter';
    ctx.drawImage(this.src, 0, 0, W * scale, H * scale);
    ctx.globalCompositeOperation = 'source-over';
    ctx.drawImage(dotMask(scale), 0, 0);
    if (this.seams) {
      // Edges of the twelve 64x32 panels, to check what straddles a seam.
      ctx.fillStyle = 'rgba(255, 80, 80, 0.7)';
      for (let x = 64; x < W; x += 64) ctx.fillRect(x * scale - 1, 0, 2, H * scale);
      for (let y = 32; y < H; y += 32) ctx.fillRect(0, y * scale - 1, W * scale, 2);
    }
  }
}
