// "New PRs" slide: a line-for-line port of web/prslides.js, checked byte for byte by
// test/test_parity like the whiteboard and Instagram slides.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "surface.h"
#include "wall_shared.h"

namespace wall {
namespace render {

// At most this many athletes get a screen per pass; it is also the dot row's length.
constexpr int MAX_PR_SCREENS = 20;

struct PrLift {
  int lift = 0;  // Lift
  double kg = 0;
  bool hasPrev = false;  // pr_prev known and not null
  double prev = 0;
  std::string date;
};

struct PrEntry {
  const Athlete* athlete = nullptr;  // points into the Content it came from
  std::vector<PrLift> lifts;         // newest first, board lift order within a day
  std::string newest;
};

// Athletes with at least one PR inside the highlight window, newest PR first. Hidden
// athletes and blank names are left out. today: the wall's local date "YYYY-MM-DD"; ""
// or anything invalid (no clock yet) means nothing is recent, as on the whiteboard.
// Perf: O(n x 5) to collect plus O(n log n) to sort; n is capped at 500 by the API.
std::vector<PrEntry> recentPrs(const std::vector<Athlete>& athletes, int days, const std::string& today);

// recentPrs for the wall's settings, cut to MAX_PR_SCREENS: what prScreens() in web/app.js
// gives the rotation.
std::vector<PrEntry> prScreens(const Content& c, const std::string& today);

// One athlete's screen. index/count: position among the athletes being celebrated (dots).
// Perf: clears and draws one 256x96 frame; O(pixels + characters).
void renderPrSlide(Surface& s, const PrEntry& entry, int index, int count, const std::string& today);

}  // namespace render
}  // namespace wall
