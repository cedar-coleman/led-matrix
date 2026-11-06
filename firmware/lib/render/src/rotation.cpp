#include "rotation.h"

#include "layout.h"

namespace wall {
namespace render {

bool sameScreen(const Screen& a, const Screen& b) {
  return a.kind == b.kind && a.index == b.index && a.slide_id == b.slide_id && a.file == b.file;
}

int findScreen(const std::vector<Screen>& seq, const Screen& s) {
  for (size_t i = 0; i < seq.size(); i++) {
    if (sameScreen(seq[i], s)) return static_cast<int>(i);
  }
  return -1;
}

std::vector<Screen> sequence(const Content& c, int whiteboardPages) {
  std::vector<Screen> seq;
  const bool igActive = c.instagram.enabled;
  for (const Slide& s : c.slides) {
    if (!s.enabled || s.duration_s < kMinSeconds || s.duration_s > kMaxSeconds) continue;
    Screen base;
    base.seconds = s.duration_s;
    base.slide_id = s.id;
    if (s.type == SlideType::kWhiteboard) {
      for (int p = 0; p < whiteboardPages; p++) {
        Screen sc = base;
        sc.kind = ScreenKind::kWhiteboard;
        sc.index = p;
        sc.label = "PR whiteboard, page " + std::to_string(p + 1) + " of " + std::to_string(whiteboardPages);
        seq.push_back(sc);
      }
    } else if (s.type == SlideType::kInstagramStory) {
      if (!igActive) continue;
      const int n = static_cast<int>(c.instagram.stories.size());
      for (int i = 0; i < n; i++) {
        Screen sc = base;
        sc.kind = ScreenKind::kStory;
        sc.index = i;
        sc.label = "Instagram story " + std::to_string(i + 1) + " of " + std::to_string(n);
        seq.push_back(sc);
      }
    } else if (s.type == SlideType::kInstagramPosts) {
      if (igActive && !c.instagram.posts.empty()) {
        Screen sc = base;
        sc.kind = ScreenKind::kPosts;
        sc.label = "Instagram posts";
        seq.push_back(sc);
      }
    } else {
      Screen sc = base;
      sc.kind = ScreenKind::kMedia;
      sc.file = s.file;
      sc.label = s.file;
      seq.push_back(sc);
    }
  }
  return seq;
}

}  // namespace render
}  // namespace wall
