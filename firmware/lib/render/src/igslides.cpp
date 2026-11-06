#include "igslides.h"

#include <algorithm>

namespace wall {
namespace render {

namespace {

struct Colors {
  Rgb white{255, 255, 255};
  Rgb grey{140, 140, 140};
  Rgb dim{60, 60, 60};
  Rgb pink{255, 70, 150};  // Instagram magenta
  Rgb teal{0, 150, 150};
  Rgb badgeBg{0, 0, 0};
};
const Colors C;

const int POST_X[POST_COUNT] = {98, 178};
constexpr int POST_Y = 9;

int64_t floorDiv64(int64_t a, int64_t b) {
  const int64_t q = a / b;
  return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

std::string compact(int32_t n) {
  if (n < 1000) return std::to_string(n);
  if (n < 10000) {
    std::string s = toFixed1(n / 1000.0);
    if (s.size() >= 2 && s.compare(s.size() - 2, 2, ".0") == 0) s.resize(s.size() - 2);
    return s + "k";
  }
  return intString(jsRound(n / 1000.0)) + "k";
}

// 9x9 black box with a white play triangle.
void playBadge(Surface& s, int x, int y) {
  s.rect(x, y, x + 8, y + 8, C.badgeBg);
  s.rect(x + 3, y + 2, x + 3, y + 6, C.white);
  s.rect(x + 4, y + 3, x + 4, y + 5, C.white);
  s.rect(x + 5, y + 4, x + 5, y + 4, C.white);
}

// 9x9 black box with two overlapping squares.
void carouselBadge(Surface& s, int x, int y) {
  s.rect(x, y, x + 8, y + 8, C.badgeBg);
  auto box = [&](int bx, int by) {
    s.rect(bx, by, bx + 4, by, C.white);
    s.rect(bx, by + 4, bx + 4, by + 4, C.white);
    s.rect(bx, by, bx, by + 4, C.white);
    s.rect(bx + 4, by, bx + 4, by + 4, C.white);
  };
  box(x + 3, y + 1);
  box(x + 1, y + 3);
}

}  // namespace

std::string formatAge(const std::string& timestamp, int64_t nowUnix) {
  if (nowUnix <= 0) return "";
  int64_t t = 0;
  if (!parseIsoMs(timestamp, &t)) return "";
  const int64_t min = std::max<int64_t>(0, floorDiv64(nowUnix * 1000 - t, 60000));
  if (min < 1) return "just now";
  if (min < 60) return std::to_string(min) + " min ago";
  const int64_t h = min / 60;
  if (h < 24) return std::to_string(h) + " h ago";
  const int64_t d = h / 24;
  if (d < 14) return std::to_string(d) + " d ago";
  return std::to_string(d / 7) + " wk ago";
}

void renderStorySlide(Surface& s, const std::string& username, const StoryView* story, int index, int count,
                      double progress, int64_t nowUnix) {
  s.clear();
  if (story && story->tile) s.blit565(story->tile, STORY_W, STORY_H, 0, 0);
  s.rect(STORY_W, 0, STORY_W, H - 1, C.teal);

  const int left = STORY_W + 4;
  const JsString handle = u"@" + fromUtf8(username);
  const bool big = textWidth(handle, false, 2) <= W - left - 2;
  s.text(handle, left, 6, C.white, false, big ? 2 : 1);

  s.icon(left, 30);
  s.text(std::string("Story"), left + 13, 31, C.pink);
  const std::string age = story ? formatAge(story->timestamp, nowUnix) : "";
  if (!age.empty()) s.text(age, left + 13 + textWidth(std::string("Story")) + 8, 31, C.grey);
  if (count > 1) {
    const std::string label = std::to_string(index + 1) + "/" + std::to_string(count);
    s.text(label, W - 2 - textWidth(label), 31, C.grey);
  }
  s.text(std::string("Follow along on Instagram"), left, 56, C.grey);

  // Progress segments across the bottom.
  const int x0 = left;
  const int x1 = W - 3;
  const int gap = 2;
  const int seg = floorDiv(x1 - x0 + 1 - gap * (count - 1), std::max(1, count));
  for (int i = 0; i < count; i++) {
    const int sx = x0 + i * (seg + gap);
    s.rect(sx, 88, sx + seg - 1, 89, C.dim);
    const int fill = i < index ? seg
                     : i == index ? static_cast<int>(jsRound(seg * std::min(1.0, std::max(0.0, progress))))
                                  : 0;
    if (fill) s.rect(sx, 88, sx + fill - 1, 89, C.white);
  }
}

void renderPostsSlide(Surface& s, const std::string& username, const std::vector<PostView>& posts, int64_t nowUnix) {
  s.clear();

  // Left panel: icon, label, handle.
  s.icon(39, 8, 2);
  auto centre = [&](const JsString& str, int y, Rgb c) {
    s.text(str, std::max(1, floorDiv(96 - textWidth(str), 2)), y, c);
  };
  centre(u"Latest posts", 34, C.white);
  // 15 characters fit the 96 px panel; drop the @ before cutting anything off.
  const JsString user = fromUtf8(username);
  JsString handle = u"@" + user;
  if (textWidth(handle) > 94) handle = user;
  if (textWidth(handle) > 94) handle = jsSlice(handle, 0, 14) + u"~";
  centre(handle, 50, C.pink);
  centre(u"Follow us!", 70, C.grey);
  s.rect(96, 0, 96, H - 1, C.teal);

  const size_t shown = std::min<size_t>(posts.size(), POST_COUNT);
  for (size_t i = 0; i < shown; i++) {
    const PostView& p = posts[i];
    const int x = POST_X[i];
    if (p.tile) s.blit565(p.tile, POST_SIZE, POST_SIZE, x, POST_Y);
    if (p.kind == IgKind::kReel || p.kind == IgKind::kVideo) playBadge(s, x + POST_SIZE - 11, POST_Y + 2);
    if (p.kind == IgKind::kCarousel) carouselBadge(s, x + POST_SIZE - 11, POST_Y + 2);
    if (p.like_count >= 0) {
      const std::string likes = "\xE2\x99\xA5 " + compact(p.like_count);  // "♥ 1.2k"
      s.text(likes, x, 0, C.pink);
    }
    const std::string age = formatAge(p.timestamp, nowUnix);
    s.text(age, x + floorDiv(POST_SIZE - textWidth(age), 2), POST_Y + POST_SIZE + 1, C.grey);
  }
}

}  // namespace render
}  // namespace wall
