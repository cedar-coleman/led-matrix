// PR whiteboard layout and renderer: a line-for-line port of web/layout.js.
//
// web/layout.js is the spec. The parity tests (test/test_parity) render the same inputs
// through both and compare every byte, so a change there needs the same change here.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "surface.h"
#include "wall_shared.h"

namespace wall {
namespace render {

constexpr int kMinSeconds = 5;
constexpr int kMaxSeconds = 999;
constexpr int kMaxRows = 9;  // floor(BODY_H / ROW_MIN)

extern const char* const kLiftLabels[kLiftCount];

// kg value for display: "" for blank, no trailing ".0".
std::string formatKg(double v);
std::string formatKg(const Athlete& a, int lift);

// Board order unless sorted by total (highest first, blanks last) or by name
// (leading @ _ . ignored). Both sorts are stable, as Array.prototype.sort is.
std::vector<const Athlete*> sortAthletes(const std::vector<const Athlete*>& list, SortMode mode);

// localeCompare for the name sort: the root collation order of every printable ASCII
// character, as V8's ICU returns it. Latin-1 letters sort with their base letter, accented
// after plain. Exact for ASCII, an approximation beyond it.
int compareNames(const JsString& a, const JsString& b);

struct Fit {
  std::vector<std::vector<const Athlete*>> pages;
  int pageCount = 1;
  int perPage = 1;
  int rowPitch = 8;
  int valW = 0;
  int numW = 0;
  int nameW = 0;
  int nameChars = 0;
  std::vector<std::string> truncated;
  int count = 0;
};

// Pages, row height and column widths; see fitWhiteboard() in web/layout.js.
// Perf: O(n) over athletes x 5 lifts, n capped at 500 by the API.
Fit fitWhiteboard(const std::vector<const Athlete*>& athletes);

// x of the rule on the left of column i (0 = name, 1..5 = lifts); rule 6 is the right frame.
int ruleX(const Fit& fit, int i);

// today: the wall's local date "YYYY-MM-DD". Empty or invalid (no clock yet) means no
// PR is recent, so nothing turns green on a wall that doesn't know the date.
bool isRecentPr(const Athlete& a, int lift, int days, const std::string& today);

struct WhiteboardOptions {
  std::string title = "THE STRENGTH TANK";
  int prDays = 7;
  std::string today;  // "" when time is invalid
};

// Perf: clears and draws one 256x96 frame; O(pixels + glyphs on the page).
void renderWhiteboardPage(Surface& s, const Fit& fit, int pageIndex, const WhiteboardOptions& opts);

// `!name.trim()` in JS: empty, or nothing but JS whitespace.
bool blankName(const std::string& name);

// What currentFit() in web/app.js feeds fitWhiteboard: athletes with a non-blank name,
// sorted by the wall's setting.
std::vector<const Athlete*> whiteboardAthletes(const std::vector<Athlete>& athletes, SortMode mode);

}  // namespace render
}  // namespace wall
