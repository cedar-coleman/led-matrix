// The Instagram worker: one job at a time (refresh or account lookup), plus a scheduled
// refresh every refresh_minutes. Mirrors fetch_instagram / fetch_accounts in mock_server.py;
// the decisions (what to download, in which order, the messages) are netcore's.
//
// HTTPS uses ESP-IDF's certificate bundle (esp_crt_bundle_attach): the server is always
// verified. The Graph URL carries the access token, so it is never logged, and
// esp_http_client's own logging (which prints URLs on some errors) is silenced.
#include <Arduino.h>
#include <JPEGDEC.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <mutex>

#include "net_internal.h"
#include "wall_shared.h"

namespace wall {
namespace net {

namespace {

const char* const TAG = "instagram";

constexpr size_t kMaxGraphBytes = 256 * 1024;
constexpr size_t kMaxImageBytes = 4 * 1024 * 1024;
constexpr int kStoryW = 54, kStoryH = 96, kPostSize = 78;
constexpr uint32_t kJobRefresh = 1, kJobAccounts = 2;

TaskHandle_t task = nullptr;
std::mutex jobMu;
const char* busy = nullptr;
JsonDocument accounts(psram());  // null: never run, running, failed, or the token changed
std::string accountsError;        // "" = null

// A failure that ends the refresh; `message` is what last_error shows.
struct Failure {
  std::string message;
};

template <typename T>
struct PsramBuf {
  T* p = nullptr;
  explicit PsramBuf(size_t n) { p = static_cast<T*>(heap_caps_calloc(n, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); }
  ~PsramBuf() { heap_caps_free(p); }
  PsramBuf(const PsramBuf&) = delete;
  PsramBuf& operator=(const PsramBuf&) = delete;
};

struct Download {
  std::string body;  // > 4 KB: PSRAM
  size_t max = 0;
  bool overflow = false;
};

esp_err_t onEvent(esp_http_client_event_t* e) {
  auto* d = static_cast<Download*>(e->user_data);
  if (e->event_id == HTTP_EVENT_HEADERS_SENT) {
    d->body.clear();  // a new request (after a redirect): forget the previous body
  } else if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
    if (d->body.size() + e->data_len > d->max) {
      d->overflow = true;
    } else {
      d->body.append(static_cast<const char*>(e->data), e->data_len);
    }
  }
  return ESP_OK;
}

const char* networkReason(esp_err_t err) {
  switch (err) {
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_TIMEOUT:
      return "timed out";
    case ESP_ERR_HTTP_CONNECT:
      return "couldn't connect";
    default:
      return "network error";
  }
}

// GET with the certificate bundle. Returns the HTTP status; throws Failure for a network
// error. The URL is never part of the message.
int httpGet(const std::string& url, size_t max, Download* d) {
  if (!wifiConnected()) throw Failure{"Couldn't reach Instagram: the wall is not on Wi-Fi"};
  d->max = max;
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.timeout_ms = 10000;
  cfg.buffer_size = 4096;
  cfg.buffer_size_tx = 2048;  // the request line carries the token (up to 1024 characters)
  cfg.max_redirection_count = 5;
  cfg.event_handler = onEvent;
  cfg.user_data = d;
  cfg.user_agent = "gymboard";
  cfg.keep_alive_enable = false;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) throw Failure{"Couldn't reach Instagram: out of memory"};
  const esp_err_t err = esp_http_client_perform(c);
  const int status = esp_http_client_get_status_code(c);
  esp_http_client_cleanup(c);
  esp_task_wdt_reset();
  if (err != ESP_OK) throw Failure{std::string("Couldn't reach Instagram: ") + networkReason(err)};
  if (d->overflow) throw Failure{"Couldn't download from Instagram (the answer was too large)"};
  return status;
}

void graph(const std::string& path, const std::string& token, JsonDocument& out) {
  Download d;
  const int status = httpGet(netcore::graphUrl(path, token), kMaxGraphBytes, &d);
  if (status != 200) throw Failure{netcore::graphHttpError(status, d.body.data(), d.body.size())};
  if (deserializeJson(out, d.body)) throw Failure{"Instagram sent an answer the wall couldn't read"};
}

struct DecodeTarget {
  uint16_t* buf;
  int w, h;
};

int onDraw(JPEGDRAW* p) {
  auto* t = static_cast<DecodeTarget*>(p->pUser);
  const int cols = std::min(p->iWidth, t->w - p->x);
  for (int row = 0; row < p->iHeight && p->y + row < t->h; row++) {
    if (cols <= 0 || p->x < 0) break;
    memcpy(t->buf + (p->y + row) * t->w + p->x, p->pPixels + row * p->iWidth, cols * sizeof(uint16_t));
  }
  return 1;
}

// Download one image, decode, cover-crop, and write <dir>/<base>.565 and .bmp.
void makeTile(const std::string& url, const std::string& dir, const std::string& base, int tw, int th) {
  Download d;
  const int status = httpGet(url, kMaxImageBytes, &d);
  if (status != 200) throw Failure{"Instagram's image server answered HTTP " + std::to_string(status)};

  const Failure unreadable{"Couldn't read an image from Instagram"};
  std::unique_ptr<JPEGDEC> jpeg(new JPEGDEC());  // ~18 KB: malloc puts it in PSRAM
  if (!jpeg->openRAM(reinterpret_cast<uint8_t*>(&d.body[0]), static_cast<int>(d.body.size()), onDraw)) throw unreadable;
  const int w = jpeg->getWidth(), h = jpeg->getHeight();
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    jpeg->close();
    throw unreadable;
  }
  // JPEGDEC's DCT scaling does most of the shrinking for free; the crop finishes it.
  const int scale = netcore::jpegScaleFor(w, h, tw, th);
  const int option = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER : scale == 2 ? JPEG_SCALE_HALF : 0;
  DecodeTarget t{nullptr, (w + scale - 1) / scale, (h + scale - 1) / scale};
  PsramBuf<uint16_t> pixels(static_cast<size_t>(t.w) * t.h);
  if (!pixels.p) {
    jpeg->close();
    throw Failure{"Not enough memory to decode an Instagram picture"};
  }
  t.buf = pixels.p;
  jpeg->setPixelType(RGB565_LITTLE_ENDIAN);
  jpeg->setUserPointer(&t);
  const int ok = jpeg->decode(0, 0, option);
  jpeg->close();
  if (!ok) throw unreadable;  // includes progressive JPEGs JPEGDEC can't handle

  std::vector<uint8_t> rgb(static_cast<size_t>(tw) * th * 3);
  netcore::coverCrop565(t.buf, t.w, t.h, t.w, tw, th, rgb.data());
  const std::vector<uint8_t> raw = netcore::toRgb565(rgb.data(), tw, th);
  const std::vector<uint8_t> bmp = netcore::toBmp24(rgb.data(), tw, th);
  const netcore::Storage s = fsStorage();
  if (s.used + raw.size() + bmp.size() > s.usable()) throw Failure{"Not enough space on the board for the Instagram pictures"};
  if (!fsWriteFile(dir + "/" + base + ".565", raw.data(), raw.size()) ||
      !fsWriteFile(dir + "/" + base + ".bmp", bmp.data(), bmp.size())) {
    throw Failure{"Couldn't save the Instagram pictures on the board"};
  }
  esp_task_wdt_reset();
}

std::string newGeneration() {
  StoreLock l;
  long best = 0;
  for (const std::string& n : fsList("/ig", true)) {
    if (netcore::isGeneration(n)) best = std::max(best, atol(n.c_str()));
  }
  const std::string gen = std::to_string(best + 1);
  fsMkdir("/ig/" + gen);
  igSetBuilding(gen);
  return gen;
}

void refresh() {
  std::string token, account;
  {
    StoreLock l;
    token = igToken();
    account = igDoc()["account_id"] | "";
  }
  std::string gen;
  try {
    gen = newGeneration();
    const std::string dir = "/ig/" + gen;
    JsonDocument profile(psram()), storiesRaw(psram()), postsRaw(psram());
    graph(netcore::profilePath(account), token, profile);
    graph(netcore::storiesPath(account), token, storiesRaw);
    graph(netcore::postsPath(account), token, postsRaw);
    std::vector<netcore::IgDownload> stories, posts;
    if (!netcore::planStories(storiesRaw.as<JsonVariantConst>(), &stories) ||
        !netcore::planPosts(postsRaw.as<JsonVariantConst>(), &posts)) {
      throw Failure{"Instagram sent an answer the wall couldn't read"};
    }
    JsonDocument items(psram());
    JsonArray outStories = items["stories"].to<JsonArray>();
    JsonArray outPosts = items["posts"].to<JsonArray>();
    for (size_t i = 0; i < stories.size(); i++) {
      const std::string base = "story-" + std::to_string(i);
      makeTile(stories[i].url, dir, base, kStoryW, kStoryH);
      JsonObject o = outStories.add<JsonObject>();
      o["id"] = stories[i].id;
      o["timestamp"] = stories[i].timestamp;
      o["file"] = gen + "/" + base + ".bmp";
      o["kind"] = stories[i].kind;
    }
    for (size_t i = 0; i < posts.size(); i++) {
      const std::string base = "post-" + std::to_string(i);
      makeTile(posts[i].url, dir, base, kPostSize, kPostSize);
      JsonObject o = outPosts.add<JsonObject>();
      o["id"] = posts[i].id;
      o["timestamp"] = posts[i].timestamp;
      o["file"] = gen + "/" + base + ".bmp";
      o["kind"] = posts[i].kind;
      if (posts[i].hasLikes) {
        o["like_count"] = posts[i].likes;
      } else {
        o["like_count"] = nullptr;
      }
      if (posts[i].hasPermalink) {
        o["permalink"] = posts[i].permalink;
      } else {
        o["permalink"] = nullptr;
      }
    }

    // The switch: under the store lock, and only if nobody changed the settings meanwhile.
    StoreLock l;
    JsonDocument& ig = igDoc();
    if (igToken() != token || (ig["account_id"] | "") != account) {
      throw Failure{"The Instagram settings changed during the update; refresh again"};
    }
    if (profile["username"].is<const char*>()) ig["username"] = profile["username"];
    ig["stories"].set(items["stories"]);
    ig["posts"].set(items["posts"]);
    ig["sample"] = false;
    ig["last_error"] = nullptr;
    const std::string now = nowIso();
    if (now.empty()) {
      ig["last_fetch"] = nullptr;
    } else {
      ig["last_fetch"] = now;
    }
    igSetBuilding("");
    igCommit(true);  // from here on the wall draws the new generation
    igPruneGenerations(gen);
    ESP_LOGI(TAG, "refreshed: %u stories, %u posts", static_cast<unsigned>(stories.size()), static_cast<unsigned>(posts.size()));
  } catch (const Failure& f) {
    const std::string message = netcore::scrubMessage(f.message, token);
    {
      StoreLock l;
      igSetBuilding("");
      if (!gen.empty()) fsRemoveTree("/ig/" + gen);
      JsonDocument& ig = igDoc();
      ig["last_error"] = message;
      const std::string now = nowIso();
      if (now.empty()) {
        ig["last_fetch"] = nullptr;
      } else {
        ig["last_fetch"] = now;
      }
      igCommit(false);
    }
    noteError("Instagram: " + message);
  }
}

void lookupAccounts() {
  std::string token;
  {
    StoreLock l;
    token = igToken();
  }
  JsonDocument result(psram());
  std::string error;
  try {
    JsonDocument answer(psram());
    bool single = false;
    try {
      graph(netcore::accountsPath(), token, answer);
    } catch (const Failure&) {
      graph(netcore::mePath(), token, answer);
      single = true;
    }
    if (!netcore::parseAccounts(answer.as<JsonVariantConst>(), single, result.to<JsonArray>())) {
      throw Failure{"Instagram sent an answer the wall couldn't read"};
    }
  } catch (const Failure& f) {
    error = netcore::scrubMessage(f.message, token);
  }
  std::lock_guard<std::mutex> l(jobMu);
  if (error.empty()) {
    accounts = std::move(result);
    accountsError.clear();
  } else {
    accounts.clear();
    accountsError = error;
  }
}

bool scheduledDue(int64_t* lastRunMs) {
  StoreLock l;
  JsonDocument& ig = igDoc();
  const bool enabled = !ig["enabled"].is<bool>() || ig["enabled"].as<bool>();
  const char* account = ig["account_id"] | "";
  const long minutes = ig["refresh_minutes"] | 15L;
  // TLS needs the date to check certificates; without it every attempt fails anyway.
  if (!enabled || igToken().empty() || !*account || todayLocal().empty()) return false;
  const int64_t now = monotonicMs();
  if (*lastRunMs >= 0 && now - *lastRunMs < minutes * 60 * 1000) return false;
  *lastRunMs = now;
  return true;
}

void workerTask(void*) {
  esp_task_wdt_add(nullptr);
  int64_t lastRunMs = -1;
  for (;;) {
    uint32_t job = 0;
    xTaskNotifyWait(0, UINT32_MAX, &job, pdMS_TO_TICKS(5000));
    esp_task_wdt_reset();
    if (!job && scheduledDue(&lastRunMs)) {
      std::lock_guard<std::mutex> l(jobMu);
      if (!busy) {  // a scheduled refresh that finds a job running is skipped
        busy = "refresh";
        job = kJobRefresh;
      }
    }
    if (!job) continue;
    if (job == kJobRefresh) {
      lastRunMs = monotonicMs();
      refresh();
    } else {
      lookupAccounts();
    }
    {
      std::lock_guard<std::mutex> l(jobMu);
      busy = nullptr;
    }
    esp_task_wdt_reset();
  }
}

}  // namespace

void igBegin() {
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);  // prints the URL (and so the token) on some errors
  esp_log_level_set("esp_http_client", ESP_LOG_NONE);
  // Core 0 at low priority: TLS and JPEG decoding must never delay the render task or the API.
  xTaskCreatePinnedToCore(workerTask, "ig", 16 * 1024, nullptr, 3, &task, 0);
}

netcore::ApiError igStartJob(const char* kind) {
  std::lock_guard<std::mutex> l(jobMu);
  if (busy) {
    netcore::ApiError e = netcore::ApiError::make(409, "The wall is already talking to Instagram. Try again when it's done.", "busy");
    e.busy = busy;
    return e;
  }
  const bool refresh = !strcmp(kind, "refresh");
  busy = refresh ? "refresh" : "accounts";  // set before the 202 goes out
  if (!refresh) {
    accounts.clear();
    accountsError.clear();
  }
  xTaskNotify(task, refresh ? kJobRefresh : kJobAccounts, eSetValueWithOverwrite);
  return {};
}

const char* igBusy() {
  std::lock_guard<std::mutex> l(jobMu);
  return busy;
}

void igAccountsJson(JsonObject out) {
  std::lock_guard<std::mutex> l(jobMu);
  out["accounts"] = accounts.as<JsonVariantConst>();  // null unless a lookup finished
  if (accountsError.empty()) {
    out["accounts_error"] = nullptr;
  } else {
    out["accounts_error"] = accountsError;
  }
}

void igForgetAccounts() {
  std::lock_guard<std::mutex> l(jobMu);
  accounts.clear();
  accountsError.clear();
}

}  // namespace net
}  // namespace wall
