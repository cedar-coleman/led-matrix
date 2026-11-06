#include "surface.h"

#include <string.h>

#include "font5x7.h"

namespace wall {
namespace render {

int textWidth(const JsString& s, bool narrowDot, int scale) {
  int w = 0;
  forEachChar(s, [&](char16_t u, int) { w += advance(u, narrowDot); });
  return w ? (w - 1) * scale : 0;
}

void Surface::clear() { memset(buf_, 0, kFrameBytes); }

void Surface::rect(int x0, int y0, int x1, int y1, Rgb c) {
  // Same pixels as the JS double loop through put(): clip once instead of per pixel.
  if (x0 < 0) x0 = 0;
  if (y0 < 0) y0 = 0;
  if (x1 > W - 1) x1 = W - 1;
  if (y1 > H - 1) y1 = H - 1;
  for (int y = y0; y <= y1; y++) {
    uint8_t* o = buf_ + (y * W + x0) * 3;
    for (int x = x0; x <= x1; x++, o += 3) {
      o[0] = c.r;
      o[1] = c.g;
      o[2] = c.b;
    }
  }
}

void Surface::text(const JsString& s, int x, int y, Rgb c, bool narrowDot, int scale) {
  forEachChar(s, [&](char16_t u, int) {
    if (narrowDot && u == '.') {
      rect(x, y + 6 * scale, x + scale - 1, y + 7 * scale - 1, c);
    } else {
      const int g = glyphCode(u) * 5;
      for (int col = 0; col < 5; col++) {
        const uint8_t bits = kFont5x7[g + col];
        for (int row = 0; row < 8; row++) {
          if ((bits >> row) & 1) {
            rect(x + col * scale, y + row * scale, x + (col + 1) * scale - 1, y + (row + 1) * scale - 1, c);
          }
        }
      }
    }
    x += advance(u, narrowDot) * scale;
  });
}

void Surface::blit565(const uint8_t* tile, int w, int h, int x0, int y0) {
  if (!tile) return;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint8_t* o = tile + (y * w + x) * 2;
      put(x0 + x, y0 + y, rgbFrom565(static_cast<uint16_t>(o[0] | (o[1] << 8))));
    }
  }
}

namespace {

// 9x9 Instagram glyph, gradient orange (bottom-left) to magenta (top-right).
const char* const kIg[9] = {
    ".GGGGGGG.", "GWWWWWWWG", "GWGGGGWWG", "GWGGWGGWG", "GWGWGWGWG",
    "GWGGWGGWG", "GWGGGGGWG", "GWWWWWWWG", ".GGGGGGG.",
};

}  // namespace

void Surface::icon(int x0, int y0, int scale) {
  for (int y = 0; y < 9; y++) {
    for (int x = 0; x < 9; x++) {
      const char ch = kIg[y][x];
      Rgb c;
      if (ch == 'W') {
        c = {255, 255, 255};
      } else if (ch == 'G') {
        const double t = (x + (8 - y)) / 16.0;
        c = {255, static_cast<uint8_t>(jsRound(170 * (1 - t))), static_cast<uint8_t>(jsRound(40 + 160 * t))};
      } else {
        continue;
      }
      rect(x0 + x * scale, y0 + y * scale, x0 + (x + 1) * scale - 1, y0 + (y + 1) * scale - 1, c);
    }
  }
}

}  // namespace render
}  // namespace wall
