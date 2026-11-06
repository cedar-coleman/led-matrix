#include "hub75.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <esp_log.h>

#include "board_pins.h"
#include "panel_map.h"
#include "surface.h"

namespace wall {
namespace hub75 {

namespace {

const char* const TAG = "hub75";

// The ICN2037BP column driver is a plain shift register, so SHIFTREG is expected to work.
// Build with -DWALL_DRIVER_FM6126A to try the FM6126A init sequence if panels stay blank.
#ifdef WALL_DRIVER_FM6126A
constexpr auto kDriver = HUB75_I2S_CFG::FM6126A;
#else
constexpr auto kDriver = HUB75_I2S_CFG::SHIFTREG;
#endif

#ifndef WALL_COLOR_DEPTH
#define WALL_COLOR_DEPTH 6  // 144 KB of DMA RAM at 6 bits vs 192 KB at 8; see design.md section 3
#endif

// The one pixel-clock knob. A 768-pixel chain refreshes slowly, so Phase 0 raises this to
// the fastest the panels take without ghosting (HZ_16M, then HZ_20M). Note the library's
// HZ_10M is really 8 MHz and HZ_15M is 16 MHz.
constexpr auto kPixelClock = HUB75_I2S_CFG::HZ_8M;

MatrixPanel_I2S_DMA* panel = nullptr;

}  // namespace

bool begin() {
  HUB75_I2S_CFG::i2s_pins pins = {pins::R1, pins::G1, pins::B1, pins::R2, pins::G2, pins::B2, pins::A,
                                  pins::B,  pins::C,  pins::D,  pins::E,  pins::LAT, pins::OE, pins::CLK};
  HUB75_I2S_CFG cfg(render::kPanelW, render::kPanelH, render::kChainLength, pins, kDriver, HUB75_I2S_CFG::TYPE138,
                    false /* single buffer */, kPixelClock, 0 /* latch blanking: auto */, true /* clock phase */,
                    60 /* min refresh Hz */, WALL_COLOR_DEPTH);
  panel = new MatrixPanel_I2S_DMA(cfg);
  if (!panel->begin()) {
    ESP_LOGE(TAG, "HUB75 init failed: not enough internal DMA RAM for %d-bit colour?", WALL_COLOR_DEPTH);
    delete panel;
    panel = nullptr;
    return false;
  }
  panel->clearScreen();
  ESP_LOGI(TAG, "HUB75 %dx%d x%d, %d-bit colour, refresh %d Hz", render::kPanelW, render::kPanelH,
           render::kChainLength, WALL_COLOR_DEPTH, panel->calculated_refresh_rate);
  return true;
}

void push(const uint8_t* rgb) {
  if (!panel) return;
  const uint8_t* p = rgb;
  for (int y = 0; y < render::H; y++) {
    for (int x = 0; x < render::W; x++, p += 3) {
      const render::ChainXY c = render::chainXY(x, y);
      panel->drawPixelRGB888(c.x, c.y, p[0], p[1], p[2]);
    }
  }
}

void setBrightnessPct(int pct) {
  if (!panel) return;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  panel->setBrightness8(static_cast<uint8_t>((pct * 255 + 50) / 100));
}

float refreshHz() { return panel ? static_cast<float>(panel->calculated_refresh_rate) : 0.0f; }

}  // namespace hub75
}  // namespace wall
