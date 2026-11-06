// Pixel parity: lib/render must draw exactly what web/layout.js, web/igslides.js and
// web/power.js draw. The expected bytes come from test/parity/gen_golden.mjs, which runs
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

#include "font5x7.h"
#include "igslides.h"
#include "layout.h"
#include "power.h"
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
  RUN_TEST(test_power_parity);
  printf("parity: %d frames compared byte for byte\n", framesCompared);
  return UNITY_END();
}
