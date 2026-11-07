#include "announceslides.h"

#include <algorithm>

namespace wall {
namespace render {

namespace {

struct Colors {
  Rgb teal{0, 150, 150};  // the logo slide's background
  Rgb white{255, 255, 255};
  Rgb dim{0, 90, 90};  // the other dots: visible on teal without competing with the text
};
const Colors C;

constexpr int MARGIN = 4;
constexpr int SCALES[] = {3, 2};
constexpr int DOT = 4;
constexpr int DOT_PITCH = 7;
constexpr int DOT_Y = H - MARGIN - DOT;  // 88
constexpr int DOT_GAP = 4;               // clear space between the text and the dots

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// /^\d{4}-\d{2}-\d{2}$/: the wall's local date; anything else means the wall has no clock.
bool validToday(const std::string& t) {
  if (t.size() != 10 || t[4] != '-' || t[7] != '-') return false;
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) {
    if (!isDigit(t[i])) return false;
  }
  return true;
}

// String.prototype.split with a one-unit separator: n separators give n + 1 parts.
std::vector<JsString> split(const JsString& s, char16_t sep) {
  std::vector<JsString> out(1);
  for (char16_t u : s) {
    if (u == sep) {
      out.emplace_back();
    } else {
      out.back() += u;
    }
  }
  return out;
}

// [...s]: one string per code point.
std::vector<JsString> codePoints(const JsString& s) {
  std::vector<JsString> out;
  size_t i = 0;
  forEachChar(s, [&](char16_t, int units) {
    out.push_back(s.substr(i, units));
    i += units;
  });
  return out;
}

JsString join(const std::vector<JsString>& chars) {
  JsString out;
  for (const JsString& c : chars) out += c;
  return out;
}

// Characters that fit across the wall at this scale with the margin on both sides.
int lineChars(int scale) { return floorDiv(W - 2 * MARGIN + scale, 6 * scale); }
// Lines are 9 font pixels apart: the 8-row glyph plus one row of air.
int blockHeight(int lines, int scale) { return lines * 9 * scale - scale; }

int longestWord(const JsString& text) {
  int longest = 0;
  for (const JsString& para : split(text, u'\n')) {
    for (const JsString& word : split(para, u' ')) longest = std::max(longest, static_cast<int>(codePoints(word).size()));
  }
  return longest;
}

}  // namespace

bool announcementActive(const Announcement& a, const std::string& today) {
  if (!a.enabled) return false;
  const std::string& start = a.start_date;
  const std::string& end = a.end_date;
  if (start.empty() && end.empty()) return true;
  if (!validToday(today)) return false;
  return (start.empty() || start <= today) && (end.empty() || today <= end);
}

std::vector<const Announcement*> announcementScreens(const Content& c, const std::string& today) {
  std::vector<const Announcement*> out;
  for (const Announcement& a : c.announcements) {
    if (announcementActive(a, today)) out.push_back(&a);
  }
  if (out.size() > static_cast<size_t>(MAX_ANNOUNCE_SCREENS)) out.resize(MAX_ANNOUNCE_SCREENS);
  return out;
}

std::vector<JsString> wrapLines(const JsString& text, int maxChars) {
  std::vector<JsString> lines;
  for (const JsString& para : split(text, u'\n')) {
    std::vector<JsString> line;
    for (const JsString& word : split(para, u' ')) {
      if (word.empty()) continue;
      std::vector<JsString> w = codePoints(word);
      const auto n = [](const std::vector<JsString>& v) { return static_cast<int>(v.size()); };
      if (n(line) && n(line) + 1 + n(w) > maxChars) {
        lines.push_back(join(line));
        line.clear();
      }
      if (n(line)) line.push_back(u" ");
      while (n(line) + n(w) > maxChars) {
        const int take = maxChars - n(line);
        line.insert(line.end(), w.begin(), w.begin() + take);
        lines.push_back(join(line));
        w.erase(w.begin(), w.begin() + take);
        line.clear();
      }
      line.insert(line.end(), w.begin(), w.end());
    }
    lines.push_back(join(line));
  }
  return lines;
}

AnnounceFit fitAnnouncement(const JsString& text, int count) {
  const int bottom = count > 1 ? DOT_Y - DOT_GAP : H - MARGIN;
  const int areaH = bottom - MARGIN;
  const int longest = longestWord(text);
  AnnounceFit fit;
  fit.top = MARGIN;
  fit.areaH = areaH;
  for (int scale : SCALES) {
    if (longest > lineChars(scale)) continue;
    std::vector<JsString> lines = wrapLines(text, lineChars(scale));
    if (blockHeight(static_cast<int>(lines.size()), scale) <= areaH) {
      fit.scale = scale;
      fit.lines = std::move(lines);
      return fit;
    }
  }
  const int maxChars = lineChars(1);
  std::vector<JsString> lines = wrapLines(text, maxChars);
  const int maxLines = floorDiv(areaH + 1, 9);
  fit.scale = 1;
  if (static_cast<int>(lines.size()) <= maxLines) {
    fit.lines = std::move(lines);
    return fit;
  }
  lines.resize(maxLines);
  std::vector<JsString> last = codePoints(lines[maxLines - 1]);
  if (static_cast<int>(last.size()) >= maxChars) last.resize(maxChars - 1);
  lines[maxLines - 1] = join(last) + u"~";
  fit.lines = std::move(lines);
  fit.cut = true;
  return fit;
}

void renderAnnouncement(Surface& s, const std::string& text, int index, int count) {
  s.clear();
  s.rect(0, 0, W - 1, H - 1, C.teal);
  const AnnounceFit fit = fitAnnouncement(fromUtf8(text), count);
  const int scale = fit.scale;
  int y = fit.top + floorDiv(fit.areaH - blockHeight(static_cast<int>(fit.lines.size()), scale), 2);
  for (const JsString& line : fit.lines) {
    s.text(line, floorDiv(W - textWidth(line, false, scale), 2), y, C.white, false, scale);
    y += 9 * scale;
  }
  if (count > 1) {
    const int dots = std::min(count, MAX_ANNOUNCE_SCREENS);
    const int x0 = floorDiv(W - (dots * DOT_PITCH - (DOT_PITCH - DOT)), 2);
    for (int i = 0; i < dots; i++) {
      const int x = x0 + i * DOT_PITCH;
      s.rect(x, DOT_Y, x + DOT - 1, DOT_Y + DOT - 1, i == index % dots ? C.white : C.dim);
    }
  }
}

}  // namespace render
}  // namespace wall
