// Announcements: a line-for-line port of web/announceslides.js, checked byte for byte by
// test/test_parity like the other slides. The console-only helpers (unshownChars,
// plainText) are not ported: the wall never needs them.
#pragma once

#include <string>
#include <vector>

#include "surface.h"
#include "wall_shared.h"

namespace wall {
namespace render {

// At most this many announcements get a screen per pass; it is also the dot row's length.
constexpr int MAX_ANNOUNCE_SCREENS = 20;

// Whether one announcement is on the wall today. today: the wall's local date
// "YYYY-MM-DD"; "" (or anything else) means no clock, and then only undated ones show.
bool announcementActive(const Announcement& a, const std::string& today);

// The announcements that get a screen in one pass, in list order, at most
// MAX_ANNOUNCE_SCREENS. The pointers point into c.announcements.
// Perf: O(n) over the list; n is capped at 50 by the API.
std::vector<const Announcement*> announcementScreens(const Content& c, const std::string& today);

// Word-wrap at spaces into lines of at most maxChars characters (code points); '\n'
// starts a new line and a word longer than a line is split.
// Perf: O(characters x lines); text is at most 200 characters.
std::vector<JsString> wrapLines(const JsString& text, int maxChars);

struct AnnounceFit {
  int scale = 1;
  std::vector<JsString> lines;
  bool cut = false;  // too long even at scale 1: the end is cut and the last line ends in '~'
  int top = 0;
  int areaH = 0;
};

// The largest scale (3, then 2, then 1) at which the whole text fits; see the JS.
// Perf: at most three wraps of 200 characters.
AnnounceFit fitAnnouncement(const JsString& text, int count);

// One announcement's screen. index/count: its place among those showing (the dots).
// Perf: clears and draws one 256x96 frame; O(pixels + characters).
void renderAnnouncement(Surface& s, const std::string& text, int index, int count);

}  // namespace render
}  // namespace wall
