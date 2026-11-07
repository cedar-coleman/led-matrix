// The config store and every /api/ route except the upload streams (http_server.cpp) and
// the Instagram jobs themselves (instagram.cpp).
//
// The documents live in PSRAM and are the source of truth while running; each successful
// save writes its file atomically and then replaces the in-RAM copy, so a failed write
// leaves both the file and the API on the old version. Files are read back only at start.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <mutex>
#include <set>

#include "layout.h"
#include "net_internal.h"
#include "rotation.h"
#include "web_assets.h"
#include "wall_shared.h"

namespace wall {
namespace net {

using netcore::ApiError;

namespace {

const char* const TAG = "store";

constexpr int64_t kPinCacheMs = 5 * 60 * 1000;
constexpr const char* kFiles[] = {"settings", "athletes", "playlist", "announcements", "instagram", "media_index"};

SemaphoreHandle_t mu = nullptr;

JsonDocument settingsDoc(psram());
JsonDocument athletesDoc(psram());
JsonDocument playlistDoc(psram());
JsonDocument announcementsDoc(psram());
JsonDocument instagramDoc(psram());  // instagram.json minus the token (that's in NVS)
JsonDocument mediaIndexDoc(psram());
JsonDocument configErrors(psram());
std::string token;

netcore::PinGuard guard(monotonicMs, 32);

std::mutex errorsMutex;  // noteError comes from every task
netcore::ErrorRing errorRing(20);

std::string lastAction;
bool paused = false;

// The display schedule, parsed from settingsDoc on every save, and the wake/sleep override
// (RAM only, like `paused`: a restart goes back to the schedule).
netcore::Schedule schedule;
netcore::Override scheduleOverride;
bool displayOnSent = true;  // the display half starts on
uint32_t contentVersion = 0;

// What each label render_status() can report refers to. New PRs and announcement screens
// depend on the date, so the list is rebuilt on every publish and when the date changes.
struct Playing {
  std::string label, slideId, type, file;
};
std::vector<Playing> screens;
std::string screensDate;
ContentPtr screensContent;
std::mutex screensMutex;

// A recently verified PIN: SHA-256 of salt and PIN, so a PIN change (new salt) can
// never match an old entry.
uint8_t pinCacheKey[32];
int64_t pinCacheUntil = -1;

std::set<std::string> uploading;  // names with an upload in flight
uint64_t reservedBytes = 0;

std::string buildingGen;  // the generation the Instagram worker is filling

std::string path(const char* name) { return std::string("/") + name + ".json"; }

void defaultsFor(const std::string& name, JsonDocument& doc);

void rebuildMediaIndex(JsonDocument& doc) {
  JsonObject idx = doc.to<JsonObject>();
  std::vector<std::string> names = fsList("/media", false);
  std::sort(names.begin(), names.end());
  for (const std::string& n : names) {
    if (!netcore::isMediaName(n)) continue;
    JsonObject e = idx[n].to<JsonObject>();
    e["frames"] = nullptr;
    e["peak_a"] = nullptr;
    const int64_t m = fsMtime("/media/" + n);
    if (m >= buildUnix()) {
      e["uploaded"] = netcore::civilDate(m / 86400);
    } else {
      e["uploaded"] = nullptr;  // written while the clock wasn't set
    }
  }
}

void defaultsFor(const std::string& name, JsonDocument& doc) {
  doc.clear();
  if (name == "settings") {
    netcore::defaultSettings(doc.to<JsonObject>());
  } else if (name == "athletes") {
    doc["rev"] = 1;
    doc["athletes"].to<JsonArray>();
  } else if (name == "playlist") {
    doc["rev"] = 1;
    doc["slides"].to<JsonArray>();
  } else if (name == "announcements") {
    doc["rev"] = 1;
    doc["announcements"].to<JsonArray>();
  } else if (name == "instagram") {
    netcore::defaultInstagram(doc.to<JsonObject>());
    doc.remove("access_token");
  } else {
    rebuildMediaIndex(doc);
  }
}

// Read one config file with the recovery in API.md 13.3.
void load(const char* name, JsonDocument& doc) {
  const std::string p = path(name);
  std::string text;
  if (!fsExists(p)) {
    defaultsFor(name, doc);
    fsWriteJsonAtomic(p, doc.as<JsonVariantConst>());
    return;
  }
  if (fsReadAll(p, &text, 1024 * 1024) && netcore::parseConfigObject(text.data(), text.size(), doc)) return;
  const std::string bad = p + ".bad";
  fsRemove(bad);
  fsRename(p, bad);
  defaultsFor(name, doc);
  fsWriteJsonAtomic(p, doc.as<JsonVariantConst>());
  const std::string now = nowIso();
  JsonObject e = configErrors.add<JsonObject>();
  e["file"] = p.substr(1);
  e["renamed_to"] = bad.substr(1);
  if (now.empty()) {
    e["time"] = nullptr;
  } else {
    e["time"] = now;
  }
  noteError(p.substr(1) + " was damaged; saved as " + bad.substr(1) + " and reset to defaults");
}

void normalize() {
  netcore::fillSettingsDefaults(settingsDoc.as<JsonObject>());
  for (JsonDocument* d : {&athletesDoc, &playlistDoc, &announcementsDoc}) {
    (*d)["rev"] = netcore::revOf(d->as<JsonVariantConst>());
  }
  if (!athletesDoc["athletes"].is<JsonArray>()) athletesDoc["athletes"].to<JsonArray>();
  if (!playlistDoc["slides"].is<JsonArray>()) playlistDoc["slides"].to<JsonArray>();
  if (!announcementsDoc["announcements"].is<JsonArray>()) announcementsDoc["announcements"].to<JsonArray>();

  JsonDocument d;
  netcore::defaultInstagram(d.to<JsonObject>());
  for (JsonPairConst kv : d.as<JsonObjectConst>()) {
    if (!netcore::hasKey(instagramDoc.as<JsonObjectConst>(), kv.key().c_str())) instagramDoc[kv.key()] = kv.value();
  }
  // A file written by the mock (or an older firmware) may still hold the token: move it
  // to NVS, where it doesn't sit in a file anyone with flash access could read.
  if (instagramDoc["access_token"].is<const char*>()) {
    const std::string t = instagramDoc["access_token"].as<const char*>();
    if (!t.empty() && token.empty()) {
      Preferences p;
      if (p.begin("wall", false)) {
        p.putString("ig_token", t.c_str());
        p.end();
        token = t;
      }
    }
    instagramDoc.remove("access_token");
    fsWriteJsonAtomic(path("instagram"), instagramDoc.as<JsonVariantConst>());
  }
  instagramDoc["sample"] = false;
  if (token.empty()) {
    instagramDoc["stories"].to<JsonArray>();
    instagramDoc["posts"].to<JsonArray>();
  }
  timeSetZone(netcore::zoneIndex(settingsDoc["timezone"].as<std::string>()));
}

ApiError storageError() { return ApiError::make(500, "Couldn't save on the wall's storage. Try again.", "storage_error"); }

bool save(const char* name, const JsonDocument& doc) { return fsWriteJsonAtomic(path(name), doc.as<JsonVariantConst>()); }

std::vector<std::string> mediaFiles() {
  std::vector<std::string> out;
  for (const std::string& n : fsList("/media", false)) {
    if (netcore::isMediaName(n)) out.push_back(n);
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool verifyPin(const std::string& pin) {
  const std::string salt = settingsDoc["pin_salt"] | "";
  uint8_t key[32];
  netcore::Sha256 h;
  h.update(salt.data(), salt.size());
  h.update("\n", 1);
  h.update(pin.data(), pin.size());
  h.final(key);
  const int64_t now = monotonicMs();
  if (pinCacheUntil > now && netcore::constantTimeEqual(key, pinCacheKey, sizeof key)) {
    pinCacheUntil = now + kPinCacheMs;
    return true;
  }
  const int64_t t0 = monotonicMs();
  const bool ok = netcore::pinMatches(pin, salt, settingsDoc["pin_hash"] | "", settingsDoc["pin_iter"] | 0LL);
  ESP_LOGI(TAG, "PIN hash took %lld ms", static_cast<long long>(monotonicMs() - t0));
  if (ok) {
    memcpy(pinCacheKey, key, sizeof key);
    pinCacheUntil = now + kPinCacheMs;
  }
  return ok;
}

void randomBytes(uint8_t* p, size_t n) { esp_fill_random(p, n); }

void igPublic(JsonObject out) {
  for (JsonPairConst kv : instagramDoc.as<JsonObjectConst>()) out[kv.key()] = kv.value();
  out["token_set"] = !token.empty();
  const char* busy = igBusy();
  if (busy) {
    out["busy"] = busy;
  } else {
    out["busy"] = nullptr;
  }
  igAccountsJson(out);
}

void mediaList(JsonObject out) {
  JsonArray files = out["files"].to<JsonArray>();
  for (const std::string& n : mediaFiles()) {
    JsonObject f = files.add<JsonObject>();
    JsonVariantConst meta = mediaIndexDoc[n];
    f["name"] = n;
    f["size"] = fsSize("/media/" + n);
    f["frames"] = meta["frames"];
    f["peak_a"] = meta["peak_a"];
    f["uploaded"] = meta["uploaded"];
  }
  netcore::storageJson(fsStorage(), out["storage"].to<JsonObject>());
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "poweron";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "int_wdt";
    case ESP_RST_TASK_WDT:
      return "task_wdt";
    case ESP_RST_WDT:
      return "wdt";
    case ESP_RST_DEEPSLEEP:
      return "deepsleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}

int zone() { return netcore::zoneIndex(settingsDoc["timezone"].as<std::string>()); }

// Caller holds StoreLock.
netcore::ScheduleStatus scheduleEvaluate() {
  const netcore::ScheduleStatus st =
      netcore::evaluateSchedule(schedule, zone(), wall_time_valid(), wall_unix_time(), &scheduleOverride);
  if (st.displayOn != displayOnSent) {
    ESP_LOGI(TAG, "display %s (%s)", st.displayOn ? "on" : "off", netcore::scheduleStateName(st.state));
    displayOnSent = st.displayOn;
  }
  display_set_power(st.displayOn);
  return st;
}

// The same screen list the render task builds for this date, so /api/status can turn its
// label back into the slide it came from. Caller holds screensMutex.
void rebuildScreens(const std::string& today) {
  screensDate = today;
  screens.clear();
  if (!screensContent) return;
  const Content& content = *screensContent;
  const auto athletes = render::whiteboardAthletes(content.athletes, content.settings.sort);
  const auto prs = render::prScreens(content, today);
  const auto seq =
      render::sequence(content, render::fitWhiteboard(athletes).pageCount, prs, render::announcementScreens(content, today));
  for (const render::Screen& sc : seq) {
    Playing p;
    p.label = sc.label;
    p.slideId = sc.slide_id;
    p.file = sc.file;
    for (const Slide& s : content.slides) {
      if (s.id == sc.slide_id) p.type = netcore::slideTypeName(s.type);
    }
    screens.push_back(p);
  }
}

void status(JsonObject s) {
  const RenderStatus rs = render_status();
  s["mock"] = false;
  s["fw_version"] = kFwVersion;
  s["ui_version"] = kUiVersion;
  s["uptime_s"] = monotonicMs() / 1000;
  s["reset_reason"] = resetReason();
  s["paused"] = paused;
  if (lastAction.empty()) {
    s["last_action"] = nullptr;
  } else {
    s["last_action"] = lastAction;
  }
  netcore::scheduleStatusJson(scheduleEvaluate(), zone(), s);
  {
    std::lock_guard<std::mutex> l(screensMutex);
    // After midnight the render task has a new list; follow it so its labels still match.
    if (todayLocal() != screensDate) rebuildScreens(todayLocal());
    s["now_playing"] = nullptr;
    for (const Playing& p : screens) {
      if (p.label == rs.now_playing) {
        JsonObject np = s["now_playing"].to<JsonObject>();
        np["slide_id"] = p.slideId;
        np["type"] = p.type;
        if (p.file.empty()) {
          np["file"] = nullptr;
        } else {
          np["file"] = p.file;
        }
        break;
      }
    }
  }
  netcore::storageJson(fsStorage(), s["storage"].to<JsonObject>());
  wifiStatusJson(s["wifi"].to<JsonObject>());
  const std::string today = todayLocal();
  s["time_valid"] = !today.empty();
  if (today.empty()) {
    s["local_date"] = nullptr;
  } else {
    s["local_date"] = today;
  }
  s["timezone"] = settingsDoc["timezone"];
  s["min_free_heap"] = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  s["refresh_hz"] = rs.refresh_hz;
  s["limiter_scale"] = rs.limiter_scale;
  s["config_errors"] = configErrors.as<JsonArrayConst>();
  {
    std::lock_guard<std::mutex> l(errorsMutex);
    errorRing.toJson(s["recent_errors"].to<JsonArray>());
  }
  // Device-only, additive (API.md section 12).
  s["free_psram"] = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  otaStatusJson(s["firmware"].to<JsonObject>());
}

void controlPost(const std::string& a) {
  lastAction = a;
  if (a == "wake" || a == "sleep") {
    scheduleOverride = netcore::overrideFor(schedule, zone(), wall_time_valid(), wall_unix_time(), a == "wake");
    scheduleEvaluate();
    return;
  }
  if (a == "pause" || a == "resume") paused = a == "pause";
  control_post(a == "next" ? Control::kNext : a == "prev" ? Control::kPrev : a == "pause" ? Control::kPause : Control::kResume);
}

// ---- routes ---------------------------------------------------------------------------------------

struct Ctx {
  const HttpReq& req;
  HttpResp* resp;
  JsonDocument body{psram()};
  ApiError parse() {
    if (req.bodyTooLarge) return ApiError::make(413, "Too large (limit 256 KB)");
    if (req.bodyFormEncoded) return ApiError::make(400, "Send the body as JSON (Content-Type: application/json)");
    return netcore::parseJsonBody(req.body.data(), req.body.size(), body);
  }
  void ok(JsonVariantConst v, int status = 200) { resp->json(status, v); }
};

ApiError putAthletes(Ctx& c) {
  ApiError e = c.parse();
  if (e.ok()) e = netcore::requireObject(c.body.as<JsonVariantConst>(), "athletes");
  if (!e.ok()) return e;
  const long long rev = netcore::revOf(athletesDoc.as<JsonVariantConst>());
  if (!(e = netcore::checkRev(c.body.as<JsonVariantConst>(), rev)).ok()) return e;
  JsonDocument out(psram());
  out["rev"] = rev + 1;
  JsonArray list = out["athletes"].to<JsonArray>();
  if (!(e = netcore::validateAthletes(c.body["athletes"].as<JsonArrayConst>(), list)).ok()) return e;
  netcore::stampPrs(list, athletesDoc["athletes"].as<JsonArrayConst>(), todayLocal());
  if (!save("athletes", out)) return storageError();
  athletesDoc = std::move(out);
  publish();
  c.ok(athletesDoc.as<JsonVariantConst>());
  return {};
}

ApiError putPlaylist(Ctx& c) {
  ApiError e = c.parse();
  if (e.ok()) e = netcore::requireObject(c.body.as<JsonVariantConst>(), "slides");
  if (!e.ok()) return e;
  const long long rev = netcore::revOf(playlistDoc.as<JsonVariantConst>());
  if (!(e = netcore::checkRev(c.body.as<JsonVariantConst>(), rev)).ok()) return e;
  const std::vector<std::string> files = mediaFiles();
  const std::set<std::string> names(files.begin(), files.end());
  JsonDocument out(psram());
  out["rev"] = rev + 1;
  if (!(e = netcore::validatePlaylist(c.body["slides"].as<JsonArrayConst>(), names, out["slides"].to<JsonArray>())).ok()) {
    return e;
  }
  if (!save("playlist", out)) return storageError();
  playlistDoc = std::move(out);
  publish();
  c.ok(playlistDoc.as<JsonVariantConst>());
  return {};
}

ApiError putAnnouncements(Ctx& c) {
  ApiError e = c.parse();
  if (e.ok()) e = netcore::requireObject(c.body.as<JsonVariantConst>(), "announcements");
  if (!e.ok()) return e;
  const long long rev = netcore::revOf(announcementsDoc.as<JsonVariantConst>());
  if (!(e = netcore::checkRev(c.body.as<JsonVariantConst>(), rev)).ok()) return e;
  JsonDocument out(psram());
  out["rev"] = rev + 1;
  JsonArray list = out["announcements"].to<JsonArray>();
  if (!(e = netcore::validateAnnouncements(c.body["announcements"].as<JsonArrayConst>(), list)).ok()) return e;
  if (!save("announcements", out)) return storageError();
  announcementsDoc = std::move(out);
  publish();
  c.ok(announcementsDoc.as<JsonVariantConst>());
  return {};
}

ApiError putSettings(Ctx& c) {
  ApiError e = c.parse();
  if (e.ok()) e = netcore::requireObject(c.body.as<JsonVariantConst>());
  if (!e.ok()) return e;
  JsonDocument next(psram());
  next.set(settingsDoc.as<JsonVariantConst>());
  if (!(e = netcore::applySettings(c.body.as<JsonObjectConst>(), next.as<JsonObject>(), randomBytes)).ok()) return e;
  if (!save("settings", next)) return storageError();
  settingsDoc = std::move(next);
  timeSetZone(netcore::zoneIndex(settingsDoc["timezone"].as<std::string>()));
  if (netcore::hasKey(c.body.as<JsonObjectConst>(), "schedule")) {
    schedule = netcore::scheduleFromJson(settingsDoc["schedule"]);
    scheduleOverride = netcore::Override();  // a new schedule starts clean
  }
  scheduleEvaluate();  // the schedule or the zone may have changed
  publish();
  JsonDocument out;
  netcore::settingsPublic(settingsDoc.as<JsonObjectConst>(), out.to<JsonObject>());
  c.ok(out.as<JsonVariantConst>());
  return {};
}

bool saveToken(const std::string& t) {
  Preferences p;
  if (!p.begin("wall", false)) return false;
  const bool ok = t.empty() ? (p.remove("ig_token") || !p.isKey("ig_token")) : p.putString("ig_token", t.c_str()) == t.size();
  p.end();
  return ok;
}

ApiError putInstagram(Ctx& c) {
  ApiError e = c.parse();
  if (e.ok()) e = netcore::requireObject(c.body.as<JsonVariantConst>());
  if (!e.ok()) return e;
  JsonDocument next(psram());
  next.set(instagramDoc.as<JsonVariantConst>());
  next["access_token"] = token;
  bool touched = false;
  if (!(e = netcore::applyInstagram(c.body.as<JsonObjectConst>(), next.as<JsonObject>(), &touched)).ok()) return e;
  const std::string newToken = next["access_token"].as<std::string>();
  next.remove("access_token");
  if (newToken != token && !saveToken(newToken)) return storageError();
  if (!save("instagram", next)) return storageError();
  token = newToken;
  instagramDoc = std::move(next);
  if (touched) igForgetAccounts();
  if (touched && token.empty()) igPruneGenerations("");  // no token: no tiles (API.md section 12)
  publish();
  JsonDocument out;
  igPublic(out.to<JsonObject>());
  c.ok(out.as<JsonVariantConst>());
  return {};
}

ApiError igJob(Ctx& c, const char* kind) {
  const bool refresh = !strcmp(kind, "refresh");
  if (token.empty() || (refresh && !(instagramDoc["account_id"] | "")[0])) {
    return ApiError::make(400, refresh ? "Add an access token and an Instagram account ID first" : "Save an access token first",
                          "not_configured");
  }
  ApiError e = igStartJob(kind);
  if (!e.ok()) return e;
  JsonDocument out;
  out["queued"] = true;
  c.ok(out.as<JsonVariantConst>(), 202);
  return {};
}

ApiError deleteMedia(Ctx& c, const std::string& name) {
  if (!netcore::isMediaName(name) || !fsExists("/media/" + name)) return ApiError::make(404, "No such file");
  for (JsonVariantConst s : playlistDoc["slides"].as<JsonArrayConst>()) {
    if (s["file"] == name) return ApiError::make(409, name + " is in the rotation. Remove it there first.", "in_use");
  }
  JsonDocument next(psram());
  next.set(mediaIndexDoc.as<JsonVariantConst>());
  next.remove(name);
  if (!fsRemove("/media/" + name)) return storageError();
  save("media_index", next);  // a stale entry for a missing file is harmless: listings start from the files
  mediaIndexDoc = std::move(next);
  publish();
  JsonDocument out;
  out["ok"] = true;
  c.ok(out.as<JsonVariantConst>());
  return {};
}

ApiError route(Ctx& c) {
  const std::string& m = c.req.method;
  const std::string& p = c.req.path;
  const bool get = m == "GET" || m == "HEAD";
  JsonDocument out(psram());
  if (p == "/api/status" && get) {
    status(out.to<JsonObject>());
    c.ok(out.as<JsonVariantConst>());
    return {};
  }
  if (p == "/api/athletes") {
    if (get) return c.ok(athletesDoc.as<JsonVariantConst>()), ApiError();
    if (m == "PUT") return putAthletes(c);
  }
  if (p == "/api/playlist") {
    if (get) return c.ok(playlistDoc.as<JsonVariantConst>()), ApiError();
    if (m == "PUT") return putPlaylist(c);
  }
  if (p == "/api/announcements") {
    if (get) return c.ok(announcementsDoc.as<JsonVariantConst>()), ApiError();
    if (m == "PUT") return putAnnouncements(c);
  }
  if (p == "/api/settings") {
    if (get) {
      netcore::settingsPublic(settingsDoc.as<JsonObjectConst>(), out.to<JsonObject>());
      c.ok(out.as<JsonVariantConst>());
      return {};
    }
    if (m == "PUT") return putSettings(c);
  }
  if (p == "/api/instagram") {
    if (get) {
      igPublic(out.to<JsonObject>());
      c.ok(out.as<JsonVariantConst>());
      return {};
    }
    if (m == "PUT") return putInstagram(c);
  }
  if (p == "/api/instagram/refresh" && m == "POST") return igJob(c, "refresh");
  if (p == "/api/instagram/accounts" && m == "POST") return igJob(c, "accounts");
  if (p == "/api/control" && m == "POST") {
    ApiError e = c.parse();
    std::string action;
    if (e.ok()) e = netcore::parseControl(c.body.as<JsonVariantConst>(), &action);
    if (!e.ok()) return e;
    controlPost(action);
    out["ok"] = true;
    c.ok(out.as<JsonVariantConst>());
    return {};
  }
  if (p == "/api/media" && get) {
    mediaList(out.to<JsonObject>());
    c.ok(out.as<JsonVariantConst>());
    return {};
  }
  if (p.compare(0, 11, "/api/media/") == 0 && m == "DELETE") return deleteMedia(c, p.substr(11));
  if (p == "/api/wifi" && m == "PUT") {
    ApiError e = c.parse();
    if (e.ok()) e = wifiSave(c.body.as<JsonVariantConst>(), c.req.fromSetupAp);
    if (!e.ok()) return e;
    out["ok"] = true;
    out["restarting"] = true;
    c.ok(out.as<JsonVariantConst>());
    return {};
  }
  return ApiError::make(404, "No route for " + m + " " + p);
}

}  // namespace

StoreLock::StoreLock() { xSemaphoreTakeRecursive(mu, portMAX_DELAY); }
StoreLock::~StoreLock() { xSemaphoreGiveRecursive(mu); }

const std::string* HttpReq::param(const char* name) const {
  for (const auto& kv : query) {
    if (kv.first == name) return &kv.second;
  }
  return nullptr;
}

void HttpResp::json(int s, JsonVariantConst value) {
  status = s;
  body->clear();
  body->reserve(measureJson(value));
  serializeJson(value, *body);
}

void HttpResp::error(const ApiError& e) {
  JsonDocument d;
  e.toJson(d.to<JsonObject>());
  json(e.status, d.as<JsonVariantConst>());
  retryAfter = e.retryAfter;
}

void noteError(const std::string& message) {
  ESP_LOGW(TAG, "%s", message.c_str());
  std::lock_guard<std::mutex> l(errorsMutex);
  errorRing.add(nowIso(), message);
}

void storeBegin() {
  mu = xSemaphoreCreateRecursiveMutex();
  StoreLock l;
  configErrors.to<JsonArray>();
  {
    Preferences p;
    if (p.begin("wall", true)) {
      token = p.isKey("ig_token") ? p.getString("ig_token", "").c_str() : "";
      p.end();
    }
  }
  fsMkdir("/media");
  fsMkdir("/ig");
  fsCleanLeftovers();
  JsonDocument* docs[] = {&settingsDoc, &athletesDoc, &playlistDoc, &announcementsDoc, &instagramDoc, &mediaIndexDoc};
  for (size_t i = 0; i < 6; i++) load(kFiles[i], *docs[i]);
  normalize();
  schedule = netcore::scheduleFromJson(settingsDoc["schedule"]);
  igPruneGenerations(igGenerationOf(instagramDoc.as<JsonVariantConst>()));
  publish();
  scheduleEvaluate();
}

bool scheduleTick() {
  // net_loop must not wait behind a PIN hash in the api task; it asks again in a second.
  if (xSemaphoreTakeRecursive(mu, 0) != pdTRUE) return false;
  scheduleEvaluate();
  xSemaphoreGiveRecursive(mu);
  return true;
}

void storeClearPin() {
  StoreLock l;
  settingsDoc["pin_salt"] = nullptr;
  settingsDoc["pin_hash"] = nullptr;
  settingsDoc["pin_iter"] = nullptr;
  pinCacheUntil = -1;
  save("settings", settingsDoc);
}

void handleApi(const HttpReq& req, HttpResp* resp) {
  StoreLock l;
  Ctx c{req, resp};
  ApiError e;
  if (req.method != "GET" && req.method != "HEAD") {
    netcore::WriteRequest w;
    w.ip = req.ip;
    w.hasPinHeader = req.hasPin;
    w.pin = req.pin;
    w.fromSetupAp = req.fromSetupAp;
    w.deviceRules = true;
    // The first-PIN exception looks at the body, after the lockout (as the mock does).
    if (guard.waitSeconds(req.ip) == 0 && !netcore::pinSet(settingsDoc.as<JsonObjectConst>()) && req.method == "PUT" &&
        req.path == "/api/settings") {
      e = c.parse();
      w.firstPinRequest = e.ok() && netcore::isFirstPinBody(c.body.as<JsonVariantConst>());
    }
    if (e.ok()) e = netcore::authorizeWrite(guard, settingsDoc.as<JsonObjectConst>(), w, verifyPin);
  }
  if (e.ok()) e = route(c);
  if (!e.ok()) resp->error(e);
}

ApiError uploadAuthorize(const HttpReq& req, const UploadInfo& info, const ApiError& early) {
  StoreLock l;
  netcore::WriteRequest w;
  w.ip = req.ip;
  w.hasPinHeader = req.hasPin;
  w.pin = req.pin;
  w.fromSetupAp = req.fromSetupAp;
  w.deviceRules = true;
  ApiError e = netcore::authorizeWrite(guard, settingsDoc.as<JsonObjectConst>(), w, verifyPin);
  if (!e.ok()) return e;
  if (!early.ok()) return early;
  if (info.firmware) return {};
  const std::string& name = info.params.name;
  if (fsExists("/media/" + name) || uploading.count(name)) return ApiError::make(409, name + " already exists", "exists");
  const netcore::Storage s = fsStorage();
  if (s.used + reservedBytes + info.total > s.usable()) {
    return ApiError::make(507, "Not enough space on the board. Delete something first.", "no_space");
  }
  uploading.insert(name);
  reservedBytes += info.total;
  return {};
}

void uploadRelease(const UploadInfo& info) {
  if (info.firmware) return;
  StoreLock l;
  if (uploading.erase(info.params.name)) reservedBytes -= std::min<uint64_t>(reservedBytes, info.total);
}

void uploadMediaFinish(const UploadInfo& info, HttpResp* resp) {
  StoreLock l;
  const std::string& name = info.params.name;
  const std::string part = "/media/" + name + ".part", final = "/media/" + name;
  if (!fsRename(part, final)) {
    fsRemove(part);
    uploadRelease(info);
    resp->error(storageError());
    return;
  }
  JsonDocument next(psram());
  next.set(mediaIndexDoc.as<JsonVariantConst>());
  JsonObject e = next[name].to<JsonObject>();
  if (info.params.frames >= 0) {
    e["frames"] = info.params.frames;
  } else {
    e["frames"] = nullptr;
  }
  if (info.params.hasPeak) {
    e["peak_a"] = info.params.peak;
  } else {
    e["peak_a"] = nullptr;
  }
  const std::string today = todayLocal();
  if (today.empty()) {
    e["uploaded"] = nullptr;
  } else {
    e["uploaded"] = today;
  }
  save("media_index", next);  // the GIF is in place either way; a lost entry lists as nulls
  mediaIndexDoc = std::move(next);
  uploadRelease(info);
  publish();
  JsonDocument out;
  out["name"] = name;
  out["size"] = info.total;
  if (info.params.hasPeak) {
    out["peak_a"] = info.params.peak;
  } else {
    out["peak_a"] = nullptr;
  }
  resp->json(200, out.as<JsonVariantConst>());
}

std::string igToken() { return token; }
JsonDocument& igDoc() { return instagramDoc; }

void igCommit(bool publishContent) {
  StoreLock l;
  save("instagram", instagramDoc);
  if (publishContent) publish();
}

std::string igGenerationOf(JsonVariantConst ig) {
  for (const char* list : {"stories", "posts"}) {
    for (JsonVariantConst it : ig[list].as<JsonArrayConst>()) {
      std::string gen;
      if (netcore::parseTilePath(it["file"] | "", &gen, nullptr)) return gen;
    }
  }
  return "";
}

void igSetBuilding(const std::string& gen) {
  StoreLock l;
  buildingGen = gen;
}

void igPruneGenerations(const std::string& keep) {
  StoreLock l;
  for (const std::string& d : fsList("/ig", true)) {
    if (d != keep && d != buildingGen) fsRemoveTree("/ig/" + d);
  }
  for (const std::string& f : fsList("/ig", false)) fsRemove("/ig/" + f);  // stray files
}

void publish() {
  StoreLock l;
  netcore::ContentSources src;
  src.settings = settingsDoc.as<JsonObjectConst>();
  src.athletes = athletesDoc.as<JsonObjectConst>();
  src.playlist = playlistDoc.as<JsonObjectConst>();
  src.announcements = announcementsDoc.as<JsonObjectConst>();
  src.mediaIndex = mediaIndexDoc.as<JsonObjectConst>();
  src.mediaFiles = mediaFiles();
  src.instagram = instagramDoc.as<JsonObjectConst>();
  src.igTokenSet = !token.empty();
  auto content = std::make_shared<Content>(netcore::buildContent(src, ++contentVersion));

  {
    std::lock_guard<std::mutex> g(screensMutex);
    screensContent = content;
    rebuildScreens(todayLocal());
  }
  content_publish(std::move(content));
  setupMessageReshow();
}

}  // namespace net
}  // namespace wall
