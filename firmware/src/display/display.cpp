// The display half: render task, rotation, slide renderers and the power limiter.
// See design.md section 7 and include/wall_shared.h.
//
// One task on core 1 owns the canvas, the panel and the GIF decoder; the public
// functions below only hand it messages under a mutex or through a queue, so the network
// half on core 0 never touches display state directly.
//
// Messages (display_show_message) stay on the wall until the next content_publish, or
// until display_show_message is called with two empty lines.
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "gif_player.h"
#include "hub75.h"
#include "igslides.h"
#include "layout.h"
#include "power.h"
#include "rotation.h"
#include "screens.h"
#include "surface.h"
#include "wall_shared.h"

namespace wall {

namespace {

const char* const TAG = "display";

using render::Screen;
using render::ScreenKind;

constexpr uint32_t kStoryRedrawMs = 100;  // the progress bar moves; the console redraws at the same rate
constexpr uint32_t kIdleTickMs = 20;
constexpr uint32_t kAllFailedRetryMs = 1000;

// ---- shared with the other core, under `lock` ------------------------------------------

SemaphoreHandle_t lock = nullptr;
QueueHandle_t controls = nullptr;
std::atomic<int> flashWrites{0};

ContentPtr pending;  // newest published content the render task hasn't taken yet
bool messageActive = false;
bool messageChanged = false;
std::string message1, message2;
RenderStatus status;

struct Locked {
  Locked() { xSemaphoreTake(lock, portMAX_DELAY); }
  ~Locked() { xSemaphoreGive(lock); }
};

// ---- owned by the render task -----------------------------------------------------------

uint8_t* canvas = nullptr;      // 256x96 RGB888 in PSRAM
uint8_t* storyTile = nullptr;   // PSRAM tile buffers
uint8_t* postTiles[render::POST_COUNT] = {};

// Rotation state. `content` is what the current screen was drawn from; `incoming` waits
// for the next screen boundary unless the current screen disappears from it.
struct Rotation {
  ContentPtr content;
  ContentPtr incoming;
  std::vector<const Athlete*> athletes;
  render::Fit fit;
  std::vector<render::Screen> seq;
  int index = -1;
  uint32_t startMs = 0;
  uint32_t pausedAtMs = 0;
  bool paused = false;
  int heldPct = 50;           // brightness held for this screen
  uint32_t lastDrawMs = 0;
  uint32_t nextGifFrameMs = 0;
  int failures = 0;           // consecutive screens skipped because they can't be drawn
  uint32_t idleUntilMs = 0;   // after every screen failed, wait before trying again
} rot;

int appliedPct = -1;

void setStatus(const std::string& label, float scale) {
  Locked l;
  status.now_playing = label;
  status.limiter_scale = scale;
  status.paused = rot.paused;
}

// The per-frame backstop: whatever the held brightness, no frame may push a supply past
// its budget. Then copy to the panel.
void present() {
  const int pct = render::limitedBrightness(canvas, rot.heldPct);
  if (pct != appliedPct) {
    hub75::setBrightnessPct(pct);
    appliedPct = pct;
  }
  hub75::push(canvas);
  Locked l;
  status.limiter_scale = rot.heldPct > 0 ? static_cast<float>(pct) / rot.heldPct : 1.0f;
  status.frames++;
}

int settingPct() { return rot.content ? rot.content->settings.brightness : 50; }

// Whole-file read of a tile; anything missing or the wrong size draws black.
const uint8_t* loadTile(const std::string& path, uint8_t* buf, size_t bytes) {
  if (path.empty() || !buf) return nullptr;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) return nullptr;
  const bool ok = f.size() == bytes && f.read(buf, bytes) == bytes;
  f.close();
  if (!ok) ESP_LOGW(TAG, "tile %s is not %u bytes; drawing black", path.c_str(), static_cast<unsigned>(bytes));
  return ok ? buf : nullptr;
}

void adopt(ContentPtr c) {
  rot.content = std::move(c);
  rot.athletes = render::whiteboardAthletes(rot.content->athletes, rot.content->settings.sort);
  rot.fit = render::fitWhiteboard(rot.athletes);
  rot.seq = render::sequence(*rot.content, rot.fit.pageCount);
}

void drawStory(uint32_t now) {
  const Screen& sc = rot.seq[rot.index];
  const IgContent& ig = rot.content->instagram;
  const double progress = std::min(1.0, (now - rot.startMs) / (sc.seconds * 1000.0));
  render::StoryView view;
  const bool has = sc.index < static_cast<int>(ig.stories.size());
  if (has) {
    view.tile = storyTile;
    view.timestamp = ig.stories[sc.index].timestamp;
  }
  render::Surface s(canvas);
  render::renderStorySlide(s, ig.username, has ? &view : nullptr, sc.index, static_cast<int>(ig.stories.size()),
                           progress, wall_unix_time());
}

// Draw the first frame of rot.seq[rot.index] and fix this screen's brightness.
// False if the screen can't be shown (a GIF that won't open or decode).
bool startScreen(uint32_t now) {
  const Screen& sc = rot.seq[rot.index];
  const Content& c = *rot.content;
  render::Surface s(canvas);
  rot.startMs = now;
  rot.pausedAtMs = now;
  rot.lastDrawMs = now;
  gif::close();

  switch (sc.kind) {
    case ScreenKind::kWhiteboard: {
      render::WhiteboardOptions opts;
      opts.title = c.settings.title;
      opts.prDays = c.settings.pr_highlight_days;
      opts.today = wall_local_date();  // "" until the clock is set: no green PRs
      render::renderWhiteboardPage(s, rot.fit, sc.index, opts);
      rot.heldPct = render::limitedBrightness(canvas, settingPct());
      break;
    }
    case ScreenKind::kStory: {
      const auto& stories = c.instagram.stories;
      const bool has = sc.index < static_cast<int>(stories.size());
      if (!has || !loadTile(stories[sc.index].tile_path, storyTile, render::kStoryTileBytes)) {
        memset(storyTile, 0, render::kStoryTileBytes);  // missing tile: black, as in the JS
      }
      drawStory(now);
      rot.heldPct = render::limitedBrightness(canvas, settingPct());
      break;
    }
    case ScreenKind::kPosts: {
      std::vector<render::PostView> posts;
      const auto& items = c.instagram.posts;
      for (size_t i = 0; i < items.size() && i < render::POST_COUNT; i++) {
        render::PostView v;
        v.tile = loadTile(items[i].tile_path, postTiles[i], render::kPostTileBytes);
        v.timestamp = items[i].timestamp;
        v.kind = items[i].kind;
        v.like_count = items[i].like_count;
        posts.push_back(v);
      }
      render::renderPostsSlide(s, c.instagram.username, posts, wall_unix_time());
      rot.heldPct = render::limitedBrightness(canvas, settingPct());
      break;
    }
    case ScreenKind::kMedia: {
      int delayMs = 100;
      if (!gif::open(sc.file, canvas, &delayMs)) return false;
      rot.nextGifFrameMs = now + delayMs;
      // Hold one brightness for the whole slide so bright frames don't pump it. The
      // uploader measured the brightest frame; without that, the first frame stands in.
      float peak = -1;
      for (const MediaInfo& m : c.media) {
        if (m.name == sc.file) peak = m.peak_a;
      }
      if (peak < 0) {
        const render::SupplyAmps a = render::estimateSupplyAmps(canvas, 100);
        peak = static_cast<float>(std::max(a.a, a.b));
      }
      rot.heldPct = render::slideBrightness(peak, settingPct());
      break;
    }
  }
  setStatus(sc.label, 1);
  present();
  return true;
}

void showNothing(const char* label) {
  gif::close();
  memset(canvas, 0, render::kFrameBytes);
  rot.heldPct = settingPct();
  setStatus(label, 1);
  present();
}

// Move delta screens on (1 = next, -1 = previous), taking up waiting content first.
// Screens that can't be drawn are skipped in the same direction.
void advance(int delta, uint32_t now) {
  if (rot.incoming) {
    const Screen current = rot.index >= 0 && rot.index < static_cast<int>(rot.seq.size()) ? rot.seq[rot.index] : Screen();
    const bool hadScreen = rot.index >= 0 && rot.index < static_cast<int>(rot.seq.size());
    adopt(std::move(rot.incoming));
    rot.incoming.reset();
    const int at = hadScreen ? render::findScreen(rot.seq, current) : -1;
    rot.index = at >= 0 ? at : rot.index - (delta > 0 ? 1 : 0);
  }
  const int n = static_cast<int>(rot.seq.size());
  if (!rot.content || n == 0) {
    rot.index = -1;
    showNothing(rot.content ? "Nothing in the rotation" : "Waiting for content");
    return;
  }
  for (int tries = 0; tries < n; tries++) {
    rot.index = rot.index < 0 ? 0 : ((rot.index + delta) % n + n) % n;
    if (startScreen(now)) {
      rot.failures = 0;
      return;
    }
    ESP_LOGW(TAG, "skipping %s", rot.seq[rot.index].label.c_str());
    if (delta == 0) delta = 1;
  }
  // Every screen failed (for example only media, all unreadable): show black and retry
  // later instead of spinning on the filesystem.
  showNothing("Nothing playable in the rotation");
  rot.idleUntilMs = now + kAllFailedRetryMs;
}

// New content arrived. Keep the current screen until it ends, unless it no longer exists.
void onContent(ContentPtr c, uint32_t now) {
  rot.incoming = std::move(c);
  if (!rot.content || rot.index < 0) {
    advance(0, now);
    return;
  }
  render::Fit fit = render::fitWhiteboard(render::whiteboardAthletes(rot.incoming->athletes, rot.incoming->settings.sort));
  const std::vector<Screen> next = render::sequence(*rot.incoming, fit.pageCount);
  if (render::findScreen(next, rot.seq[rot.index]) < 0) advance(0, now);
}

void onControl(Control action, uint32_t now) {
  switch (action) {
    case Control::kNext:
      advance(1, now);
      break;
    case Control::kPrev:
      advance(-1, now);
      break;
    case Control::kPause:
      if (!rot.paused) {
        rot.paused = true;
        rot.pausedAtMs = now;
      }
      break;
    case Control::kResume:
      if (rot.paused) {
        rot.paused = false;
        rot.startMs += now - rot.pausedAtMs;  // the screen keeps the time it had left
      }
      break;
  }
  Locked l;
  status.paused = rot.paused;
}

// One pass of the render loop. Returns how long to sleep.
uint32_t tick() {
  const uint32_t now = millis();

  // Flash erase/write stalls PSRAM and the cache; hold the current frame (the DMA keeps
  // refreshing it from internal RAM) and read nothing from flash until the write ends.
  if (flashWrites.load() > 0) return kIdleTickMs;

  ContentPtr fresh;
  bool showMessage = false, messageNew = false;
  std::string l1, l2;
  {
    Locked l;
    fresh = std::move(pending);
    pending.reset();
    if (fresh) messageActive = false;
    showMessage = messageActive;
    messageNew = messageChanged;
    messageChanged = false;
    if (messageNew) {
      l1 = message1;
      l2 = message2;
    }
  }

  static bool showingMessage = false;
  if (showMessage) {
    showingMessage = true;
    Control ignored;  // the wall is showing setup or error text; there is nothing to step through
    while (controls && xQueueReceive(controls, &ignored, 0) == pdTRUE) {
    }
    if (messageNew) {
      gif::close();
      render::Surface s(canvas);
      render::renderMessage(s, l1, l2);
      rot.heldPct = render::limitedBrightness(canvas, settingPct());
      setStatus("Message: " + l1, 1);
      present();
    }
    return kIdleTickMs * 5;
  }
  if (showingMessage) {
    // Message gone (new content, or cleared): start the rotation over from the top.
    showingMessage = false;
    if (fresh) rot.incoming = std::move(fresh);
    fresh.reset();
    rot.index = -1;
    advance(0, now);
  }

  if (fresh) onContent(std::move(fresh), now);

  Control action;
  while (controls && xQueueReceive(controls, &action, 0) == pdTRUE) onControl(action, now);

  if (rot.idleUntilMs) {
    if (static_cast<int32_t>(now - rot.idleUntilMs) < 0) return kIdleTickMs * 5;
    rot.idleUntilMs = 0;
    advance(1, now);
  }
  if (rot.index < 0 || rot.index >= static_cast<int>(rot.seq.size())) return kIdleTickMs * 5;

  const Screen& sc = rot.seq[rot.index];
  const uint32_t elapsed = (rot.paused ? rot.pausedAtMs : now) - rot.startMs;
  if (!rot.paused && elapsed >= sc.seconds * 1000u) {
    advance(1, now);
    return 1;
  }

  if (sc.kind == ScreenKind::kStory && now - rot.lastDrawMs >= kStoryRedrawMs && !rot.paused) {
    rot.lastDrawMs = now;
    drawStory(now);
    present();
  } else if (sc.kind == ScreenKind::kMedia && static_cast<int32_t>(now - rot.nextGifFrameMs) >= 0) {
    int delayMs = 100;
    if (!gif::next(canvas, &delayMs)) {
      ESP_LOGW(TAG, "%s stopped decoding; skipping it", sc.file.c_str());
      advance(1, now);
      return 1;
    }
    // Frame times are kept on a schedule, not relative to when the decode finished, so a
    // slow decode doesn't stretch the animation.
    rot.nextGifFrameMs += delayMs;
    if (static_cast<int32_t>(now - rot.nextGifFrameMs) > 500) rot.nextGifFrameMs = now + delayMs;
    present();
  }

  uint32_t sleep = kIdleTickMs;
  if (sc.kind == ScreenKind::kMedia) {
    const int32_t untilFrame = static_cast<int32_t>(rot.nextGifFrameMs - millis());
    sleep = untilFrame <= 1 ? 1 : std::min<uint32_t>(untilFrame, kIdleTickMs);
  }
  return sleep;
}

void renderTask(void*) {
  gif::init();
#ifdef WALL_TEST_PATTERN
  // Phase 0 bring-up: panel order and orientation only; content is ignored.
  render::Surface s(canvas);
  render::renderTestPattern(s);
  rot.heldPct = 30;
  setStatus("Test pattern", 1);
  present();
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#else
  for (;;) vTaskDelay(pdMS_TO_TICKS(tick()));
#endif
}

uint8_t* psramAlloc(size_t bytes) {
  auto* p = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!p) {
    ESP_LOGE(TAG, "no PSRAM for %u bytes; using internal RAM", static_cast<unsigned>(bytes));
    p = static_cast<uint8_t*>(malloc(bytes));
  }
  if (p) memset(p, 0, bytes);
  return p;
}

}  // namespace

void display_begin() {
  // First, before Wi-Fi or anything else fragments internal RAM: the DMA buffers.
  hub75::begin();

  lock = xSemaphoreCreateMutex();
  controls = xQueueCreate(8, sizeof(Control));
  canvas = psramAlloc(render::kFrameBytes);
  storyTile = psramAlloc(render::kStoryTileBytes);
  for (auto& t : postTiles) t = psramAlloc(render::kPostTileBytes);
  {
    Locked l;
    status.refresh_hz = hub75::refreshHz();
  }
  ESP_LOGI(TAG, "internal free %u (largest %u), PSRAM free %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  // Core 1, above the Arduino loop (priority 1) so frame timing holds; it sleeps between
  // frames, so loop() still runs.
  xTaskCreatePinnedToCore(renderTask, "render", 8192, nullptr, 3, nullptr, 1);
}

void display_show_message(const char* line1, const char* line2) {
  if (!lock) return;
  Locked l;
  message1 = line1 ? line1 : "";
  message2 = line2 ? line2 : "";
  messageActive = !(message1.empty() && message2.empty());
  messageChanged = true;
}

void content_publish(ContentPtr content) {
  if (!lock || !content) return;
  Locked l;
  pending = std::move(content);
}

void control_post(Control action) {
  if (controls) xQueueSend(controls, &action, 0);
}

RenderStatus render_status() {
  if (!lock) return {};
  Locked l;
  return status;
}

void display_flash_write_begin() { flashWrites.fetch_add(1); }
void display_flash_write_end() {
  if (flashWrites.fetch_sub(1) <= 0) flashWrites.store(0);  // unbalanced end: don't go negative
}

}  // namespace wall
