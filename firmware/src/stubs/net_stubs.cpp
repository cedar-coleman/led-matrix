// Stand-in for the network half, used by the display_dev environment only.
//
// Makes display_dev useful on the real wall with no Wi-Fi: it publishes the chalkboard
// athletes (athletes-from-chalkboard.csv, compiled in) on a whiteboard slide, plus one
// media slide for each GIF already in LittleFS /media, and a fixed clock so recent PRs
// show green. Serial commands step the rotation: n = next, p = previous, space = pause or
// resume, b = cycle brightness.
#include <Arduino.h>
#include <LittleFS.h>

#include <memory>

#include "wall_shared.h"

namespace wall {

namespace {

// 2026-11-06 20:00 UTC, the day the chalkboard was transcribed. Advances with uptime.
constexpr int64_t kFakeNow = 1793995200;
constexpr const char* kFakeToday = "2026-11-06";

struct Row {
  const char* name;
  float kg[kLiftCount];  // < 0: blank on the chalkboard
};

const Row kChalkboard[] = {
    {"rytownusa", {90, 112, 200, 172, 137}},
    {"barbie_lifter", {91, 106, 193, 145, 120}},
    {"Annjeanine C.", {64, 82, 140, 123, 103}},
    {"amousalli", {71, 85, 154, 136.5, 112}},
    {"lawrence_revsci", {97, 126, 221, 166, 143}},
    {"chrisajuan", {112, 140, 252, 235, 195}},
    {"jedthefred", {92, 114, 206, 182, 145}},
    {"the_peoples_master", {153, 175, 322, 250.5, 210}},
    {"cynthialbrown_", {75, 92, 166, 127, 109}},
    {"Brad B.", {95, 130, 225, 180, 155}},
    {"noah_c_m96", {110, 147, 257, 205, 170}},
    {"leashbond", {68, 86, 150, 118, 103}},
    {"colbierose", {82, 94, 176, 136.5, 109}},
    {"alli_garver", {61, 77, 138, -1, -1}},
    {"meng.wooo", {58, 75, 126, 120, 107}},
    {"dgcormier", {112, 138, 248, 185, 155}},
    {"tylermarsengill_", {128, 161, 288, 200, 180}},
    {"mabuhaykilos", {66, 85, 145, 125, 100}},
    {"kellygainz.sd", {50, 65, 103, 92, 77}},
    {"raynalifts", {51, 74, 121, 120, 101.5}},
    {"icedkoolaid", {82, 102, 182, 170, 120}},
    {"zules_xx", {64, 80, 141, 125, 100}},
    {"justin_ghost", {90, 124, 210, -1, -1}},
    {"ooohhhhpandy", {92, 114, 203, 202, 182}},
    {"aksel_koo", {67, 91, 158, 120, 95}},
    {"loungewithrob", {91, 115, 201, 150, 140}},
    {"jonjon5658", {105, 135, 240, 195, 166}},
    {"nairobatron", {93, 123, 210, 171, 160}},
    {"Josh S.", {122, 151, 267, 180, 155}},
    {"ishlie", {91, 114, 200, 152, 130}},
    {"donaldfitness", {115, 141, 253, 181, 147}},
};

// A few PRs inside the 7-day window, and one just outside it, so green shows up on
// several pages and the boundary can be checked by eye.
struct Pr {
  int row;
  Lift lift;
  const char* date;
};
const Pr kPrs[] = {
    {0, kSnatch, "2026-11-06"}, {5, kTotal, "2026-11-04"}, {7, kBackSquat, "2026-11-01"},
    {12, kCleanJerk, "2026-10-31"}, {19, kFrontSquat, "2026-11-05"}, {28, kSnatch, "2026-10-30"},  // 7 days: not green
};

std::shared_ptr<Content> demoContent() {
  auto c = std::make_shared<Content>();
  c->version = 1;
  int i = 0;
  for (const Row& r : kChalkboard) {
    Athlete a;
    a.id = "cb" + std::to_string(i++);
    a.name = r.name;
    for (int k = 0; k < kLiftCount; k++) {
      a.has[k] = r.kg[k] >= 0;
      a.kg[k] = a.has[k] ? r.kg[k] : 0;
    }
    c->athletes.push_back(a);
  }
  for (const Pr& p : kPrs) c->athletes[p.row].pr_date[p.lift] = p.date;

  Slide wb;
  wb.id = "whiteboard";
  wb.type = SlideType::kWhiteboard;
  wb.duration_s = 8;
  c->slides.push_back(wb);

  // Whatever GIFs are already on the board (uploaded by the full firmware earlier).
  File dir = LittleFS.open(kMediaDir);
  for (File f = dir ? dir.openNextFile() : File(); f && c->media.size() < 8; f = dir.openNextFile()) {
    const std::string name = f.name();
    if (f.isDirectory() || name.size() < 5 || name.compare(name.size() - 4, 4, ".gif") != 0) continue;
    MediaInfo m;
    m.name = name;
    c->media.push_back(m);  // peak_a unknown: the display estimates it from the first frame
    Slide s;
    s.id = "media-" + name;
    s.type = SlideType::kMedia;
    s.duration_s = 10;
    s.file = name;
    c->slides.push_back(s);
  }
  return c;
}

}  // namespace

void net_begin() {
  if (!LittleFS.begin(false)) Serial.println("LittleFS did not mount; showing the whiteboard only");
  content_publish(demoContent());
  Serial.println("display_dev: n = next, p = previous, space = pause/resume, b = brightness, s = status");
}

void net_loop() {
  static bool paused = false;
  static uint8_t brightness = 50;
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'n':
        control_post(Control::kNext);
        break;
      case 'p':
        control_post(Control::kPrev);
        break;
      case ' ':
        paused = !paused;
        control_post(paused ? Control::kPause : Control::kResume);
        break;
      case 'b': {
        brightness = brightness >= 100 ? 10 : brightness + 10;
        auto c = demoContent();
        c->version = brightness;
        c->settings.brightness = brightness;
        content_publish(c);
        Serial.printf("brightness %u%% from the next screen\n", brightness);
        break;
      }
      case 's': {
        const RenderStatus s = render_status();
        Serial.printf("%s%s | refresh %.0f Hz | limiter %.2f | %u frames\n", s.now_playing.c_str(),
                      s.paused ? " (paused)" : "", s.refresh_hz, s.limiter_scale, static_cast<unsigned>(s.frames));
        break;
      }
      default:
        break;
    }
  }
}

bool wall_time_valid() { return true; }
std::string wall_local_date() { return kFakeToday; }
int64_t wall_unix_time() { return kFakeNow + static_cast<int64_t>(millis() / 1000); }

}  // namespace wall
