// Plays a 256x96 GIF from LittleFS into the RGB888 canvas, one frame per call.
#pragma once

#include <stdint.h>

#include <string>

namespace wall {
namespace gif {

// Allocates the decoder (about 27 KB) in PSRAM. Call once from the render task.
bool init();

// Opens kMediaDir/name and decodes its first frame into canvas. False if the file is
// missing or not a GIF the decoder can read; the caller skips the screen.
bool open(const std::string& name, uint8_t* canvas, int* delayMs);

// Decodes the next frame, looping back to the first after the last. False on a decode
// error. delayMs: how long to show the frame just drawn.
bool next(uint8_t* canvas, int* delayMs);

void close();

}  // namespace gif
}  // namespace wall
