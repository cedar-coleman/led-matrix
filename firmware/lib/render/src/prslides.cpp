#include "prslides.h"

#include <algorithm>

#include "layout.h"

namespace wall {
namespace render {

namespace {

const char* const kLiftNames[kLiftCount] = {"Snatch", "Clean & Jerk", "Total", "Back squat", "Front squat"};

struct Colors {
  Rgb white{255, 255, 255};
  Rgb gold{255, 200, 40};
  Rgb green{60, 255, 110};
  Rgb grey{140, 140, 140};
  Rgb dim{60, 60, 60};
  Rgb banner{120, 80, 0};
};
const Colors C;

// The JS takes a Date; the wall passes its local date, and "" (or garbage) means no clock.
bool validDay(const std::string& today) {
  int64_t ms = 0;
  return today.size() == 10 && parseIsoMs(today, &ms);
}

std::string daysAgo(const std::string& date, const std::string& today) {
  if (!validDay(today)) return "";
  int64_t t = 0, now = 0;
  parseIsoMs(today, &now);
  // A date that doesn't parse never reaches here: isRecentPr has already turned it away.
  if (!parseIsoMs(date + "T00:00:00Z", &t)) return "";
  const double d = jsRound(static_cast<double>(now - t) / 86400000);
  return d <= 0 ? "today" : d == 1 ? "yesterday" : intString(d) + " days ago";
}

std::string increase(const PrLift& l) { return l.hasPrev && l.kg > l.prev ? "+" + formatKg(l.kg - l.prev) : ""; }

}  // namespace

std::vector<PrEntry> recentPrs(const std::vector<Athlete>& athletes, int days, const std::string& today) {
  std::vector<PrEntry> out;
  if (!validDay(today)) return out;
  for (const Athlete& a : athletes) {
    if (!a.visible || blankName(a.name)) continue;
    PrEntry e;
    e.athlete = &a;
    for (int l = 0; l < kLiftCount; l++) {
      if (!a.has[l] || !isRecentPr(a, l, days, today)) continue;
      PrLift p;
      p.lift = l;
      p.kg = a.kg[l];
      p.hasPrev = a.has_prev[l];
      p.prev = a.prev_kg[l];
      p.date = a.pr_date[l];
      e.lifts.push_back(p);
    }
    if (e.lifts.empty()) continue;
    // Newest date first; within a day, board lift order (no two lifts compare equal).
    std::sort(e.lifts.begin(), e.lifts.end(), [](const PrLift& x, const PrLift& y) {
      return x.date != y.date ? x.date > y.date : x.lift < y.lift;
    });
    e.newest = e.lifts[0].date;
    out.push_back(std::move(e));
  }
  // Array.prototype.sort is stable: athletes with the same newest date keep board order.
  std::stable_sort(out.begin(), out.end(), [](const PrEntry& x, const PrEntry& y) { return x.newest > y.newest; });
  return out;
}

std::vector<PrEntry> prScreens(const Content& c, const std::string& today) {
  std::vector<PrEntry> list = recentPrs(c.athletes, c.settings.pr_highlight_days, today);
  if (list.size() > static_cast<size_t>(MAX_PR_SCREENS)) list.resize(MAX_PR_SCREENS);
  return list;
}

void renderPrSlide(Surface& s, const PrEntry& entry, int index, int count, const std::string& today) {
  s.clear();
  const Athlete& athlete = *entry.athlete;
  const std::vector<PrLift>& lifts = entry.lifts;

  // Gold banner across the top with the athlete's handle.
  s.rect(0, 0, W - 1, 19, C.banner);
  s.rect(0, 20, W - 1, 20, C.gold);
  const std::string label = "NEW PR";
  s.text(label, 4, 6, C.gold);
  const int nameX = 4 + textWidth(label) + 8;
  const int room = W - nameX - 4;
  JsString name = fromUtf8(athlete.name);
  const bool big = textWidth(name, false, 2) <= room;
  if (!big) {
    const int chars = floorDiv(room + 1, 6);
    if (static_cast<int>(name.size()) > chars) name = jsSlice(name, 0, chars - 1) + u"~";
  }
  s.text(name, nameX, big ? 2 : 6, C.white, false, big ? 2 : 1);

  // Lifts: one big line for a single PR, up to three lines for several.
  const size_t shown = std::min<size_t>(lifts.size(), 3);
  if (shown == 1) {
    const PrLift& l = lifts[0];
    const std::string liftName = kLiftNames[l.lift];
    const int scale = textWidth(liftName, false, 2) <= W - 8 ? 2 : 1;
    s.text(liftName, floorDiv(W - textWidth(liftName, false, scale), 2), 28, C.white, false, scale);
    const std::string kg = formatKg(l.kg) + " kg";
    const int kgW = textWidth(kg, true, 3);
    const std::string up = increase(l);
    const int upW = !up.empty() ? textWidth(up, true, 2) + 8 : 0;
    const int x = floorDiv(W - kgW - upW, 2);
    s.text(kg, x, 48, C.gold, true, 3);
    if (!up.empty()) s.text(up, x + kgW + 8, 56, C.green, true, 2);
  } else {
    // One column for every row's weight, so "+3" on one row and "+12.5" on the next don't
    // push their numbers out of line.
    std::vector<std::string> ups;
    int upW = 0;
    for (size_t i = 0; i < shown; i++) {
      ups.push_back(increase(lifts[i]));
      upW = std::max(upW, !ups[i].empty() ? textWidth(ups[i], true) + 6 : 0);
    }
    for (size_t i = 0; i < shown; i++) {
      const PrLift& l = lifts[i];
      const int y = 26 + static_cast<int>(i) * 18;
      s.text(std::string(kLiftNames[l.lift]), 6, y, C.white, false, 2);
      const std::string kg = formatKg(l.kg);
      s.text(kg, W - 6 - upW - textWidth(kg, true, 2), y, C.gold, true, 2);
      if (!ups[i].empty()) s.text(ups[i], W - upW, y + 4, C.green, true);
    }
    if (lifts.size() > 3) s.text("+" + std::to_string(lifts.size() - 3) + " more", 6, 80, C.grey);
  }

  // Footer: when, and which athlete of how many.
  const std::string when = daysAgo(entry.newest, today);
  s.text(when, W - 4 - textWidth(when), 87, C.grey);
  if (count > 1) {
    const int dots = std::min(count, MAX_PR_SCREENS);
    for (int i = 0; i < dots; i++) {
      const int x = 4 + i * 6;
      s.rect(x, 89, x + 3, 92, i == index % dots ? C.gold : C.dim);
    }
  }
}

}  // namespace render
}  // namespace wall
