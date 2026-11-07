// Power estimate for a 256x96 frame on the wall's supplies (settings.power, API.md 7.3).
//
// The default is today's wiring: two Mean Well LRS-100-5 supplies (5 V, 18 A each), supply
// A on chain panels 1-6 and supply B on panels 7-12. With the serpentine chain (row 2
// mounted upside down, running right to left) that is:
//   A: the top row, plus the right half of the middle row
//   B: the left half of the middle row, plus the bottom row
// With one supply (for example a Mean Well UHP-500-5) all twelve panels share one budget.
//
// Per-panel full-white current comes from the seller spec: 480 W/m2 x 0.0512 m2 = 24.6 W,
// about 4.9 A at 5 V. Current is treated as linear in channel value and brightness, which
// ignores driver gamma, so this is an estimate. The firmware uses the same model to scale
// brightness down on any frame that would push a supply past its budget.

import { W, H } from './layout.js';

export const PANEL_FULL_WHITE_A = 4.9;
export const SUPPLY_RATED_A = 18;
export const SUPPLY_BUDGET_A = SUPPLY_RATED_A * 0.85; // keep 15% headroom for continuous load

/** settings.power as the API sends it. This default is the two LRS-100-5s above. */
export const DEFAULT_POWER = Object.freeze({ supplies: 2, rated_a: SUPPLY_RATED_A, budget_pct: 85 });

/**
 * Amps one supply may carry continuously. Written as rated * (pct / 100) because 85 / 100
 * is the same double as 0.85, so the default stays bit for bit the old SUPPLY_BUDGET_A
 * (and the C++ port, which multiplies in the same order, agrees).
 */
export function supplyBudgetA(power = DEFAULT_POWER) {
  return power.rated_a * (power.budget_pct / 100);
}

/**
 * Estimated amps per supply for one RGBA frame at a brightness of 0-100%. With one supply
 * the whole wall is `a` and `b` is 0, so callers can keep taking max(a, b).
 * Perf: one pass over 24,576 pixels.
 */
export function estimateSupplyAmps(rgba, brightnessPct, power = DEFAULT_POWER) {
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
  // Six panels of 2048 pixels make half the wall; full white is 765 per pixel. The sums
  // are exact integers, so adding them before scaling keeps C++ and JS bit-identical.
  const perPixelA = PANEL_FULL_WHITE_A / (2048 * 765);
  const k = (brightnessPct / 100) * perPixelA;
  if (power.supplies === 1) return { a: (a + b) * k, b: 0 };
  return { a: a * k, b: b * k };
}

/** Brightness (0-100) the firmware would actually use for this frame. */
export function limitedBrightness(rgba, brightnessPct, power = DEFAULT_POWER) {
  const { a, b } = estimateSupplyAmps(rgba, brightnessPct, power);
  const worst = Math.max(a, b);
  const budget = supplyBudgetA(power);
  if (worst <= budget) return brightnessPct;
  return Math.floor((brightnessPct * budget) / worst);
}

/**
 * Brightness (0-100) the wall uses for a whole media slide, from the file's peak_a: the
 * brightest frame's estimated current on the busier half of the wall (panels 1-6 or 7-12)
 * at 100% brightness. Dimming the whole slide once keeps it steady, instead of each bright
 * frame pumping the brightness. The per-frame limiter above stays on as a backstop.
 * peakA missing or 0: no cap.
 *
 * peak_a keeps that meaning whatever the supplies, so files uploaded before a supply change
 * stay right. One supply carries both halves, which is at most twice the busier one; that
 * bound is exact for all-white and only cautious for frames lit on one half.
 */
export function slideBrightness(peakA, brightnessPct, power = DEFAULT_POWER) {
  if (!(peakA > 0)) return brightnessPct;
  const load = power.supplies === 1 ? peakA * 2 : peakA;
  return Math.min(brightnessPct, Math.floor((supplyBudgetA(power) / load) * 100));
}
