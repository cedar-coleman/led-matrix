// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { W, H } from '../web/layout.js';
import {
  estimateSupplyAmps, limitedBrightness, slideBrightness, supplyBudgetA, DEFAULT_POWER, SUPPLY_BUDGET_A, PANEL_FULL_WHITE_A,
} from '../web/power.js';

const peakAmps = (rgba) => Math.max(...Object.values(estimateSupplyAmps(rgba, 100)));
const frame = (rgb, rows = [0, H]) => {
  const f = new Uint8ClampedArray(W * H * 4);
  for (let y = rows[0]; y < rows[1]; y++) for (let x = 0; x < W; x++) f.set([...rgb, 255], (y * W + x) * 4);
  return f;
};

test('full white at 100% is six panels per supply', () => {
  const { a, b } = estimateSupplyAmps(frame([255, 255, 255]), 100);
  assert.ok(Math.abs(a - 6 * PANEL_FULL_WHITE_A) < 1e-9);
  assert.ok(Math.abs(b - 6 * PANEL_FULL_WHITE_A) < 1e-9);
});

test('slideBrightness caps by the peak and never raises the setting', () => {
  const white = peakAmps(frame([255, 255, 255]));
  assert.equal(slideBrightness(white, 100), Math.floor((SUPPLY_BUDGET_A / white) * 100));
  assert.equal(slideBrightness(white, 100), 52);
  assert.equal(slideBrightness(white, 30), 30);
  assert.equal(slideBrightness(SUPPLY_BUDGET_A, 100), 100);
  assert.equal(slideBrightness(SUPPLY_BUDGET_A * 2, 80), 50);
  assert.equal(slideBrightness(1, 75), 75); // a dim file: the cap is above 100%
  assert.equal(slideBrightness(null, 60), 60);
  assert.equal(slideBrightness(0, 60), 60);
});

test('peak counts only the busier supply', () => {
  // Top row only: all on supply A.
  const top = frame([255, 255, 255], [0, 32]);
  const { a, b } = estimateSupplyAmps(top, 100);
  assert.equal(b, 0);
  assert.equal(peakAmps(top), a);
  // The slide cap agrees with the per-frame limiter on its own peak frame.
  assert.equal(slideBrightness(peakAmps(frame([255, 255, 255])), 100), limitedBrightness(frame([255, 255, 255]), 100));
});

// ---- settings.power (API.md 7.3) ----

const UHP = { supplies: 1, rated_a: 80, budget_pct: 85 };

test('the default power setting is exactly the old two-supply model', () => {
  assert.equal(supplyBudgetA(DEFAULT_POWER), SUPPLY_BUDGET_A);
  assert.equal(supplyBudgetA({ supplies: 2, rated_a: 18, budget_pct: 85 }), SUPPLY_BUDGET_A);
  for (const rgb of [[255, 255, 255], [0, 150, 150], [40, 0, 200]]) {
    const f = frame(rgb, [0, 70]);
    assert.deepEqual(estimateSupplyAmps(f, 37, DEFAULT_POWER), estimateSupplyAmps(f, 37));
    for (const pct of [100, 64, 52, 5]) assert.equal(limitedBrightness(f, pct, DEFAULT_POWER), limitedBrightness(f, pct));
  }
  for (const peak of [29.4, 15.3, 7.65, 0, null]) assert.equal(slideBrightness(peak, 100, DEFAULT_POWER), slideBrightness(peak, 100));
});

test('one supply carries all twelve panels against one budget', () => {
  const white = frame([255, 255, 255]);
  const one = estimateSupplyAmps(white, 100, UHP);
  assert.ok(Math.abs(one.a - 12 * PANEL_FULL_WHITE_A) < 1e-9);
  assert.equal(one.b, 0);
  // 58.8 A of full white fits under 80 A x 85% = 68 A, so nothing is dimmed.
  assert.equal(supplyBudgetA(UHP), 68);
  assert.equal(limitedBrightness(white, 100, UHP), 100);
  assert.equal(slideBrightness(6 * PANEL_FULL_WHITE_A, 100, UHP), 100);
  // The same frame on one 18 A supply is dimmed against 15.3 A for the whole wall.
  const small = { supplies: 1, rated_a: 18, budget_pct: 85 };
  assert.equal(limitedBrightness(white, 100, small), Math.floor((100 * SUPPLY_BUDGET_A) / (12 * PANEL_FULL_WHITE_A)));
  assert.equal(limitedBrightness(white, 100, small), 26);
});

test('one supply sums both halves, so a half-lit frame counts once', () => {
  const top = frame([255, 255, 255], [0, 32]); // supply A's panels only
  const two = estimateSupplyAmps(top, 100);
  const one = estimateSupplyAmps(top, 100, UHP);
  assert.equal(one.a, two.a + two.b);
  // A 40 A supply at 90%: 36 A budget. The top row (19.6 A) fits, all-white (58.8 A) doesn't.
  const mid = { supplies: 1, rated_a: 40, budget_pct: 90 };
  assert.equal(limitedBrightness(top, 100, mid), 100);
  assert.equal(limitedBrightness(frame([255, 255, 255]), 100, mid), 61);
});

test('slideBrightness on one supply treats peak_a as the busier half, so the bound is twice it', () => {
  const mid = { supplies: 1, rated_a: 40, budget_pct: 90 };
  const whitePeak = peakAmps(frame([255, 255, 255])); // 29.4 A on each half
  assert.equal(slideBrightness(whitePeak, 100, mid), Math.floor((36 / (2 * whitePeak)) * 100));
  // For all-white the bound is exact: the slide cap agrees with the per-frame limiter.
  assert.equal(slideBrightness(whitePeak, 100, mid), limitedBrightness(frame([255, 255, 255]), 100, mid));
  assert.equal(slideBrightness(whitePeak, 40, mid), 40);
  assert.equal(slideBrightness(0, 70, mid), 70);
});

test('two supplies with another rating and budget scale the old split', () => {
  const p = { supplies: 2, rated_a: 30.5, budget_pct: 70 };
  const white = frame([255, 255, 255]);
  assert.deepEqual(estimateSupplyAmps(white, 80, p), estimateSupplyAmps(white, 80));
  assert.equal(limitedBrightness(white, 100, p), Math.floor((100 * 30.5 * 0.7) / (6 * PANEL_FULL_WHITE_A)));
});
