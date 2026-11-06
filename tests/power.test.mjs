// Run: node --test tests/
import test from 'node:test';
import assert from 'node:assert/strict';
import { W, H } from '../web/layout.js';
import { estimateSupplyAmps, limitedBrightness, slideBrightness, SUPPLY_BUDGET_A, PANEL_FULL_WHITE_A } from '../web/power.js';

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
