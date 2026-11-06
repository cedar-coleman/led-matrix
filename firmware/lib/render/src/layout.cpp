#include "layout.h"

#include <math.h>

#include <algorithm>

namespace wall {
namespace render {

const char* const kLiftLabels[kLiftCount] = {"S", "CJ", "Ttl", "BS", "FS"};

namespace {

struct Colors {
  Rgb rowAlt{0, 10, 28};    // every other row, dim navy band
  Rgb rowRule{0, 30, 75};   // thin line between rows when there is room for one
  Rgb grid{0, 70, 170};     // the chalkboard's blue column tape
  Rgb teal{0, 150, 150};    // brand teal: frame and header rules
  Rgb title{255, 255, 255};
  Rgb header{235, 215, 70}; // the chalkboard's yellow header chalk
  Rgb name{170, 220, 255};
  Rgb num{255, 255, 255};
  Rgb pr{60, 255, 110};     // lift set within the highlight window
  Rgb page{120, 120, 120};
  Rgb empty{120, 120, 120};
};
const Colors C;

constexpr int TITLE_RULE_Y = 11;
constexpr int HEADER_Y = 12;
constexpr int HEADER_RULE_Y = 20;
constexpr int BODY_Y = 21;
constexpr int BOTTOM_RULE_Y = H - 1;
constexpr int BODY_H = BOTTOM_RULE_Y - BODY_Y;  // 74 px for data rows
constexpr int ROW_MIN = 8;                       // glyph height including descenders
constexpr int ROW_MAX = 10;                      // more than this and short pages look sparse
constexpr int INNER_W = W - 7;                   // 2 frame rules + 5 column rules
constexpr int NAME_PAD = 4;                      // 2 px each side of the name text
constexpr int NUM_PAD = 2;                       // at least 1 px clear of each column rule
static_assert(BODY_H / ROW_MIN == kMaxRows, "MAX_ROWS in web/layout.js");

// ICU root collation order of printable ASCII after lowercasing, read from V8 with
// localeCompare (the parity fixtures re-check it on every run).
const char kCollation[] = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$0123456789abcdefghijklmnopqrstuvwxyz";

struct Weight {
  int primary;
  int secondary;
};

// Primary weights are spaced so other characters can be slotted in just before an ASCII one.
constexpr int kSlot = 1000000;

int asciiPrimary(char16_t u) {
  for (int i = 0; kCollation[i]; i++) {
    if (kCollation[i] == u) return i * kSlot;
  }
  return -kSlot + u;  // control characters: ICU ignores them; sort them first
}

// Sorts just before the ASCII character `next`; among themselves by code unit.
int before(char next, char16_t u) { return asciiPrimary(static_cast<char16_t>(next)) - kSlot / 2 + u; }
// Sorts just after the ASCII character `prev`, ahead of anything placed before the next one.
int after(char prev, char16_t u) { return asciiPrimary(static_cast<char16_t>(prev)) + kSlot / 4 + u; }

// Root collation beyond ASCII, approximated from what V8 returns for the characters a
// gym's names plausibly hold: Latin-1 letters sort as their base letter (accent second),
// Latin-1 punctuation and symbols where ICU puts them, dashes, quotes and arrows near
// their ASCII cousins, other symbols and emoji just after '~', currency around '$', and other scripts after 'z'.
Weight weightOf(char16_t u) {
  if (u < 128) return {asciiPrimary(u), 0};
  if (u == 0xA0) return {asciiPrimary(' '), 0};
  static const char kFold[] = "aaaaaaaceeeeiiiidnooooo/ouuuuy/y";  // U+00E0..U+00FF, after lowercasing
  if (u >= 0xE0 && u <= 0xFF && kFold[u - 0xE0] != '/') return {asciiPrimary(kFold[u - 0xE0]), 1};
  if (u == 0xDF) return {asciiPrimary('s'), 1};  // sharp s, near "ss"
  if (u == 0xAA) return {asciiPrimary('a'), 1};
  if (u == 0xBA) return {asciiPrimary('o'), 1};
  // U+00A1..U+00BF: the ASCII character each sorts just before ('L' = a letter after 'z').
  static const char kLatin1[] = "?$000|@^+L(|-+^+<23`L@.`1L(111.";
  if (u >= 0xA1 && u <= 0xBF) {
    const char next = kLatin1[u - 0xA1];
    if (next != 'L') return {before(next, u), 0};
  }
  if (u == 0xD7 || u == 0xF7) return {before('<', u), 0};
  if (u >= 0x2010 && u <= 0x2015) return {before(',', u), 0};
  if (u >= 0x2018 && u <= 0x201F) return {before('"', u), 0};
  if (u >= 0x20A0 && u <= 0x20CF) return {before('0', u), 0};
  if (u >= 0x2190 && u <= 0x22FF) return {before('+', u), 0};
  if ((u >= 0x2000 && u <= 0x2BFF) || (u >= 0xD800 && u <= 0xDFFF) || (u >= 0xE000 && u <= 0xF8FF)) {
    return {after('~', u), 0};
  }
  return {asciiPrimary('z') + kSlot / 2 + (u == 0xB5 ? 0x3BC : u), 0};  // micro sign sorts as Greek mu
}

int compareWeights(const JsString& a, const JsString& b, bool secondary) {
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; i++) {
    const Weight wa = weightOf(a[i]), wb = weightOf(b[i]);
    const int da = secondary ? wa.secondary : wa.primary;
    const int db = secondary ? wb.secondary : wb.primary;
    if (da != db) return da < db ? -1 : 1;
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  return 0;
}

char16_t toLower(char16_t u) {
  if (u >= 'A' && u <= 'Z') return u + 32;
  if (u >= 0xC0 && u <= 0xDE && u != 0xD7) return u + 32;
  return u;
}

bool isJsSpace(char16_t u) {
  return u == 9 || u == 10 || u == 11 || u == 12 || u == 13 || u == 32 || u == 0xA0 || u == 0x1680 ||
         (u >= 0x2000 && u <= 0x200A) || u == 0x2028 || u == 0x2029 || u == 0x202F || u == 0x205F ||
         u == 0x3000 || u == 0xFEFF;
}

// a.name.replace(/^[@_.]+/, '').toLowerCase()
JsString nameKey(const Athlete* a) {
  JsString s = fromUtf8(a->name);
  size_t i = 0;
  while (i < s.size() && (s[i] == '@' || s[i] == '_' || s[i] == '.')) i++;
  s.erase(0, i);
  for (auto& u : s) u = toLower(u);
  return s;
}

double totalKey(const Athlete* a) { return a->has[kTotal] ? a->kg[kTotal] : -1; }

}  // namespace

std::string formatKg(double v) {
  return v == floor(v) ? intString(v) : toFixed1(v);
}

std::string formatKg(const Athlete& a, int lift) {
  if (!a.has[lift]) return "";
  return formatKg(static_cast<double>(a.kg[lift]));
}

int compareNames(const JsString& a, const JsString& b) {
  const int p = compareWeights(a, b, false);
  return p ? p : compareWeights(a, b, true);
}

std::vector<const Athlete*> sortAthletes(const std::vector<const Athlete*>& list, SortMode mode) {
  std::vector<const Athlete*> out = list;
  if (mode == SortMode::kTotal) {
    std::stable_sort(out.begin(), out.end(),
                     [](const Athlete* a, const Athlete* b) { return totalKey(b) - totalKey(a) < 0; });
  } else if (mode == SortMode::kName) {
    std::vector<std::pair<JsString, const Athlete*>> keyed;
    keyed.reserve(out.size());
    for (const Athlete* a : out) keyed.emplace_back(nameKey(a), a);
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) { return compareNames(a.first, b.first) < 0; });
    for (size_t i = 0; i < out.size(); i++) out[i] = keyed[i].second;
  }
  return out;  // kBoard: the order coaches arranged in the editor
}

Fit fitWhiteboard(const std::vector<const Athlete*>& athletes) {
  Fit fit;
  std::vector<const Athlete*> rows;
  for (const Athlete* a : athletes) {
    if (a->visible) rows.push_back(a);
  }
  const int n = static_cast<int>(rows.size());
  const int pageCount = std::max(1, (n + kMaxRows - 1) / kMaxRows);
  const int base = n / pageCount;
  const int extra = n % pageCount;
  for (int p = 0, i = 0; p < pageCount; p++) {
    const int k = base + (p < extra ? 1 : 0);
    fit.pages.emplace_back(rows.begin() + i, rows.begin() + i + k);
    i += k;
  }
  const int perPage = std::max(1, base + (extra ? 1 : 0));
  const int rowPitch = std::min(ROW_MAX, std::max(ROW_MIN, BODY_H / perPage));

  int valW = 0;
  for (const char* l : kLiftLabels) valW = std::max(valW, textWidth(std::string(l)));
  int longest = textWidth(std::string("Name"));
  std::vector<JsString> names;
  names.reserve(rows.size());
  for (const Athlete* a : rows) {
    names.push_back(fromUtf8(a->name));
    longest = std::max(longest, textWidth(names.back()));
    for (int k = 0; k < kLiftCount; k++) valW = std::max(valW, textWidth(formatKg(*a, k), true));
  }
  const int numMin = valW + NUM_PAD;
  const int nameNeed = longest + NAME_PAD;
  int numW = numMin;
  if (nameNeed + 5 * numMin <= INNER_W) {
    numW += (INNER_W - nameNeed - 5 * numMin) / 6;
  }
  const int nameW = INNER_W - 5 * numW;
  const int nameChars = floorDiv(nameW - NAME_PAD + 1, 6);
  for (size_t i = 0; i < rows.size(); i++) {
    if (static_cast<int>(names[i].size()) > nameChars) fit.truncated.push_back(rows[i]->name);
  }

  fit.pageCount = pageCount;
  fit.perPage = perPage;
  fit.rowPitch = rowPitch;
  fit.valW = valW;
  fit.numW = numW;
  fit.nameW = nameW;
  fit.nameChars = nameChars;
  fit.count = n;
  return fit;
}

int ruleX(const Fit& fit, int i) { return i == 0 ? 0 : fit.nameW + 1 + (i - 1) * (fit.numW + 1); }

bool isRecentPr(const Athlete& a, int lift, int days, const std::string& today) {
  const std::string& d = a.pr_date[lift];
  if (!days || d.empty()) return false;
  int64_t t = 0, now = 0;
  if (!parseIsoMs(d + "T00:00:00Z", &t)) return false;
  // The JS takes Date.UTC(today's local Y, M, D); the wall's local date string is that.
  if (today.size() != 10 || !parseIsoMs(today, &now)) return false;
  const double age = static_cast<double>(now - t) / 86400000;
  return age >= 0 && age < days;
}

void renderWhiteboardPage(Surface& s, const Fit& fit, int pageIndex, const WhiteboardOptions& opts) {
  s.clear();
  const JsString title = fromUtf8(opts.title);

  // Title band: Instagram glyph, title, page number.
  s.icon(2, 1);
  JsString pageLabel;
  if (fit.pageCount > 1) pageLabel = fromUtf8(std::to_string(pageIndex + 1) + "/" + std::to_string(fit.pageCount));
  const int titleRoom = floorDiv(W - 15 - (!pageLabel.empty() ? textWidth(pageLabel) + 6 : 2) + 1, 6);
  s.text(jsSlice(title, 0, titleRoom), 15, 2, C.title);
  if (!pageLabel.empty()) s.text(pageLabel, W - 2 - textWidth(pageLabel), 2, C.page);

  static const std::vector<const Athlete*> kNoPage;
  const auto& page = pageIndex >= 0 && pageIndex < static_cast<int>(fit.pages.size()) ? fit.pages[pageIndex] : kNoPage;
  const int pitch = fit.rowPitch;
  const int textDy = pitch >= 10 ? 1 : 0;
  const int rowsOnPage = static_cast<int>(page.size());

  // Row bands and thin row rules go under the column rules.
  for (int i = 0; i < rowsOnPage; i++) {
    const int top = BODY_Y + i * pitch;
    if (i % 2) s.rect(1, top, W - 2, top + pitch - 1, C.rowAlt);
    if (pitch >= 9 && i < rowsOnPage - 1) s.rect(1, top + pitch - 1, W - 2, top + pitch - 1, C.rowRule);
  }

  // Header labels.
  s.text(std::string("Name"), 3, HEADER_Y, C.header);
  for (int i = 0; i < kLiftCount; i++) {
    const std::string label = kLiftLabels[i];
    const int left = ruleX(fit, i + 1) + 1;
    s.text(label, left + floorDiv(fit.numW - textWidth(label), 2), HEADER_Y, C.header);
  }

  // Column rules, then the teal frame and header rules on top.
  for (int i = 1; i <= 5; i++) s.rect(ruleX(fit, i), TITLE_RULE_Y, ruleX(fit, i), BOTTOM_RULE_Y, C.grid);
  s.rect(0, TITLE_RULE_Y, W - 1, TITLE_RULE_Y, C.teal);
  s.rect(0, HEADER_RULE_Y, W - 1, HEADER_RULE_Y, C.teal);
  s.rect(0, BOTTOM_RULE_Y, W - 1, BOTTOM_RULE_Y, C.teal);
  s.rect(0, TITLE_RULE_Y, 0, BOTTOM_RULE_Y, C.teal);
  s.rect(W - 1, TITLE_RULE_Y, W - 1, BOTTOM_RULE_Y, C.teal);

  if (fit.count == 0) {
    const std::string msg = "No athletes yet";
    s.text(msg, floorDiv(fit.nameW - textWidth(msg), 2), BODY_Y + 30, C.empty);
    return;
  }

  // Numbers sit right-aligned in a block of the widest value, centred in each column.
  const int numInset = floorDiv(fit.numW - fit.valW, 2);
  for (int i = 0; i < rowsOnPage; i++) {
    const Athlete& a = *page[i];
    const int y = BODY_Y + i * pitch + textDy;
    JsString name = fromUtf8(a.name);
    if (static_cast<int>(name.size()) > fit.nameChars) name = jsSlice(name, 0, fit.nameChars - 1) + u"~";
    s.text(name, 3, y, C.name);
    for (int j = 0; j < kLiftCount; j++) {
      const std::string v = formatKg(a, j);
      if (v.empty()) continue;
      const int right = ruleX(fit, j + 1) + numInset + fit.valW;
      const Rgb color = isRecentPr(a, j, opts.prDays, opts.today) ? C.pr : C.num;
      s.text(v, right - textWidth(v, true), y, color, true);
    }
  }
}

std::vector<const Athlete*> whiteboardAthletes(const std::vector<Athlete>& athletes, SortMode mode) {
  std::vector<const Athlete*> named;
  named.reserve(athletes.size());
  for (const Athlete& a : athletes) {
    const JsString n = fromUtf8(a.name);
    if (std::any_of(n.begin(), n.end(), [](char16_t u) { return !isJsSpace(u); })) named.push_back(&a);
  }
  return sortAthletes(named, mode);
}

}  // namespace render
}  // namespace wall
