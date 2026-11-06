// The rotation as a list of screens: a port of sequence() in web/app.js, so the wall
// and the console's preview strip agree on what plays and in which order.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "wall_shared.h"

namespace wall {
namespace render {

enum class ScreenKind : uint8_t { kWhiteboard, kStory, kPosts, kMedia };

struct Screen {
  ScreenKind kind = ScreenKind::kWhiteboard;
  int index = 0;         // whiteboard page or story index
  uint16_t seconds = 8;  // per page / per story
  std::string file;      // media
  std::string slide_id;
  std::string label;     // "PR whiteboard, page 2 of 4", as the console labels it
};

// A screen is "the same screen" across content versions when the slide, kind, page or
// story and file all match. Used to keep the current screen through a publish.
bool sameScreen(const Screen& a, const Screen& b);

// -1 when the screen no longer exists in seq.
int findScreen(const std::vector<Screen>& seq, const Screen& s);

// Whiteboard slides become one screen per page, story slides one per current story,
// posts only when there are any, media one screen each. Disabled slides, slides with an
// out-of-range duration, and Instagram slides while Instagram is off are skipped.
std::vector<Screen> sequence(const Content& c, int whiteboardPages);

}  // namespace render
}  // namespace wall
