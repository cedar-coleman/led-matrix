// Instagram slides for the 256x96 wall: a line-for-line port of web/igslides.js.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "surface.h"
#include "wall_shared.h"

namespace wall {
namespace render {

constexpr int STORY_W = 54;
constexpr int STORY_H = 96;
constexpr int POST_SIZE = 78;
constexpr int MAX_STORIES = 5;
constexpr int POST_COUNT = 2;
constexpr int kStoryTileBytes = STORY_W * STORY_H * 2;
constexpr int kPostTileBytes = POST_SIZE * POST_SIZE * 2;

// "just now", "12 min ago", "3 h ago", "2 d ago", "5 wk ago".
// nowUnix: seconds since the epoch; 0 or less (no clock yet) gives "", so a wall that
// doesn't know the time shows no age rather than a wrong one.
std::string formatAge(const std::string& timestamp, int64_t nowUnix);

struct StoryView {
  const uint8_t* tile = nullptr;  // RGB565 LE, STORY_W x STORY_H; null draws black
  std::string timestamp;
};

// story: null when there is no story at this index (the JS passes null).
// progress: 0..1 through this story's screen time.
// Perf: clears and draws one 256x96 frame; O(pixels).
void renderStorySlide(Surface& s, const std::string& username, const StoryView* story, int index, int count,
                      double progress, int64_t nowUnix);

struct PostView {
  const uint8_t* tile = nullptr;  // RGB565 LE, POST_SIZE x POST_SIZE; null draws black
  std::string timestamp;
  IgKind kind = IgKind::kImage;
  int32_t like_count = -1;  // < 0 unknown: no like line
};

// Perf: clears and draws one 256x96 frame; O(pixels).
void renderPostsSlide(Surface& s, const std::string& username, const std::vector<PostView>& posts, int64_t nowUnix);

}  // namespace render
}  // namespace wall
