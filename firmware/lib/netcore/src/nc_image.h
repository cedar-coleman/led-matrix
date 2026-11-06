// Instagram tiles: cover crop of a decoded JPEG, and the two files written per tile
// (raw RGB565 for the renderer, 24-bit BMP for the console's preview).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

namespace netcore {

// JPEGDEC's reduction (1, 2, 4 or 8): the largest that still leaves at least tw x th
// pixels, so the crop never upscales when the source is big enough.
int jpegScaleFor(int w, int h, int tw, int th);

// Center crop to the tile's aspect ratio, then area-average down (or nearest up) to
// tw x th. Same framing as Pillow's ImageOps.fit with the default centering, which the
// mock uses. src: RGB565 little-endian, row-major, `stride` pixels per row.
// out: tw * th * 3 bytes RGB888.
void coverCrop565(const uint16_t* src, int w, int h, int stride, int tw, int th, uint8_t* out);

// RGB888 to RGB565 little-endian bytes (what the display's loadTile expects).
std::vector<uint8_t> toRgb565(const uint8_t* rgb, int w, int h);

// A 24-bit bottom-up BMP, as Pillow writes it.
std::vector<uint8_t> toBmp24(const uint8_t* rgb, int w, int h);

}  // namespace netcore
