// Pixel parity: lib/render must draw exactly what web/layout.js, web/igslides.js,
// web/prslides.js, web/announceslides.js and web/power.js draw. The expected bytes come from test/parity/gen_golden.mjs, which runs
// the JS on the same inputs; regenerate them after changing anything in web/.
#include <ArduinoJson.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "announceslides.h"
#include "font5x7.h"
#include "igslides.h"
#include "layout.h"
#include "power.h"
#include "prslides.h"
#include "surface.h"

using namespace wall;
using namespace wall::render;

namespace {

JsonDocument manifest;
std::string fixtureDir;
std::map<std::string, std::vector<uint8_t>> tileCache;
int framesCompared = 0;

std::string readFile(const std::string& path, bool* ok) {
  std::ifstream f(path, std::ios::binary);
  *ok = static_cast<bool>(f);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool loadManifest() {
  // pio runs the test binary from the project directory; fall back to this file's location.
  std::string here = __FILE__;
  here = here.substr(0, here.find_last_of('/'));
  for (const std::string& dir : {std::string("test/parity/fixtures"), here + "/../parity/fixtures"}) {
    bool ok = false;
    const std::string text = readFile(dir + "/manifest.json", &ok);
    if (!ok) continue;
    fixtureDir = dir;
    return !deserializeJson(manifest, text);
  }
  return false;
}

double fromBits(const char* hex) {
  const uint64_t v = strtoull(hex, nullptr, 16);
  double d;
  memcpy(&d, &v, sizeof d);
  return d;
}

std::vector<uint8_t> loadFrame(const char* file) {
  bool ok = false;
  const std::string rle = readFile(fixtureDir + "/frames/" + file, &ok);
  std::vector<uint8_t> rgb;
  rgb.reserve(kFrameBytes);
  for (size_t i = 0; ok && i + 3 < rle.size(); i += 4) {
    for (int k = 0; k < static_cast<uint8_t>(rle[i]); k++) {
      rgb.push_back(static_cast<uint8_t>(rle[i + 1]));
      rgb.push_back(static_cast<uint8_t>(rle[i + 2]));
      rgb.push_back(static_cast<uint8_t>(rle[i + 3]));
    }
  }
  return rgb;
}

const uint8_t* loadTile(JsonVariantConst name) {
  if (name.isNull()) return nullptr;
  const std::string key = name.as<const char*>();
  auto it = tileCache.find(key);
  if (it == tileCache.end()) {
    bool ok = false;
    const std::string bytes = readFile(fixtureDir + "/tiles/" + key, &ok);
    it = tileCache.emplace(key, std::vector<uint8_t>(bytes.begin(), bytes.end())).first;
  }
  return it->second.data();
}

// Byte-for-byte; on failure names the case and the first pixel that differs.
void expectFrame(const std::string& label, const char* file, const uint8_t* got) {
  const std::vector<uint8_t> want = loadFrame(file);
  char msg[256];
  if (want.size() != static_cast<size_t>(kFrameBytes)) {
    snprintf(msg, sizeof msg, "%s: fixture %s is %zu bytes", label.c_str(), file, want.size());
    TEST_FAIL_MESSAGE(msg);
  }
  framesCompared++;
  int diffs = 0, first = -1;
  for (int i = 0; i < W * H; i++) {
    if (memcmp(&want[i * 3], got + i * 3, 3) != 0) {
      if (first < 0) first = i;
      diffs++;
    }
  }
  if (diffs) {
    const uint8_t* w = &want[first * 3];
    const uint8_t* g = got + first * 3;
    snprintf(msg, sizeof msg, "%s: %d pixels differ; first at (%d,%d) want %d,%d,%d got %d,%d,%d", label.c_str(),
             diffs, first % W, first / W, w[0], w[1], w[2], g[0], g[1], g[2]);
    TEST_FAIL_MESSAGE(msg);
  }
}

const char* const kLiftKeys[kLiftCount] = {"snatch", "clean_jerk", "total", "back_squat", "front_squat"};

std::vector<Athlete> athletesFrom(JsonArrayConst list) {
  std::vector<Athlete> out;
  for (JsonObjectConst j : list) {
    Athlete a;
    a.id = j["id"].as<const char*>();
    a.name = j["name"].as<const char*>();
    a.visible = j["visible"] | true;
    for (int k = 0; k < kLiftCount; k++) {
      JsonVariantConst v = j[kLiftKeys[k]];
      a.has[k] = !v.isNull();
      a.kg[k] = a.has[k] ? v.as<float>() : 0;
      JsonVariantConst d = j["pr_dates"][kLiftKeys[k]];
      a.pr_date[k] = d.isNull() ? "" : d.as<const char*>();
      JsonVariantConst p = j["pr_prev"][kLiftKeys[k]];
      a.has_prev[k] = p.is<float>();  // null and absent are both "unknown"
      a.prev_kg[k] = a.has_prev[k] ? p.as<float>() : 0;
    }
    out.push_back(a);
  }
  return out;
}

SortMode sortFrom(const char* s) {
  if (!strcmp(s, "total")) return SortMode::kTotal;
  if (!strcmp(s, "name")) return SortMode::kName;
  return SortMode::kBoard;
}

IgKind kindFrom(const char* s) {
  if (!strcmp(s, "video")) return IgKind::kVideo;
  if (!strcmp(s, "reel")) return IgKind::kReel;
  if (!strcmp(s, "carousel")) return IgKind::kCarousel;
  return IgKind::kImage;
}

std::unique_ptr<uint8_t[]> frameBuffer() { return std::unique_ptr<uint8_t[]>(new uint8_t[kFrameBytes]); }

}  // namespace

void setUp() {}
void tearDown() {}

void test_constants_and_font() {
  JsonObjectConst c = manifest["constants"];
  TEST_ASSERT_EQUAL(W, c["W"].as<int>());
  TEST_ASSERT_EQUAL(H, c["H"].as<int>());
  TEST_ASSERT_EQUAL(STORY_W, c["STORY_W"].as<int>());
  TEST_ASSERT_EQUAL(STORY_H, c["STORY_H"].as<int>());
  TEST_ASSERT_EQUAL(POST_SIZE, c["POST_SIZE"].as<int>());
  TEST_ASSERT_EQUAL(kMaxRows, c["MAX_ROWS"].as<int>());
  TEST_ASSERT_EQUAL(kMinSeconds, c["MIN_SECONDS"].as<int>());
  TEST_ASSERT_EQUAL(kMaxSeconds, c["MAX_SECONDS"].as<int>());
  TEST_ASSERT_EQUAL(MAX_PR_SCREENS, c["MAX_PR_SCREENS"].as<int>());
  TEST_ASSERT_EQUAL(MAX_ANNOUNCE_SCREENS, c["MAX_ANNOUNCE_SCREENS"].as<int>());
  TEST_ASSERT_TRUE(PANEL_FULL_WHITE_A == fromBits(c["PANEL_FULL_WHITE_A"]));
  TEST_ASSERT_TRUE(SUPPLY_RATED_A == fromBits(c["SUPPLY_RATED_A"]));
  TEST_ASSERT_TRUE(SUPPLY_BUDGET_A == fromBits(c["SUPPLY_BUDGET_A"]));
  JsonArrayConst font = manifest["font"];
  TEST_ASSERT_EQUAL(256 * 5, font.size());
  for (int i = 0; i < 256 * 5; i++) TEST_ASSERT_EQUAL_UINT8_MESSAGE(font[i].as<int>(), kFont5x7[i], "font byte");
}

void test_collation_matches_v8() {
  const std::string order = manifest["collation"].as<const char*>();
  for (size_t i = 0; i + 1 < order.size(); i++) {
    const JsString a(1, static_cast<char16_t>(order[i])), b(1, static_cast<char16_t>(order[i + 1]));
    char msg[64];
    snprintf(msg, sizeof msg, "'%c' sorts before '%c'", order[i], order[i + 1]);
    TEST_ASSERT_TRUE_MESSAGE(compareNames(a, b) < 0, msg);
  }
}

void test_small_functions() {
  for (JsonObjectConst k : manifest["formatKg"].as<JsonArrayConst>()) {
    TEST_ASSERT_EQUAL_STRING(k["s"].as<const char*>(), formatKg(fromBits(k["v"])).c_str());
  }
  for (JsonObjectConst p : manifest["parse"].as<JsonArrayConst>()) {
    int64_t ms = 0;
    const bool ok = parseIsoMs(p["s"].as<const char*>(), &ms);
    TEST_ASSERT_EQUAL_MESSAGE(!p["ms"].isNull(), ok, p["s"].as<const char*>());
    if (ok) TEST_ASSERT_TRUE_MESSAGE(ms == p["ms"].as<int64_t>(), p["s"].as<const char*>());
  }
  for (JsonObjectConst a : manifest["formatAge"].as<JsonArrayConst>()) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE(a["s"].as<const char*>(),
                                     formatAge(a["t"].as<const char*>(), a["now"].as<int64_t>()).c_str(),
                                     a["t"].as<const char*>());
  }
  for (JsonObjectConst t : manifest["textWidths"].as<JsonArrayConst>()) {
    TEST_ASSERT_EQUAL_MESSAGE(t["w"].as<int>(),
                              textWidth(std::string(t["s"].as<const char*>()), t["narrow"].as<bool>(), t["scale"].as<int>()),
                              t["s"].as<const char*>());
  }
}

void test_whiteboard_parity() {
  auto buf = frameBuffer();
  Surface s(buf.get());
  for (JsonObjectConst c : manifest["whiteboard"].as<JsonArrayConst>()) {
    const std::string name = c["name"].as<const char*>();
    const std::vector<Athlete> athletes = athletesFrom(c["athletes"]);
    const Fit fit = fitWhiteboard(whiteboardAthletes(athletes, sortFrom(c["sort"])));

    JsonObjectConst f = c["fit"];
    const std::string m = "whiteboard " + name;
    TEST_ASSERT_EQUAL_MESSAGE(f["pageCount"].as<int>(), fit.pageCount, (m + " pageCount").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["perPage"].as<int>(), fit.perPage, (m + " perPage").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["rowPitch"].as<int>(), fit.rowPitch, (m + " rowPitch").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["valW"].as<int>(), fit.valW, (m + " valW").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["numW"].as<int>(), fit.numW, (m + " numW").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["nameW"].as<int>(), fit.nameW, (m + " nameW").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["nameChars"].as<int>(), fit.nameChars, (m + " nameChars").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(f["count"].as<int>(), fit.count, (m + " count").c_str());
    JsonArrayConst trunc = f["truncated"];
    TEST_ASSERT_EQUAL_MESSAGE(trunc.size(), fit.truncated.size(), (m + " truncated").c_str());
    for (size_t i = 0; i < trunc.size(); i++) TEST_ASSERT_EQUAL_STRING(trunc[i].as<const char*>(), fit.truncated[i].c_str());
    JsonArrayConst pages = f["pages"];
    TEST_ASSERT_EQUAL(pages.size(), fit.pages.size());
    for (size_t p = 0; p < pages.size(); p++) {
      JsonArrayConst ids = pages[p];
      TEST_ASSERT_EQUAL_MESSAGE(ids.size(), fit.pages[p].size(), (m + " page size").c_str());
      for (size_t i = 0; i < ids.size(); i++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(ids[i].as<const char*>(), fit.pages[p][i]->id.c_str(), (m + " order").c_str());
      }
    }

    WhiteboardOptions opts;
    opts.title = c["title"].as<const char*>();
    opts.prDays = c["prDays"].as<int>();
    opts.today = c["today"].as<const char*>();
    JsonArrayConst frames = c["frames"];
    for (size_t p = 0; p < frames.size(); p++) {
      renderWhiteboardPage(s, fit, static_cast<int>(p), opts);
      expectFrame(m + " page " + std::to_string(p + 1), frames[p], buf.get());
    }
    renderWhiteboardPage(s, fit, fit.pageCount, opts);
    expectFrame(m + " past the last page", c["beyond"], buf.get());
  }
}

void test_story_parity() {
  auto buf = frameBuffer();
  Surface s(buf.get());
  for (JsonObjectConst c : manifest["stories"].as<JsonArrayConst>()) {
    StoryView view;
    const bool hasStory = !c["story"].isNull();
    if (hasStory) {
      view.tile = loadTile(c["story"]["tile"]);
      view.timestamp = c["story"]["timestamp"].as<const char*>();
    }
    renderStorySlide(s, c["username"].as<const char*>(), hasStory ? &view : nullptr, c["index"].as<int>(),
                     c["count"].as<int>(), fromBits(c["progress"]), c["now"].as<int64_t>());
    expectFrame(std::string("story ") + c["name"].as<const char*>(), c["frame"], buf.get());
  }
}

void test_posts_parity() {
  auto buf = frameBuffer();
  Surface s(buf.get());
  for (JsonObjectConst c : manifest["posts"].as<JsonArrayConst>()) {
    std::vector<PostView> posts;
    for (JsonObjectConst p : c["posts"].as<JsonArrayConst>()) {
      PostView v;
      v.tile = loadTile(p["tile"]);
      v.timestamp = p["timestamp"].as<const char*>();
      v.kind = kindFrom(p["kind"]);
      v.like_count = p["like_count"].isNull() ? -1 : p["like_count"].as<int32_t>();
      posts.push_back(v);
    }
    renderPostsSlide(s, c["username"].as<const char*>(), posts, c["now"].as<int64_t>());
    expectFrame(std::string("posts ") + c["name"].as<const char*>(), c["frame"], buf.get());
  }
}

void test_prs_parity() {
  auto buf = frameBuffer();
  Surface s(buf.get());
  int cases = 0;
  for (JsonObjectConst c : manifest["prs"].as<JsonArrayConst>()) {
    const std::string m = std::string("new PRs ") + c["name"].as<const char*>();
    const std::vector<Athlete> athletes = athletesFrom(c["athletes"]);
    const std::string today = c["today"].as<const char*>();
    const std::vector<PrEntry> list = recentPrs(athletes, c["days"].as<int>(), today);

    // Who is celebrated, in which order, with which lifts.
    JsonArrayConst want = c["list"];
    TEST_ASSERT_EQUAL_MESSAGE(want.size(), list.size(), (m + " list size").c_str());
    for (size_t i = 0; i < list.size(); i++) {
      JsonObjectConst w = want[i];
      TEST_ASSERT_EQUAL_STRING_MESSAGE(w["id"].as<const char*>(), list[i].athlete->id.c_str(), (m + " order").c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(w["newest"].as<const char*>(), list[i].newest.c_str(), (m + " newest").c_str());
      JsonArrayConst lifts = w["lifts"];
      TEST_ASSERT_EQUAL_MESSAGE(lifts.size(), list[i].lifts.size(), (m + " lift count").c_str());
      for (size_t k = 0; k < lifts.size(); k++) {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(lifts[k]["lift"].as<const char*>(), kLiftKeys[list[i].lifts[k].lift], (m + " lift order").c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(lifts[k]["date"].as<const char*>(), list[i].lifts[k].date.c_str(), (m + " lift date").c_str());
      }
    }

    for (JsonObjectConst f : c["frames"].as<JsonArrayConst>()) {
      const int entry = f["entry"].as<int>(), index = f["index"].as<int>(), count = f["count"].as<int>();
      renderPrSlide(s, list[entry], index, count, today);
      expectFrame(m + " entry " + std::to_string(entry) + " dot " + std::to_string(index) + "/" + std::to_string(count),
                  f["frame"], buf.get());
    }
    cases++;
  }
  TEST_ASSERT_TRUE_MESSAGE(cases >= 15, "the fixtures have the New PRs cases");
}

void test_announce_parity() {
  auto buf = frameBuffer();
  Surface s(buf.get());
  int cases = 0;
  for (JsonObjectConst c : manifest["announce"].as<JsonArrayConst>()) {
    const std::string m = std::string("announcement ") + c["name"].as<const char*>();
    const std::string text = c["text"].as<const char*>();
    const int index = c["index"].as<int>(), count = c["count"].as<int>();
    // The fit first, so a mismatch says whether the scale, the wrapping or the drawing is off.
    const AnnounceFit fit = fitAnnouncement(fromUtf8(text), count);
    TEST_ASSERT_EQUAL_MESSAGE(c["scale"].as<int>(), fit.scale, (m + " scale").c_str());
    TEST_ASSERT_EQUAL_MESSAGE(c["cut"].as<bool>(), fit.cut, (m + " cut").c_str());
    JsonArrayConst lines = c["lines"];
    TEST_ASSERT_EQUAL_MESSAGE(lines.size(), fit.lines.size(), (m + " line count").c_str());
    for (size_t i = 0; i < lines.size(); i++) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE(lines[i].as<const char*>(), toUtf8(fit.lines[i]).c_str(), (m + " line").c_str());
    }
    renderAnnouncement(s, text, index, count);
    expectFrame(m, c["frame"], buf.get());
    cases++;
  }
  TEST_ASSERT_TRUE_MESSAGE(cases >= 23, "the fixtures have the announcement cases");

  int lists = 0;
  for (JsonObjectConst c : manifest["announceActive"].as<JsonArrayConst>()) {
    Content content;
    for (JsonObjectConst j : c["list"].as<JsonArrayConst>()) {
      Announcement a;
      a.id = j["id"].as<const char*>();
      a.text = j["text"].as<const char*>();
      a.enabled = j["enabled"].as<bool>();
      a.start_date = j["start_date"].isNull() ? "" : j["start_date"].as<const char*>();
      a.end_date = j["end_date"].isNull() ? "" : j["end_date"].as<const char*>();
      content.announcements.push_back(a);
    }
    const std::string today = c["today"].as<const char*>();
    const std::vector<const Announcement*> got = announcementScreens(content, today);
    std::string want, have;
    for (JsonVariantConst id : c["ids"].as<JsonArrayConst>()) want += std::string(id.as<const char*>()) + " ";
    for (const Announcement* a : got) have += a->id + " ";
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), have.c_str(), ("showing on '" + today + "'").c_str());
    lists++;
  }
  TEST_ASSERT_EQUAL(12, lists);
}

void test_power_parity() {
  for (JsonObjectConst c : manifest["power"].as<JsonArrayConst>()) {
    const std::string name = std::string("power ") + c["name"].as<const char*>();
    const std::vector<uint8_t> rgb = loadFrame(c["frame"]);
    TEST_ASSERT_EQUAL(kFrameBytes, rgb.size());
    for (JsonObjectConst e : c["estimates"].as<JsonArrayConst>()) {
      const SupplyAmps amps = estimateSupplyAmps(rgb.data(), fromBits(e["pct"]));
      TEST_ASSERT_TRUE_MESSAGE(amps.a == fromBits(e["a"]), (name + " supply A").c_str());
      TEST_ASSERT_TRUE_MESSAGE(amps.b == fromBits(e["b"]), (name + " supply B").c_str());
    }
    for (JsonObjectConst l : c["limited"].as<JsonArrayConst>()) {
      TEST_ASSERT_EQUAL_MESSAGE(l["value"].as<int>(), limitedBrightness(rgb.data(), l["pct"].as<int>()),
                                (name + " limitedBrightness").c_str());
    }
    const SupplyAmps full = estimateSupplyAmps(rgb.data(), 100);
    const double peak = full.a > full.b ? full.a : full.b;
    TEST_ASSERT_TRUE_MESSAGE(peak == fromBits(c["peak"]), (name + " peak").c_str());
    for (JsonObjectConst sb : c["slide"].as<JsonArrayConst>()) {
      TEST_ASSERT_EQUAL_MESSAGE(sb["value"].as<int>(), slideBrightness(peak, sb["pct"].as<int>()),
                                (name + " slideBrightness").c_str());
    }
  }
  for (JsonObjectConst sb : manifest["slideOnly"].as<JsonArrayConst>()) {
    TEST_ASSERT_EQUAL_MESSAGE(sb["value"].as<int>(), slideBrightness(fromBits(sb["peak"]), sb["pct"].as<int>()),
                              "slideBrightness");
  }
}

// settings.power: one supply, other ratings, and the default written out.
void test_power_config_parity() {
  int checked = 0;
  for (JsonObjectConst cfgJson : manifest["powerConfigs"].as<JsonArrayConst>()) {
    PowerSupplies cfg;
    cfg.supplies = cfgJson["supplies"].as<uint8_t>();
    cfg.rated_a = fromBits(cfgJson["rated_a"]);
    cfg.budget_pct = cfgJson["budget_pct"].as<uint8_t>();
    const std::string tag = std::to_string(cfg.supplies) + "x" + std::to_string(cfg.rated_a) + "A@" +
                            std::to_string(cfg.budget_pct) + "% ";
    TEST_ASSERT_TRUE_MESSAGE(supplyBudgetA(cfg) == fromBits(cfgJson["budget"]), (tag + "budget").c_str());
    for (JsonObjectConst c : cfgJson["frames"].as<JsonArrayConst>()) {
      const std::string name = tag + c["name"].as<const char*>();
      const std::vector<uint8_t> rgb = loadFrame(c["frame"]);
      TEST_ASSERT_EQUAL(kFrameBytes, rgb.size());
      for (JsonObjectConst e : c["estimates"].as<JsonArrayConst>()) {
        const SupplyAmps amps = estimateSupplyAmps(rgb.data(), fromBits(e["pct"]), cfg);
        TEST_ASSERT_TRUE_MESSAGE(amps.a == fromBits(e["a"]), (name + " supply A").c_str());
        TEST_ASSERT_TRUE_MESSAGE(amps.b == fromBits(e["b"]), (name + " supply B").c_str());
      }
      for (JsonObjectConst l : c["limited"].as<JsonArrayConst>()) {
        TEST_ASSERT_EQUAL_MESSAGE(l["value"].as<int>(), limitedBrightness(rgb.data(), l["pct"].as<int>(), cfg),
                                  (name + " limitedBrightness").c_str());
      }
      const SupplyAmps full = estimateSupplyAmps(rgb.data(), 100);  // peak_a: the two-supply split, always
      const double peak = full.a > full.b ? full.a : full.b;
      for (JsonObjectConst sb : c["slide"].as<JsonArrayConst>()) {
        TEST_ASSERT_EQUAL_MESSAGE(sb["value"].as<int>(), slideBrightness(peak, sb["pct"].as<int>(), cfg),
                                  (name + " slideBrightness").c_str());
      }
      checked++;
    }
    for (JsonObjectConst sb : cfgJson["slideOnly"].as<JsonArrayConst>()) {
      TEST_ASSERT_EQUAL_MESSAGE(sb["value"].as<int>(), slideBrightness(fromBits(sb["peak"]), sb["pct"].as<int>(), cfg),
                                (tag + "slideBrightness").c_str());
    }
  }
  TEST_ASSERT_EQUAL(6 * 7, checked);
}

int main() {
  if (!loadManifest()) {
    printf("No parity fixtures: run `node test/parity/gen_golden.mjs` from firmware/ first.\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_constants_and_font);
  RUN_TEST(test_collation_matches_v8);
  RUN_TEST(test_small_functions);
  RUN_TEST(test_whiteboard_parity);
  RUN_TEST(test_story_parity);
  RUN_TEST(test_posts_parity);
  RUN_TEST(test_prs_parity);
  RUN_TEST(test_announce_parity);
  RUN_TEST(test_power_parity);
  RUN_TEST(test_power_config_parity);
  printf("parity: %d frames compared byte for byte\n", framesCompared);
  return UNITY_END();
}
