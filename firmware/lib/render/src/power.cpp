#include "power.h"

#include <math.h>

#include <algorithm>

#include "surface.h"

namespace wall {
namespace render {

SupplyAmps estimateSupplyAmps(const uint8_t* rgb, double brightnessPct) {
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
  // Each supply covers 6 panels of 2048 pixels; full white is 765 per pixel.
  const double perPixelA = PANEL_FULL_WHITE_A / (2048 * 765);
  const double k = (brightnessPct / 100) * perPixelA;
  return {a * k, b * k};
}

int limitedBrightness(const uint8_t* rgb, int brightnessPct) {
  const SupplyAmps s = estimateSupplyAmps(rgb, brightnessPct);
  const double worst = std::max(s.a, s.b);
  if (worst <= SUPPLY_BUDGET_A) return brightnessPct;
  return static_cast<int>(floor((brightnessPct * SUPPLY_BUDGET_A) / worst));
}

int slideBrightness(double peakA, int brightnessPct) {
  if (!(peakA > 0)) return brightnessPct;
  const double cap = floor(SUPPLY_BUDGET_A / peakA * 100);
  return cap < brightnessPct ? static_cast<int>(cap) : brightnessPct;
}

}  // namespace render
}  // namespace wall
