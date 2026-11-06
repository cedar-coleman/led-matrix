// The 256x96 drawing surface: a port of createSurface() in web/layout.js.
//
// The JS surface is RGBA with alpha always 255; this one is RGB888 (3 bytes per pixel,
// row-major), which is what the render task composes in PSRAM and copies to the panel.
// The memory belongs to the caller, so the firmware can put it in PSRAM and the host
// tests on the stack or heap.
#pragma once

#include <stdint.h>

#include <string>

#include "jsport.h"

namespace wall {
namespace render {

constexpr int W = 256;
constexpr int H = 96;
constexpr int kFrameBytes = W * H * 3;

struct Rgb {
  uint8_t r, g, b;
};

// RGB565 little-endian, as stored for Instagram tiles, to RGB888. Each channel's top bits
// are repeated into the low bits so full scale stays full scale (31 -> 255, 63 -> 255).
inline Rgb rgbFrom565(uint16_t v) {
  const unsigned r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  return {static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)),
          static_cast<uint8_t>((b << 3) | (b >> 2))};
}

// Characters outside printable ASCII have no glyph in the 5x7 font; show '?'.
// The one exception is the heart, which the font has at code 3.
inline int glyphCode(char16_t firstUnit) {
  if (firstUnit == 0x2665) return 3;
  return firstUnit >= 32 && firstUnit <= 126 ? firstUnit : 63;
}

// In numbers '.' is one baseline pixel with a 2 px advance, so "250.5" is 25 px wide.
inline int advance(char16_t firstUnit, bool narrowDot) { return narrowDot && firstUnit == '.' ? 2 : 6; }

int textWidth(const JsString& s, bool narrowDot = false, int scale = 1);
inline int textWidth(const std::string& utf8, bool narrowDot = false, int scale = 1) {
  return textWidth(fromUtf8(utf8), narrowDot, scale);
}

class Surface {
 public:
  explicit Surface(uint8_t* rgb) : buf_(rgb) {}

  uint8_t* data() const { return buf_; }
  void clear();  // black, as a fresh createSurface()

  void put(int x, int y, Rgb c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t* o = buf_ + (y * W + x) * 3;
    o[0] = c.r;
    o[1] = c.g;
    o[2] = c.b;
  }
  void rect(int x0, int y0, int x1, int y1, Rgb c);
  // scale 2 doubles every font pixel, for headline text.
  void text(const JsString& s, int x, int y, Rgb c, bool narrowDot = false, int scale = 1);
  void text(const std::string& utf8, int x, int y, Rgb c, bool narrowDot = false, int scale = 1) {
    text(fromUtf8(utf8), x, y, c, narrowDot, scale);
  }
  // Copy a w x h RGB565 little-endian tile to (x0, y0). A null tile draws nothing,
  // leaving black, as the JS does for a tile that hasn't loaded.
  void blit565(const uint8_t* tile, int w, int h, int x0, int y0);
  // The 9x9 Instagram glyph.
  void icon(int x0, int y0, int scale = 1);

 private:
  uint8_t* buf_;
};

}  // namespace render
}  // namespace wall
