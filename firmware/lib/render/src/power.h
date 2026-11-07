// Power estimate for a 256x96 frame on the wall's supplies: a port of web/power.js.
//
// The default (wall::PowerSupplies) is two 5 V 18 A supplies: supply A feeds chain panels
// 1-6, supply B panels 7-12. With the serpentine chain (row 2 mounted upside down, running
// right to left) that is:
//   A: the top row, plus the right half of the middle row
//   B: the left half of the middle row, plus the bottom row
// With one supply all twelve panels count against one budget.
// Current is treated as linear in channel value and brightness, which ignores driver
// gamma, so this is an estimate; Phase 0 step 5 calibrates PANEL_FULL_WHITE_A.
#pragma once

#include <stdint.h>

#include "wall_shared.h"

namespace wall {
namespace render {

constexpr double PANEL_FULL_WHITE_A = 4.9;
constexpr double SUPPLY_RATED_A = 18;
constexpr double SUPPLY_BUDGET_A = SUPPLY_RATED_A * 0.85;  // keep 15% headroom for continuous load

struct SupplyAmps {
  double a = 0;
  double b = 0;
};

// Amps one supply may carry continuously: rated * (pct / 100), in that order, so the
// default is exactly SUPPLY_BUDGET_A.
double supplyBudgetA(const PowerSupplies& p = PowerSupplies());

// Estimated amps per supply for one RGB888 frame at a brightness of 0-100%. With one
// supply the whole wall is `a` and `b` is 0.
// Perf: one pass over 24,576 pixels.
SupplyAmps estimateSupplyAmps(const uint8_t* rgb, double brightnessPct, const PowerSupplies& p = PowerSupplies());

// Brightness (0-100) to use for this frame so no supply passes its budget.
int limitedBrightness(const uint8_t* rgb, int brightnessPct, const PowerSupplies& p = PowerSupplies());

// Brightness for a media slide whose brightest frame draws peakA on the busier half of
// the wall (panels 1-6 or 7-12) at 100%. Held for the whole slide so an animation doesn't
// pump. peakA <= 0 (black or unknown) leaves the setting alone, as Infinity does in the
// JS. One supply carries both halves, so it is charged twice peakA (exact for all-white).
int slideBrightness(double peakA, int brightnessPct, const PowerSupplies& p = PowerSupplies());

}  // namespace render
}  // namespace wall
