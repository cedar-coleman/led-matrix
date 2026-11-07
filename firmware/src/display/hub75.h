// The HUB75 chain: twelve 64x32 1/16-scan panels on one output, driven by
// ESP32-HUB75-MatrixPanel-DMA built with NO_GFX. The serpentine layout is mapped by
// lib/render/src/panel_map.h, not the library's VirtualMatrixPanel.
#pragma once

#include <stdint.h>

namespace wall {
namespace hub75 {

// Allocates the DMA bit-plane buffers (internal SRAM) and starts output. Call before
// anything else allocates, so the buffers find contiguous internal RAM.
bool begin();

// Copy a 256x96 RGB888 canvas to the DMA buffer. Single buffered: the copy takes a few
// milliseconds and only those rows can tear.
void push(const uint8_t* rgb);

// 0..100 percent of the driver's full brightness.
void setBrightnessPct(int pct);

// Display off (schedule or sleep): brightness 0 and the DMA buffer cleared, so the panels
// are black even if the output-enable timing leaks. The caller's canvas is untouched.
void blank();

float refreshHz();  // as computed by the library at begin()

}  // namespace hub75
}  // namespace wall
