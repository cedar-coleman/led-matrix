// HUB75 pins of the Waveshare ESP32-S3-RGB-Matrix (module ESP32-S3-WROOM-2-N32R16V).
//
// Source: Waveshare's own examples, github.com/waveshareteam/ESP32-S3-RGB-Matrix at
// commit 6e92fbe (2026-10-14), linked from docs.waveshare.com/ESP32-S3-RGB-Matrix/Arduino:
//   - example/arduino_v3.3.7/*/platforms/esp32s3/esp32s3-default-pins.hpp, the pin header
//     Waveshare ships inside every Arduino example (identical in all ten);
//   - example/idf_v5.5.2/sdkconfig.defaults (CONFIG_HUB75_PIN_*), which agrees pin for pin.
// The library's own S3 defaults match except E, which is -1 there; Waveshare wires E to
// GPIO 9 and its examples set mxconfig.gpio.e = 9. Our 64x32 1/16-scan panels don't use
// E (it's for 1/32 scan), but it's assigned as the board wires it so a 64x64 panel works too.
//
// Pins to keep clear of: 33-37 (octal PSRAM/flash), 1/17/44 (TF card, SDMMC 1-bit),
// 47/48 (I2C: RTC and sensors), 19/20 (USB).
#pragma once

#include <stdint.h>

namespace wall {
namespace pins {

constexpr int8_t R1 = 4;
constexpr int8_t G1 = 5;
constexpr int8_t B1 = 6;
constexpr int8_t R2 = 7;
constexpr int8_t G2 = 15;
constexpr int8_t B2 = 16;
constexpr int8_t A = 18;
constexpr int8_t B = 8;
constexpr int8_t C = 3;
constexpr int8_t D = 42;
constexpr int8_t E = 9;
constexpr int8_t LAT = 40;
constexpr int8_t OE = 2;
constexpr int8_t CLK = 41;

}  // namespace pins
}  // namespace wall
