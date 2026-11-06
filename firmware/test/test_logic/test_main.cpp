// Host tests for the parts of lib/render with no JS counterpart to compare against:
// the panel chain map, the rotation, the firmware's no-clock rules and its own screens.
#include <string.h>
#include <unity.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "igslides.h"
#include "jsport.h"
#include "layout.h"
#include "panel_map.h"
#include "rotation.h"
#include "screens.h"

using namespace wall;
using namespace wall::render;

void setUp() {}
void tearDown() {}

void test_panel_map_is_a_bijection() {
  std::set<int> seen;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      const ChainXY c = chainXY(x, y);
      TEST_ASSERT_TRUE(c.x >= 0 && c.x < kPanelW * kChainLength);
      TEST_ASSERT_TRUE(c.y >= 0 && c.y < kPanelH);
      seen.insert(c.y * kPanelW * kChainLength + c.x);
    }
  }
  TEST_ASSERT_EQUAL(W * H, seen.size());
}

void test_panel_map_serpentine() {
  // Row 1: panel 1 at the top left, upright.
  TEST_ASSERT_EQUAL(0, chainXY(0, 0).x);
  TEST_ASSERT_EQUAL(0, chainXY(0, 0).y);
  TEST_ASSERT_EQUAL(3 * 64 + 63, chainXY(255, 31).x);
  TEST_ASSERT_EQUAL(31, chainXY(255, 31).y);
  // Row 2 runs right to left upside down: the wall's right end is panel 5, and its
  // top-right pixel is that panel's own bottom-left (x 0, y 31) because it is rotated.
  TEST_ASSERT_EQUAL(4 * 64 + 0, chainXY(255, 32).x);
  TEST_ASSERT_EQUAL(31, chainXY(255, 32).y);
  TEST_ASSERT_EQUAL(7 * 64 + 63, chainXY(0, 32).x);
  TEST_ASSERT_EQUAL(31, chainXY(0, 32).y);
  TEST_ASSERT_EQUAL(7 * 64 + 0, chainXY(63, 63).x);
  TEST_ASSERT_EQUAL(0, chainXY(63, 63).y);
  // Row 3: panel 9 at the bottom left, upright.
  TEST_ASSERT_EQUAL(8 * 64, chainXY(0, 64).x);
  TEST_ASSERT_EQUAL(0, chainXY(0, 64).y);
  TEST_ASSERT_EQUAL(11 * 64 + 63, chainXY(255, 95).x);
  // Chain numbers per grid cell, as drawn in design.md section 2.
  const int expected[3][4] = {{0, 1, 2, 3}, {7, 6, 5, 4}, {8, 9, 10, 11}};
  for (int r = 0; r < 3; r++) {
    for (int c = 0; c < 4; c++) TEST_ASSERT_EQUAL(expected[r][c], chainIndex(c, r));
  }
}

Content demoContent() {
  Content c;
  Slide wb;
  wb.id = "wb";
  wb.duration_s = 8;
  Slide story;
  story.id = "st";
  story.type = SlideType::kInstagramStory;
  story.duration_s = 6;
  Slide posts;
  posts.id = "po";
  posts.type = SlideType::kInstagramPosts;
  Slide media;
  media.id = "m1";
  media.type = SlideType::kMedia;
  media.file = "logo.gif";
  media.duration_s = 12;
  Slide off = media;
  off.id = "m2";
  off.enabled = false;
  Slide shortSlide = media;
  shortSlide.id = "m3";
  shortSlide.duration_s = 4;
  c.slides = {wb, story, posts, media, off, shortSlide};
  c.instagram.enabled = true;
  c.instagram.stories.resize(3);
  c.instagram.posts.resize(2);
  return c;
}

void test_sequence_expands_like_app_js() {
  Content c = demoContent();
  std::vector<Screen> seq = sequence(c, 4);
  TEST_ASSERT_EQUAL(4 + 3 + 1 + 1, seq.size());
  TEST_ASSERT_EQUAL_STRING("PR whiteboard, page 2 of 4", seq[1].label.c_str());
  TEST_ASSERT_EQUAL_STRING("Instagram story 3 of 3", seq[6].label.c_str());
  TEST_ASSERT_EQUAL(6, seq[6].seconds);
  TEST_ASSERT_TRUE(seq[7].kind == ScreenKind::kPosts);
  TEST_ASSERT_TRUE(seq[8].kind == ScreenKind::kMedia);
  TEST_ASSERT_EQUAL_STRING("logo.gif", seq[8].file.c_str());

  c.instagram.posts.clear();  // posts only when there are any
  TEST_ASSERT_EQUAL(4 + 3 + 1, sequence(c, 4).size());
  c.instagram.enabled = false;  // Instagram off skips both kinds
  TEST_ASSERT_EQUAL(4 + 1, sequence(c, 4).size());
}

void test_find_screen_across_publishes() {
  Content c = demoContent();
  const std::vector<Screen> before = sequence(c, 4);
  const std::vector<Screen> after = sequence(c, 2);  // athletes deleted: 2 pages left
  TEST_ASSERT_EQUAL(1, findScreen(after, before[1]));
  TEST_ASSERT_EQUAL(-1, findScreen(after, before[3]));  // page 4 is gone: swap now
  TEST_ASSERT_EQUAL(6, findScreen(after, before[8]));  // the media slide moved up
}

void test_js_strings() {
  const JsString s = fromUtf8("a\xF0\x9F\x92\xAA" "b");  // a, flexed biceps, b
  TEST_ASSERT_EQUAL(4, s.size());                        // UTF-16 units, like s.length
  int chars = 0;
  forEachChar(s, [&](char16_t, int) { chars++; });
  TEST_ASSERT_EQUAL(3, chars);
  TEST_ASSERT_EQUAL(2, jsSlice(s, 0, 2).size());  // cuts the pair in half, as JS does
  TEST_ASSERT_EQUAL(11, textWidth(jsSlice(s, 0, 2)));
  TEST_ASSERT_EQUAL(3, jsSlice(s, 0, -1).size());
  TEST_ASSERT_EQUAL_STRING("a\xF0\x9F\x92\xAA" "b", toUtf8(s).c_str());
  TEST_ASSERT_EQUAL(-2, floorDiv(-3, 2));
  TEST_ASSERT_EQUAL(1, floorDiv(3, 2));
  TEST_ASSERT_EQUAL_STRING("1.3", toFixed1(1.25).c_str());
  TEST_ASSERT_EQUAL_STRING("1.1", toFixed1(1.15).c_str());
  TEST_ASSERT_EQUAL(3, jsRound(2.5));
  TEST_ASSERT_EQUAL(0, jsRound(0.49999999999999994));
}

void test_no_clock_rules() {
  Athlete a;
  a.has[kSnatch] = true;
  a.kg[kSnatch] = 100;
  a.pr_date[kSnatch] = "2026-11-06";
  TEST_ASSERT_TRUE(isRecentPr(a, kSnatch, 7, "2026-11-06"));
  TEST_ASSERT_FALSE(isRecentPr(a, kSnatch, 7, ""));
  TEST_ASSERT_FALSE(isRecentPr(a, kSnatch, 0, "2026-11-06"));
  TEST_ASSERT_EQUAL_STRING("", formatAge("2026-11-06T19:00:00Z", 0).c_str());
  TEST_ASSERT_EQUAL_STRING("1 h ago", formatAge("2026-11-06T19:00:00Z", 1793995200).c_str());
}

int litPixels(const uint8_t* buf, int x0, int y0, int x1, int y1) {
  int n = 0;
  for (int y = y0; y <= y1; y++) {
    for (int x = x0; x <= x1; x++) {
      const uint8_t* p = buf + (y * W + x) * 3;
      n += (p[0] | p[1] | p[2]) != 0;
    }
  }
  return n;
}

void test_message_is_centred() {
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kFrameBytes]);
  Surface s(buf.get());
  renderMessage(s, "GymBoard-Setup", "http://192.168.4.1");
  // Lit pixels on both halves, nothing in the outer columns.
  TEST_ASSERT_TRUE(litPixels(buf.get(), 0, 0, 127, 95) > 100);
  TEST_ASSERT_TRUE(litPixels(buf.get(), 128, 0, 255, 95) > 100);
  TEST_ASSERT_EQUAL(0, litPixels(buf.get(), 0, 0, 2, 95));
  TEST_ASSERT_EQUAL(0, litPixels(buf.get(), 253, 0, 255, 95));
  // A line too long for double size still fits at normal size.
  renderMessage(s, std::string(80, 'W'), "");
  TEST_ASSERT_EQUAL(0, litPixels(buf.get(), 0, 0, 0, 95));
}

void test_test_pattern_marks_every_panel() {
  std::unique_ptr<uint8_t[]> buf(new uint8_t[kFrameBytes]);
  Surface s(buf.get());
  renderTestPattern(s);
  for (int r = 0; r < kPanelRows; r++) {
    for (int c = 0; c < kPanelCols; c++) {
      const int x0 = c * kPanelW, y0 = r * kPanelH;
      // Border corners lit, and the arrow's tip above its shaft.
      TEST_ASSERT_EQUAL(1, litPixels(buf.get(), x0, y0 + 16, x0, y0 + 16));
      TEST_ASSERT_EQUAL(1, litPixels(buf.get(), x0 + 63, y0 + 31, x0 + 63, y0 + 31));
      TEST_ASSERT_EQUAL(2, litPixels(buf.get(), x0 + 40, y0 + 5, x0 + 56, y0 + 5));
      TEST_ASSERT_EQUAL(10, litPixels(buf.get(), x0 + 40, y0 + 9, x0 + 56, y0 + 9));
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_panel_map_is_a_bijection);
  RUN_TEST(test_panel_map_serpentine);
  RUN_TEST(test_sequence_expands_like_app_js);
  RUN_TEST(test_find_screen_across_publishes);
  RUN_TEST(test_js_strings);
  RUN_TEST(test_no_clock_rules);
  RUN_TEST(test_message_is_centred);
  RUN_TEST(test_test_pattern_marks_every_panel);
  return UNITY_END();
}
