#include "nc_image.h"

#include <math.h>
#include <string.h>

#include <algorithm>

namespace netcore {

int jpegScaleFor(int w, int h, int tw, int th) {
  int s = 1;
  while (s < 8 && w / (s * 2) >= tw && h / (s * 2) >= th) s *= 2;
  return s;
}

void coverCrop565(const uint16_t* src, int w, int h, int stride, int tw, int th, uint8_t* out) {
  // Crop rectangle in source pixels (fractional), centered.
  double cw = w, ch = h;
  if (static_cast<double>(w) * th > static_cast<double>(h) * tw) {
    cw = static_cast<double>(h) * tw / th;
  } else {
    ch = static_cast<double>(w) * th / tw;
  }
  const double x0 = (w - cw) / 2, y0 = (h - ch) / 2;
  const double sx = cw / tw, sy = ch / th;

  auto channel = [](uint16_t v, int c) -> double {
    if (c == 0) return ((v >> 11) & 31) * 255.0 / 31;
    if (c == 1) return ((v >> 5) & 63) * 255.0 / 63;
    return (v & 31) * 255.0 / 31;
  };

  for (int ty = 0; ty < th; ty++) {
    const double fy0 = y0 + ty * sy, fy1 = fy0 + sy;
    for (int tx = 0; tx < tw; tx++) {
      const double fx0 = x0 + tx * sx, fx1 = fx0 + sx;
      double acc[3] = {0, 0, 0}, wsum = 0;
      if (sx < 1 || sy < 1) {
        // Upscaling (a source smaller than the tile): nearest pixel.
        const int px = std::min(w - 1, static_cast<int>((fx0 + fx1) / 2));
        const int py = std::min(h - 1, static_cast<int>((fy0 + fy1) / 2));
        const uint16_t v = src[py * stride + px];
        for (int c = 0; c < 3; c++) acc[c] = channel(v, c);
        wsum = 1;
      } else {
        for (int py = static_cast<int>(floor(fy0)); py < static_cast<int>(ceil(fy1)) && py < h; py++) {
          const double wy = std::min(fy1, py + 1.0) - std::max(fy0, static_cast<double>(py));
          if (wy <= 0) continue;
          for (int px = static_cast<int>(floor(fx0)); px < static_cast<int>(ceil(fx1)) && px < w; px++) {
            const double wx = std::min(fx1, px + 1.0) - std::max(fx0, static_cast<double>(px));
            if (wx <= 0) continue;
            const uint16_t v = src[py * stride + px];
            for (int c = 0; c < 3; c++) acc[c] += channel(v, c) * wx * wy;
            wsum += wx * wy;
          }
        }
      }
      uint8_t* o = out + (ty * tw + tx) * 3;
      for (int c = 0; c < 3; c++) {
        const double v = wsum > 0 ? acc[c] / wsum : 0;
        o[c] = static_cast<uint8_t>(std::min(255.0, std::max(0.0, v + 0.5)));
      }
    }
  }
}

std::vector<uint8_t> toRgb565(const uint8_t* rgb, int w, int h) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 2);
  for (int i = 0; i < w * h; i++) {
    const uint8_t* p = rgb + i * 3;
    const uint16_t v = static_cast<uint16_t>((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | (p[2] >> 3));
    out[2 * i] = v & 0xFF;
    out[2 * i + 1] = v >> 8;
  }
  return out;
}

std::vector<uint8_t> toBmp24(const uint8_t* rgb, int w, int h) {
  const int row = (w * 3 + 3) & ~3;
  const uint32_t pixels = static_cast<uint32_t>(row) * h, size = 54 + pixels;
  std::vector<uint8_t> b(size, 0);
  auto le32 = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
  };
  b[0] = 'B', b[1] = 'M';
  le32(2, size);
  le32(10, 54);
  le32(14, 40);
  le32(18, static_cast<uint32_t>(w));
  le32(22, static_cast<uint32_t>(h));  // positive: rows stored bottom-up
  b[26] = 1;
  b[28] = 24;
  le32(34, pixels);
  le32(38, 2835);  // 72 dpi, as Pillow writes
  le32(42, 2835);
  for (int y = 0; y < h; y++) {
    uint8_t* dst = b.data() + 54 + static_cast<size_t>(h - 1 - y) * row;
    for (int x = 0; x < w; x++) {
      const uint8_t* p = rgb + (y * w + x) * 3;
      dst[3 * x] = p[2], dst[3 * x + 1] = p[1], dst[3 * x + 2] = p[0];
    }
  }
  return b;
}

}  // namespace netcore
