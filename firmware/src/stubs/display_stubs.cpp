// Stand-in for the display half, used by the net_dev environment only.
//
// Behaves enough like the real one for the network half to be exercised on a bare board:
// messages go to the serial log, render_status() reports a frame count that advances
// (the OTA health check waits for one), and the flash-write brackets are counted so an
// unbalanced begin/end shows up in the log.
#include <Arduino.h>

#include <atomic>

#include "wall_shared.h"

namespace wall {

namespace {
std::atomic<int> flashWrites{0};
std::atomic<uint32_t> published{0};
}  // namespace

void display_begin() {}

void display_show_message(const char* line1, const char* line2) {
  Serial.printf("[wall] %s | %s\n", line1 ? line1 : "", line2 ? line2 : "");
}

void content_publish(ContentPtr content) {
  if (!content) return;
  published.fetch_add(1);
  Serial.printf("[wall] content v%u: %u athletes, %u slides, %u media, %u stories, %u posts\n",
                static_cast<unsigned>(content->version), static_cast<unsigned>(content->athletes.size()),
                static_cast<unsigned>(content->slides.size()), static_cast<unsigned>(content->media.size()),
                static_cast<unsigned>(content->instagram.stories.size()),
                static_cast<unsigned>(content->instagram.posts.size()));
}

void control_post(Control action) { Serial.printf("[wall] control %d\n", static_cast<int>(action)); }

RenderStatus render_status() {
  RenderStatus s;
  s.frames = static_cast<uint32_t>(millis() / 100);
  s.refresh_hz = 0;
  s.limiter_scale = 1;
  s.now_playing = "";
  return s;
}

void display_flash_write_begin() { flashWrites.fetch_add(1); }

void display_flash_write_end() {
  if (flashWrites.fetch_sub(1) <= 0) {
    flashWrites.store(0);
    Serial.println("[wall] display_flash_write_end without a begin");
  }
}

}  // namespace wall
