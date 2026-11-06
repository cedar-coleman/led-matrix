// The interface between the two halves of the firmware.
//
//   src/net/      Wi-Fi, web server and REST API, storage, PIN, time, Instagram fetcher, OTA.
//   src/display/  HUB75 driver, render task, rotation, GIF player, slide renderers, power limiter.
//
// Neither half includes the other's headers. Everything they exchange is declared here,
// and src/stubs/ holds a stand-in for each half so the other can be built alone.
// API contract for the network half: ../API.md and ../mock_server.py.
#pragma once

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace wall {

constexpr int kWidth = 256;
constexpr int kHeight = 96;

// Same order as LIFTS in web/layout.js.
enum Lift : uint8_t { kSnatch, kCleanJerk, kTotal, kBackSquat, kFrontSquat, kLiftCount };

struct Athlete {
  std::string id;
  std::string name;
  float kg[kLiftCount] = {};
  bool has[kLiftCount] = {};           // false: blank on the board
  std::string pr_date[kLiftCount];      // "YYYY-MM-DD" or ""
  bool visible = true;
};

enum class SlideType : uint8_t { kWhiteboard, kMedia, kInstagramStory, kInstagramPosts };

struct Slide {
  std::string id;
  SlideType type = SlideType::kWhiteboard;
  uint16_t duration_s = 8;  // 5..999; per page / per story where the slide expands
  bool enabled = true;
  std::string file;         // media slides: file name in /media
};

enum class SortMode : uint8_t { kBoard, kTotal, kName };

struct WallSettings {
  std::string title = "THE STRENGTH TANK";
  uint8_t brightness = 50;          // 5..100
  SortMode sort = SortMode::kBoard;
  uint8_t pr_highlight_days = 7;    // 0 = off
};

struct MediaInfo {
  std::string name;   // file name; the GIF is at kMediaDir + "/" + name
  float peak_a = -1;  // busier supply's current at 100% for the brightest frame; < 0 unknown
};

enum class IgKind : uint8_t { kImage, kVideo, kReel, kCarousel };

struct IgItem {
  std::string tile_path;   // LittleFS path of the tile: raw RGB565 little-endian, row-major
  std::string timestamp;   // ISO 8601 from the Graph API
  IgKind kind = IgKind::kImage;
  int32_t like_count = -1; // < 0 unknown
};

struct IgContent {
  bool enabled = false;
  std::string username;
  std::vector<IgItem> stories;  // tiles 54x96, oldest first
  std::vector<IgItem> posts;    // tiles 78x78, newest first, at most 2
};

// Everything the wall draws. Immutable once published: the network half builds a new
// Content after every successful save or Instagram fetch and publishes it.
struct Content {
  uint32_t version = 0;
  std::vector<Athlete> athletes;  // board order (sorting is the renderer's job)
  std::vector<Slide> slides;
  WallSettings settings;
  std::vector<MediaInfo> media;
  IgContent instagram;
};
using ContentPtr = std::shared_ptr<const Content>;

constexpr const char* kMediaDir = "/media";

// ---- implemented by the display half ---------------------------------------

// Call first in setup(): allocates the HUB75 DMA buffers in internal RAM before
// anything else can fragment it, then starts the render task on core 1.
void display_begin();

// Show a message on the wall (setup access point details, fatal errors). Two short lines.
void display_show_message(const char* line1, const char* line2);

// Thread safe. The render task picks the new content up at its next frame.
void content_publish(ContentPtr content);

enum class Control : uint8_t { kNext, kPrev, kPause, kResume };
void control_post(Control action);  // thread safe

struct RenderStatus {
  bool paused = false;
  std::string now_playing;   // e.g. "PR whiteboard, page 2 of 4"
  float refresh_hz = 0;      // panel refresh as computed by the HUB75 driver
  float limiter_scale = 1;   // last frame's power limiter factor (1 = not limiting)
  uint32_t frames = 0;
};
RenderStatus render_status();  // thread safe

// Flash erase/write stalls the cache and PSRAM. The network half brackets large writes
// (uploads, Instagram tiles) with these so the render task can hold the current frame.
void display_flash_write_begin();
void display_flash_write_end();

// ---- implemented by the network half ----------------------------------------

void net_begin();  // storage, Wi-Fi, web server, Instagram task; publishes initial content
void net_loop();   // called from loop()

bool wall_time_valid();          // false until NTP or the RTC has given a plausible time
std::string wall_local_date();   // "YYYY-MM-DD" in the wall's time zone, "" if time invalid
int64_t wall_unix_time();        // seconds since the epoch, 0 if time invalid

}  // namespace wall
