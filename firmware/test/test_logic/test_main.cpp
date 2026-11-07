// Host tests for the parts of lib/render with no JS counterpart to compare against:
// the panel chain map, the rotation, the firmware's no-clock rules and its own screens.
#include <string.h>
#include <unity.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "announceslides.h"
#include "igslides.h"
#include "jsport.h"
#include "layout.h"
#include "panel_map.h"
#include "power_gate.h"
#include "prslides.h"
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

Athlete lifter(const std::string& id, const std::string& name, Lift lift, float kg, const char* date) {
  Athlete a;
  a.id = id;
  a.name = name;
  a.has[lift] = true;
  a.kg[lift] = kg;
  a.pr_date[lift] = date;
  return a;
}

Content prContent() {
  Content c;
  c.settings.pr_highlight_days = 7;
  c.athletes = {
      lifter("a", "Ann", kSnatch, 80, "2026-11-03"),  lifter("b", "Bo", kTotal, 200, "2026-10-30"),  // 7 days: out
      lifter("c", "Cy", kCleanJerk, 100, "2026-11-06"), lifter("d", "Di", kSnatch, 70, "2026-11-06"),
      lifter("e", "Ed", kBackSquat, 150, "2026-11-05"),
  };
  c.athletes[3].visible = false;  // hidden: no screen
  Slide wb;
  wb.id = "wb";
  Slide prs;
  prs.id = "prs";
  prs.type = SlideType::kNewPrs;
  prs.duration_s = 6;
  Slide off = prs;
  off.id = "prs-off";
  off.enabled = false;
  Slide tooShort = prs;
  tooShort.id = "prs-short";
  tooShort.duration_s = 4;
  c.slides = {wb, prs, off, tooShort};
  return c;
}

void test_sequence_new_prs_one_screen_per_athlete() {
  Content c = prContent();
  const std::vector<PrEntry> prs = prScreens(c, "2026-11-06");
  const std::vector<Screen> seq = sequence(c, 2, prs);
  TEST_ASSERT_EQUAL(2 + 3, seq.size());
  const char* const ids[] = {"c", "e", "a"};  // newest PR first
  for (int i = 0; i < 3; i++) {
    const Screen& sc = seq[2 + i];
    TEST_ASSERT_TRUE(sc.kind == ScreenKind::kNewPrs);
    TEST_ASSERT_EQUAL(i, sc.index);
    TEST_ASSERT_EQUAL_STRING(ids[i], sc.athlete.c_str());
    TEST_ASSERT_EQUAL(6, sc.seconds);  // per athlete
    TEST_ASSERT_EQUAL_STRING("prs", sc.slide_id.c_str());
  }
  TEST_ASSERT_EQUAL_STRING("New PR: Cy, 1 of 3", seq[2].label.c_str());
  TEST_ASSERT_EQUAL_STRING("New PR: Ann, 3 of 3", seq[4].label.c_str());
  TEST_ASSERT_EQUAL(2, sequence(c, 2).size());  // no list given: nothing to show

  // Skipped: highlighting off, no clock, nobody in the window.
  c.settings.pr_highlight_days = 0;
  TEST_ASSERT_EQUAL(2, sequence(c, 2, prScreens(c, "2026-11-06")).size());
  c.settings.pr_highlight_days = 7;
  TEST_ASSERT_EQUAL(2, sequence(c, 2, prScreens(c, "")).size());
  TEST_ASSERT_EQUAL(2, sequence(c, 2, prScreens(c, "not a date")).size());
  TEST_ASSERT_EQUAL(2, sequence(c, 2, prScreens(c, "2026-12-25")).size());
  // A longer window takes in Bo.
  c.settings.pr_highlight_days = 8;
  TEST_ASSERT_EQUAL(2 + 4, sequence(c, 2, prScreens(c, "2026-11-06")).size());
}

void test_new_prs_capped_per_pass() {
  Content c = prContent();
  c.athletes.clear();
  for (int i = 0; i < 25; i++) {
    // Five days of PRs, five athletes a day; later athletes have older PRs.
    const std::string date = "2026-11-0" + std::to_string(6 - i / 5);
    c.athletes.push_back(lifter("x" + std::to_string(i), "lifter" + std::to_string(i), kTotal, 100 + i, date.c_str()));
  }
  TEST_ASSERT_EQUAL(25, recentPrs(c.athletes, 7, "2026-11-06").size());
  const std::vector<PrEntry> prs = prScreens(c, "2026-11-06");
  TEST_ASSERT_EQUAL(MAX_PR_SCREENS, prs.size());
  const std::vector<Screen> seq = sequence(c, 1, prs);
  TEST_ASSERT_EQUAL(1 + 20, seq.size());
  TEST_ASSERT_EQUAL_STRING("New PR: lifter0, 1 of 20", seq[1].label.c_str());
  TEST_ASSERT_EQUAL_STRING("New PR: lifter19, 20 of 20", seq[20].label.c_str());  // the oldest five wait
}

void test_new_prs_screen_follows_its_athlete() {
  Content c = prContent();
  const std::vector<PrEntry> beforePrs = prScreens(c, "2026-11-06");
  const std::vector<Screen> before = sequence(c, 2, beforePrs);
  // Ann (3rd) is on screen; then Bo sets a PR today and takes the top of the list.
  c.athletes[1].pr_date[kTotal] = "2026-11-06";
  const std::vector<PrEntry> afterPrs = prScreens(c, "2026-11-06");
  const std::vector<Screen> after = sequence(c, 2, afterPrs);
  TEST_ASSERT_EQUAL(2 + 4, after.size());
  TEST_ASSERT_EQUAL(5, findScreen(after, before[4]));  // still Ann, one place later
  TEST_ASSERT_EQUAL(3, after[5].index);
  // The day moves on and Ann's PR leaves the window: her screen is gone.
  const std::vector<PrEntry> laterPrs = prScreens(c, "2026-11-10");
  const std::vector<Screen> later = sequence(c, 2, laterPrs);
  TEST_ASSERT_EQUAL(-1, findScreen(later, before[4]));
  TEST_ASSERT_EQUAL(3, findScreen(later, before[2]));  // Cy (today) is still there
}

Announcement note(const std::string& id, const char* start = "", const char* end = "", bool enabled = true) {
  Announcement a;
  a.id = id;
  a.text = "Note " + id;
  a.start_date = start;
  a.end_date = end;
  a.enabled = enabled;
  return a;
}

Content announceContent() {
  Content c;
  c.announcements = {
      note("undated"),
      note("off", "", "", false),
      note("from_7th", "2026-11-07"),
      note("to_6th", "", "2026-11-06"),
      note("to_5th", "", "2026-11-05"),
      note("only_6th", "2026-11-06", "2026-11-06"),
      note("week", "2026-11-06", "2026-11-13"),
  };
  Slide wb;
  wb.id = "wb";
  Slide ann;
  ann.id = "ann";
  ann.type = SlideType::kAnnouncements;
  ann.duration_s = 7;
  Slide off = ann;
  off.id = "ann-off";
  off.enabled = false;
  Slide tooShort = ann;
  tooShort.id = "ann-short";
  tooShort.duration_s = 4;
  c.slides = {wb, ann, off, tooShort};
  return c;
}

std::string ids(const std::vector<Screen>& seq) {
  std::string out;
  for (const Screen& sc : seq) {
    if (sc.kind == ScreenKind::kAnnouncement) out += sc.announcement + " ";
  }
  return out;
}

void test_sequence_announcements_one_screen_per_showing() {
  Content c = announceContent();
  const std::vector<Screen> seq = sequence(c, 1, {}, announcementScreens(c, "2026-11-06"));
  TEST_ASSERT_EQUAL(1 + 4, seq.size());
  TEST_ASSERT_EQUAL_STRING("undated to_6th only_6th week ", ids(seq).c_str());  // list order
  for (int i = 0; i < 4; i++) {
    const Screen& sc = seq[1 + i];
    TEST_ASSERT_TRUE(sc.kind == ScreenKind::kAnnouncement);
    TEST_ASSERT_EQUAL(i, sc.index);
    TEST_ASSERT_EQUAL(7, sc.seconds);  // per announcement
    TEST_ASSERT_EQUAL_STRING("ann", sc.slide_id.c_str());
  }
  TEST_ASSERT_EQUAL_STRING("Announcement 1 of 4", seq[1].label.c_str());
  TEST_ASSERT_EQUAL_STRING("Announcement 4 of 4", seq[4].label.c_str());
  TEST_ASSERT_EQUAL(1, sequence(c, 1).size());  // no list given: nothing to show
}

void test_announcement_date_edges() {
  Content c = announceContent();
  const auto showing = [&](const char* today) { return ids(sequence(c, 1, {}, announcementScreens(c, today))); };
  // The day before: to_5th's last day; nothing that starts on the 6th yet.
  TEST_ASSERT_EQUAL_STRING("undated to_6th to_5th ", showing("2026-11-05").c_str());
  // The day after: to_6th and only_6th have ended; from_7th starts.
  TEST_ASSERT_EQUAL_STRING("undated from_7th week ", showing("2026-11-07").c_str());
  // The last day of a range is included, the day after is not.
  TEST_ASSERT_EQUAL_STRING("undated from_7th week ", showing("2026-11-13").c_str());
  TEST_ASSERT_EQUAL_STRING("undated from_7th ", showing("2026-11-14").c_str());
  // Across a year end.
  c.announcements.push_back(note("new_year", "2026-12-31", "2027-01-01"));
  TEST_ASSERT_EQUAL_STRING("undated from_7th new_year ", showing("2027-01-01").c_str());
  TEST_ASSERT_EQUAL_STRING("undated from_7th ", showing("2027-01-02").c_str());
  // No clock: undated ones still show, dated ones don't.
  TEST_ASSERT_EQUAL_STRING("undated ", showing("").c_str());
  TEST_ASSERT_EQUAL_STRING("undated ", showing("not a date").c_str());
  // Nothing showing: the slide is skipped.
  c.announcements[0].enabled = false;
  TEST_ASSERT_EQUAL(1, sequence(c, 1, {}, announcementScreens(c, "")).size());
}

void test_announcements_capped_and_followed() {
  Content c = announceContent();
  c.announcements.clear();
  for (int i = 0; i < 25; i++) c.announcements.push_back(note("n" + std::to_string(i)));
  std::vector<const Announcement*> anns = announcementScreens(c, "2026-11-06");
  TEST_ASSERT_EQUAL(MAX_ANNOUNCE_SCREENS, anns.size());
  std::vector<Screen> seq = sequence(c, 1, {}, anns);
  TEST_ASSERT_EQUAL(1 + 20, seq.size());
  TEST_ASSERT_EQUAL_STRING("Announcement 20 of 20", seq[20].label.c_str());
  TEST_ASSERT_EQUAL_STRING("n19", seq[20].announcement.c_str());  // the last five wait

  // The screen on the wall is the same announcement wherever it moves in the list.
  c = announceContent();
  const std::vector<Screen> before = sequence(c, 1, {}, announcementScreens(c, "2026-11-06"));
  TEST_ASSERT_EQUAL_STRING("only_6th", before[3].announcement.c_str());
  c.announcements.insert(c.announcements.begin(), note("new_first"));
  const std::vector<Screen> after = sequence(c, 1, {}, announcementScreens(c, "2026-11-06"));
  TEST_ASSERT_EQUAL(4, findScreen(after, before[3]));
  TEST_ASSERT_EQUAL(3, after[4].index);
  // Midnight: only_6th has ended, so its screen is gone; week is still there.
  const std::vector<Screen> later = sequence(c, 1, {}, announcementScreens(c, "2026-11-07"));
  TEST_ASSERT_EQUAL(-1, findScreen(later, before[3]));
  TEST_ASSERT_TRUE(findScreen(later, before[4]) > 0);
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

// ---- display power (display_set_power) ----------------------------------------------------------

namespace {

// The render loop's use of the gate, as display.cpp's tick() does it, with counters in
// place of the GIF decoder and the panel. One 8 s GIF screen at 100 ms a frame.
struct FakeWall {
  PowerGate gate;
  ScreenClock clock;
  uint32_t screenMs = 8000;
  int screens = 0, decoded = 0, pushed = 0, blanked = 0;

  void start(uint32_t now) {
    clock.startMs = now;
    clock.lastDrawMs = now;
    clock.nextFrameMs = now + 100;
    screens++;
  }
  void tick(bool wantOn, uint32_t now) {
    switch (gate.tick(wantOn, now, &clock)) {
      case PowerGate::Step::kBlank:
        blanked++;
        return;
      case PowerGate::Step::kIdle:
        return;
      case PowerGate::Step::kResume:
        pushed++;  // the held frame, at the held brightness
        break;
      case PowerGate::Step::kRun:
        break;
    }
    if (!clock.paused && clock.elapsedMs(now) >= screenMs) return start(now);
    if (static_cast<int32_t>(now - clock.nextFrameMs) >= 0) {
      decoded++;
      pushed++;
      clock.nextFrameMs += 100;
    }
  }
  void run(bool wantOn, uint32_t from, uint32_t to) {
    for (uint32_t t = from; t != to; t += 20) tick(wantOn, t);
  }
};

}  // namespace

void test_power_off_idles_and_resumes_where_it_was() {
  for (const uint32_t t0 : {1000u, 0xFFFFFFFFu - 5000u}) {  // and across the millis() wrap
    FakeWall w;
    w.start(t0);
    w.run(true, t0, t0 + 3000);
    TEST_ASSERT_EQUAL(1, w.screens);
    TEST_ASSERT_EQUAL(29, w.decoded);
    const int decoded = w.decoded, pushed = w.pushed;
    const uint32_t elapsed = w.clock.elapsedMs(t0 + 3000);

    // An hour off: black once, then nothing decoded or pushed, and the screen doesn't move on.
    w.run(false, t0 + 3000, t0 + 3000 + 3600000);
    TEST_ASSERT_EQUAL(1, w.blanked);
    TEST_ASSERT_EQUAL(decoded, w.decoded);
    TEST_ASSERT_EQUAL(pushed, w.pushed);
    TEST_ASSERT_EQUAL(1, w.screens);
    TEST_ASSERT_TRUE(w.gate.off());

    // Back on: the held frame is shown at once, then the frame that was due when it went
    // off, and the screen has the time it had left.
    const uint32_t back = t0 + 3000 + 3600000;
    w.tick(true, back);
    TEST_ASSERT_EQUAL(pushed + 2, w.pushed);
    TEST_ASSERT_EQUAL(decoded + 1, w.decoded);
    TEST_ASSERT_EQUAL(elapsed, w.clock.elapsedMs(back));
    TEST_ASSERT_FALSE(w.gate.off());
    w.run(true, back + 20, back + 4980);
    TEST_ASSERT_EQUAL(1, w.screens);  // 3 s + 4.98 s: still the first screen
    TEST_ASSERT_EQUAL(decoded + 50, w.decoded);  // frames carry on from where they were, none skipped or doubled
    w.run(true, back + 4980, back + 5020);
    TEST_ASSERT_EQUAL(2, w.screens);
  }
}

void test_power_off_while_paused_and_messages_while_off() {
  FakeWall w;
  w.start(0);
  w.run(true, 0, 2000);
  w.clock.paused = true;
  w.clock.pausedAtMs = 2000;
  w.run(false, 2000, 600000);
  w.run(true, 600000, 700000);
  TEST_ASSERT_EQUAL(1, w.screens);
  TEST_ASSERT_EQUAL(2000, w.clock.elapsedMs(700000));  // still frozen where the pause left it
  w.clock.paused = false;
  w.clock.startMs += 700000 - w.clock.pausedAtMs;  // the resume control, as display.cpp does it
  TEST_ASSERT_EQUAL(2000, w.clock.elapsedMs(700000));

  // Off, a setup message is drawn, then it goes: the panels are blanked again.
  PowerGate g;
  ScreenClock c;
  TEST_ASSERT_TRUE(g.tick(false, 10, &c) == PowerGate::Step::kBlank);
  TEST_ASSERT_TRUE(g.tick(false, 30, &c) == PowerGate::Step::kIdle);
  g.shown();
  TEST_ASSERT_TRUE(g.tick(false, 50, &c) == PowerGate::Step::kBlank);
  TEST_ASSERT_TRUE(g.tick(false, 70, &c) == PowerGate::Step::kIdle);
  TEST_ASSERT_TRUE(g.tick(true, 90, &c) == PowerGate::Step::kResume);
  TEST_ASSERT_TRUE(g.tick(true, 110, &c) == PowerGate::Step::kRun);
  // A waiting "retry later" keeps waiting for the time it had left.
  c.idleUntilMs = 1000;
  g.tick(false, 200, &c);
  g.tick(true, 5200, &c);
  TEST_ASSERT_EQUAL(6000, c.idleUntilMs);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_panel_map_is_a_bijection);
  RUN_TEST(test_panel_map_serpentine);
  RUN_TEST(test_sequence_expands_like_app_js);
  RUN_TEST(test_find_screen_across_publishes);
  RUN_TEST(test_sequence_new_prs_one_screen_per_athlete);
  RUN_TEST(test_new_prs_capped_per_pass);
  RUN_TEST(test_new_prs_screen_follows_its_athlete);
  RUN_TEST(test_sequence_announcements_one_screen_per_showing);
  RUN_TEST(test_announcement_date_edges);
  RUN_TEST(test_announcements_capped_and_followed);
  RUN_TEST(test_js_strings);
  RUN_TEST(test_no_clock_rules);
  RUN_TEST(test_message_is_centred);
  RUN_TEST(test_test_pattern_marks_every_panel);
  RUN_TEST(test_power_off_idles_and_resumes_where_it_was);
  RUN_TEST(test_power_off_while_paused_and_messages_while_off);
  return UNITY_END();
}
