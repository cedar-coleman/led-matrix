#include "gif_player.h"

#include <AnimatedGIF.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include <string.h>

#include <new>

#include "surface.h"
#include "wall_shared.h"

namespace wall {
namespace gif {

namespace {

const char* const TAG = "gif";
AnimatedGIF* decoder = nullptr;
bool isOpen = false;

void* openFile(const char* path, int32_t* size) {
  File f = LittleFS.open(path, "r");
  if (!f) return nullptr;
  *size = static_cast<int32_t>(f.size());
  return new File(f);
}

void closeFile(void* handle) {
  File* f = static_cast<File*>(handle);
  if (!f) return;
  f->close();
  delete f;
}

int32_t readFile(GIFFILE* gf, uint8_t* buf, int32_t len) {
  File* f = static_cast<File*>(gf->fHandle);
  // The decoder asks for more than is left at the end of the file.
  if (len > gf->iSize - gf->iPos) len = gf->iSize - gf->iPos;
  if (len <= 0) return 0;
  const int32_t n = static_cast<int32_t>(f->read(buf, static_cast<size_t>(len)));
  gf->iPos = static_cast<int32_t>(f->position());
  return n;
}

int32_t seekFile(GIFFILE* gf, int32_t pos) {
  File* f = static_cast<File*>(gf->fHandle);
  f->seek(static_cast<uint32_t>(pos));
  gf->iPos = static_cast<int32_t>(f->position());
  return gf->iPos;
}

// One decoded line. Transparent pixels keep what the previous frame left there, which is
// how frame-differenced GIFs (gifenc, most tools) expect to be composed.
void drawLine(GIFDRAW* d) {
  uint8_t* canvas = static_cast<uint8_t*>(d->pUser);
  const int y = d->iY + d->y;
  if (y < 0 || y >= render::H) return;
  const uint8_t* pal = d->pPalette24;
  for (int i = 0; i < d->iWidth; i++) {
    const int x = d->iX + i;
    if (x < 0 || x >= render::W) continue;
    const uint8_t idx = d->pPixels[i];
    if (d->ucHasTransparency && idx == d->ucTransparent) continue;
    uint8_t* o = canvas + (y * render::W + x) * 3;
    o[0] = pal[idx * 3];
    o[1] = pal[idx * 3 + 1];
    o[2] = pal[idx * 3 + 2];
  }
}

// Browsers show a 0 or 10 ms frame for 100 ms; the console preview is a browser, so match it.
int frameDelay(int ms) { return ms <= 10 ? 100 : ms; }

}  // namespace

bool init() {
  if (decoder) return true;
  void* mem = heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) {
    ESP_LOGE(TAG, "no PSRAM for the GIF decoder");
    return false;
  }
  decoder = new (mem) AnimatedGIF();
  decoder->begin(GIF_PALETTE_RGB888);
  return true;
}

bool open(const std::string& name, uint8_t* canvas, int* delayMs) {
  close();
  if (!decoder) return false;
  const std::string path = std::string(kMediaDir) + "/" + name;
  if (!decoder->open(path.c_str(), openFile, closeFile, readFile, seekFile, drawLine)) {
    ESP_LOGW(TAG, "can't open %s (error %d); skipping the screen", path.c_str(), decoder->getLastError());
    return false;
  }
  isOpen = true;
  if (decoder->getCanvasWidth() != render::W || decoder->getCanvasHeight() != render::H) {
    ESP_LOGW(TAG, "%s is %dx%d, not %dx%d; drawing it clipped", path.c_str(), decoder->getCanvasWidth(),
             decoder->getCanvasHeight(), render::W, render::H);
  }
  memset(canvas, 0, render::kFrameBytes);
  if (!next(canvas, delayMs)) {
    ESP_LOGW(TAG, "%s: first frame won't decode (error %d); skipping the screen", path.c_str(),
             decoder->getLastError());
    close();
    return false;
  }
  return true;
}

bool next(uint8_t* canvas, int* delayMs) {
  if (!isOpen) return false;
  int ms = 0;
  const int rc = decoder->playFrame(false, &ms, canvas);
  if (rc < 0) {
    ESP_LOGW(TAG, "decode error %d", decoder->getLastError());
    return false;
  }
  if (rc == 0) decoder->reset();  // last frame: loop until the screen time is up
  *delayMs = frameDelay(ms);
  return true;
}

void close() {
  if (decoder && isOpen) decoder->close();
  isOpen = false;
}

}  // namespace gif
}  // namespace wall
