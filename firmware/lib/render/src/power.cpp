#include "power.h"

#include <math.h>

#include <algorithm>

#include "surface.h"

namespace wall {
namespace render {

double supplyBudgetA(const PowerSupplies& p) { return p.rated_a * (p.budget_pct / 100.0); }

SupplyAmps estimateSupplyAmps(const uint8_t* rgb, double brightnessPct, const PowerSupplies& p) {
  // Integer sums are exact here and in the JS doubles (at most 24,576 x 765), so the two
  // agree to the last bit as long as the final multiplications run in the same order.
  uint32_t a = 0;
  uint32_t b = 0;
  const uint8_t* o = rgb;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++, o += 3) {
      const uint32_t load = o[0] + o[1] + o[2];
      const bool onA = y < 32 || (y < 64 && x >= 128);
      if (onA) {
        a += load;
      } else {
        b += load;
      }
    }
  }
  // Six panels of 2048 pixels make half the wall; full white is 765 per pixel. The sums
  // are exact, so adding them before scaling matches the JS bit for bit.
  const double perPixelA = PANEL_FULL_WHITE_A / (2048 * 765);
  const double k = (brightnessPct / 100) * perPixelA;
  if (p.supplies == 1) return {static_cast<double>(a + b) * k, 0};
  return {a * k, b * k};
}

int limitedBrightness(const uint8_t* rgb, int brightnessPct, const PowerSupplies& p) {
  const SupplyAmps s = estimateSupplyAmps(rgb, brightnessPct, p);
  const double worst = std::max(s.a, s.b);
  const double budget = supplyBudgetA(p);
  if (worst <= budget) return brightnessPct;
  return static_cast<int>(floor((brightnessPct * budget) / worst));
}

int slideBrightness(double peakA, int brightnessPct, const PowerSupplies& p) {
  if (!(peakA > 0)) return brightnessPct;
  const double load = p.supplies == 1 ? peakA * 2 : peakA;
  const double cap = floor(supplyBudgetA(p) / load * 100);
  return cap < brightnessPct ? static_cast<int>(cap) : brightnessPct;
}

}  // namespace render
}  // namespace wall
