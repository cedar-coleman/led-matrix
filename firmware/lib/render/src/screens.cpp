#include "screens.h"

#include <algorithm>

#include "panel_map.h"

namespace wall {
namespace render {

namespace {

struct Line {
  JsString text;
  int scale;
};

Line fitLine(const std::string& utf8) {
  JsString t = fromUtf8(utf8);
  if (textWidth(t, false, 2) <= W - 2) return {t, 2};
  const long maxChars = (W - 2 + 1) / 6;  // 42 at normal size
  if (static_cast<long>(t.size()) > maxChars) t = jsSlice(t, 0, maxChars - 1) + u"~";
  return {t, 1};
}

}  // namespace

void renderMessage(Surface& s, const std::string& line1, const std::string& line2) {
  s.clear();
  const Line a = fitLine(line1), b = fitLine(line2);
  const int ha = a.text.empty() ? 0 : 8 * a.scale;
  const int hb = b.text.empty() ? 0 : 8 * b.scale;
  const int gap = ha && hb ? 6 : 0;
  int y = floorDiv(H - (ha + gap + hb), 2);
  const Rgb white{255, 255, 255}, teal{0, 150, 150};
  if (ha) {
    s.text(a.text, floorDiv(W - textWidth(a.text, false, a.scale), 2), y, white, false, a.scale);
    y += ha + gap;
  }
  if (hb) s.text(b.text, floorDiv(W - textWidth(b.text, false, b.scale), 2), y, teal, false, b.scale);
}

void renderTestPattern(Surface& s) {
  s.clear();
  for (int row = 0; row < kPanelRows; row++) {
    for (int col = 0; col < kPanelCols; col++) {
      const int x0 = col * kPanelW, y0 = row * kPanelH;
      const int x1 = x0 + kPanelW - 1, y1 = y0 + kPanelH - 1;
      const int n = chainIndex(col, row) + 1;
      const Rgb c = n <= 6 ? Rgb{0, 200, 200} : Rgb{255, 140, 0};
      s.rect(x0, y0, x1, y0, c);
      s.rect(x0, y1, x1, y1, c);
      s.rect(x0, y0, x0, y1, c);
      s.rect(x1, y0, x1, y1, c);
      s.text(std::to_string(n), x0 + 6, y0 + 9, Rgb{255, 255, 255}, false, 2);
      // Arrow towards the top of the wall: 5-wide head over a 2-wide shaft.
      const int ax = x0 + 48;
      for (int i = 0; i < 5; i++) s.rect(ax - i, y0 + 5 + i, ax + 1 + i, y0 + 5 + i, Rgb{255, 255, 0});
      s.rect(ax, y0 + 10, ax + 1, y0 + 25, Rgb{255, 255, 0});
    }
  }
  // The board's corner: a red pixel block where chain panel 1 starts (top left), so a
  // chain that starts at the wrong end is obvious even before reading numbers.
  s.rect(1, 1, 3, 3, Rgb{255, 0, 0});
}

}  // namespace render
}  // namespace wall
