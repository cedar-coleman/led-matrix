// Power estimate for a 256x96 frame on two Mean Well LRS-100-5 supplies (5 V, 18 A each).
//
// Supply A feeds chain panels 1-6, supply B panels 7-12. With the serpentine chain
// (row 2 mounted upside down, running right to left) that is:
//   A: the top row, plus the right half of the middle row
//   B: the left half of the middle row, plus the bottom row
//
// Per-panel full-white current comes from the seller spec: 480 W/m2 x 0.0512 m2 = 24.6 W,
// about 4.9 A at 5 V. Current is treated as linear in channel value and brightness, which
// ignores driver gamma, so this is an estimate. The firmware uses the same model to scale
// brightness down on any frame that would push a supply past its budget.

import { W, H } from './layout.js';

export const PANEL_FULL_WHITE_A = 4.9;
export const SUPPLY_RATED_A = 18;
export const SUPPLY_BUDGET_A = SUPPLY_RATED_A * 0.85; // keep 15% headroom for continuous load

/**
 * Estimated amps per supply for one RGBA frame at a brightness of 0-100%.
 * Perf: one pass over 24,576 pixels.
 */
export function estimateSupplyAmps(rgba, brightnessPct) {
  let a = 0;
  let b = 0;
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const o = (y * W + x) * 4;
      const load = rgba[o] + rgba[o + 1] + rgba[o + 2];
      const onA = y < 32 || (y < 64 && x >= 128);
      if (onA) a += load;
      else b += load;
    }
  }
  // Each supply covers 6 panels of 2048 pixels; full white is 765 per pixel.
  const perPixelA = PANEL_FULL_WHITE_A / (2048 * 765);
  const k = (brightnessPct / 100) * perPixelA;
  return { a: a * k, b: b * k };
}

/** Brightness (0-100) the firmware would actually use for this frame. */
export function limitedBrightness(rgba, brightnessPct) {
  const { a, b } = estimateSupplyAmps(rgba, brightnessPct);
  const worst = Math.max(a, b);
  if (worst <= SUPPLY_BUDGET_A) return brightnessPct;
  return Math.floor((brightnessPct * SUPPLY_BUDGET_A) / worst);
}

/**
 * Brightness (0-100) the wall uses for a whole media slide, from the file's peak_a: the
 * brightest frame's estimated current on the busier supply at 100% brightness. Dimming the
 * whole slide once keeps it steady, instead of each bright frame pumping the brightness.
 * The per-frame limiter above stays on as a backstop. peakA missing or 0: no cap.
 */
export function slideBrightness(peakA, brightnessPct) {
  if (!(peakA > 0)) return brightnessPct;
  return Math.min(brightnessPct, Math.floor((SUPPLY_BUDGET_A / peakA) * 100));
}
