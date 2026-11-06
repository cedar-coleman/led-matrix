// HTTP: ESPAsyncWebServer on port 80, one catch-all handler, and the api task.
//
// Nothing slow runs on the async_tcp task. /api/ requests are paused (ESPAsyncWebServer's
// request continuation) and queued to the api task, which checks the PIN (PBKDF2, about
// 0.1 to 0.3 s), runs the route, writes flash and sends the response.
//
// Uploads (media and firmware) can't wait for the PIN before their body arrives, so the
// body streams while the api task decides: the first bytes are kept in PSRAM until the
// verdict, then flushed to the .part file (or OTA partition) and the rest streams straight
// through. A rejected upload is read and dropped; nothing from an unauthorized client
// reaches flash. With a recently verified PIN (store.cpp) the verdict takes milliseconds.
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <atomic>
#include <map>
#include <mutex>

#include "net_internal.h"
#include "wall_shared.h"
#include "web_assets.h"

namespace wall {
namespace net {

using netcore::ApiError;

extern const char kSetupPage[];

namespace {

const char* const TAG = "http";

constexpr size_t kStageMax = 1024 * 1024;  // PSRAM held for an upload awaiting its PIN verdict
constexpr size_t kGifHead = 13;
constexpr size_t kFirmwareHead = 256;      // image header + esp_app_desc_t
constexpr int kQueueDepth = 24;

AsyncWebServer server(80);
QueueHandle_t jobs = nullptr;
std::atomic<uint32_t> heartbeat{0};

enum Verdict : int { kPending = 0, kAccept = 1, kReject = 2 };

struct UploadCtx {
  UploadInfo info;
  HttpReq req;  // headers, client, query; no body
  ApiError early;
  size_t needHead = 0;
  std::vector<uint8_t> head;
  bool verifyPosted = false;

  std::atomic<int> verdict{kPending};
  ApiError verdictError;  // written before verdict is published

  std::mutex mu;  // everything below: the async_tcp task streams, the api task flushes/finishes
  std::vector<uint8_t> staged;
  File part;
  OtaSink* ota = nullptr;
  bool bracket = false;
  bool writeFailed = false;
  size_t received = 0;
  bool done = false;
};

struct ReqCtx {
  std::string body;
  bool tooLarge = false;
  std::shared_ptr<UploadCtx> up;
};

// Per-request state, touched only on the async_tcp task.
std::map<AsyncWebServerRequest*, std::shared_ptr<ReqCtx>> live;

enum class JobKind { kApi, kVerify, kFinish, kAbort };
struct Job {
  JobKind kind;
  AsyncWebServerRequestPtr req;
  HttpReq http;
  std::shared_ptr<UploadCtx> up;
};

const char* const kNoSuchFile = "{\"error\":\"No such file\",\"code\":\"not_found\"}";
const char* const kNoSuchTile = "{\"error\":\"No such tile\",\"code\":\"not_found\"}";
const char* const kBusy = "{\"error\":\"The wall is busy. Try again in a moment.\",\"code\":\"busy\"}";

// ---- responses ------------------------------------------------------------------------------------

void send(AsyncWebServerRequestPtr weak, const HttpResp& r) {
  auto req = weak.lock();
  if (!req) return;  // the client went away
  std::shared_ptr<std::string> body = r.body;
  AsyncWebServerResponse* resp =
      req->beginResponse("application/json", body->size(), [body](uint8_t* buf, size_t max, size_t index) -> size_t {
        const size_t n = std::min(max, body->size() - index);
        memcpy(buf, body->data() + index, n);
        return n;
      });
  resp->setCode(r.status);
  if (r.retryAfter >= 0) resp->addHeader("Retry-After", String(r.retryAfter));
  req->send(resp);
}

// ---- requests ----------------------------------------------------------------------------------------

std::string str(const String& s) { return std::string(s.c_str(), s.length()); }  // keeps a decoded %00

HttpReq basics(AsyncWebServerRequest* r) {
  HttpReq h;
  h.method = r->methodToString();
  h.path = str(r->url());
  for (size_t i = 0; i < r->params(); i++) {
    const AsyncWebParameter* p = r->getParam(i);
    if (p && !p->isPost() && !p->isFile()) h.query.emplace_back(str(p->name()), str(p->value()));
  }
  if (const AsyncWebHeader* pin = r->getHeader("X-Admin-Pin")) {
    h.hasPin = true;
    h.pin = str(pin->value());
  }
  AsyncClient* c = r->client();
  if (c) {
    h.ip = str(c->remoteIP().toString());
    h.fromSetupAp = wifiSetupMode() && c->localIP() == WiFi.softAPIP();
  }
  h.bodyFormEncoded = r->contentLength() > 0 && r->contentType().startsWith("application/x-www-form-urlencoded");
  return h;
}

bool post(Job* j) {
  if (xQueueSend(jobs, &j, 0) == pdTRUE) return true;
  delete j;
  return false;
}

bool isUpload(AsyncWebServerRequest* r) {
  return r->method() == AsyncWebRequestMethod::HTTP_POST && (r->url() == "/api/media" || r->url() == "/api/firmware");
}

void onGone(AsyncWebServerRequest* r) {
  auto it = live.find(r);
  if (it == live.end()) return;
  std::shared_ptr<UploadCtx> up = it->second->up;
  live.erase(it);
  if (up) post(new Job{JobKind::kAbort, {}, {}, up});  // closes and deletes anything half-written
}

// fresh: the first body chunk of a request. An entry left behind by a request that was
// destroyed without a disconnect callback must not leak into a new one at the same address.
std::shared_ptr<ReqCtx>& ctxFor(AsyncWebServerRequest* r, bool fresh) {
  auto it = live.find(r);
  if (it != live.end() && !fresh) return it->second;
  if (it != live.end() && it->second->up) post(new Job{JobKind::kAbort, {}, {}, it->second->up});
  r->onDisconnect([r] { onGone(r); });
  return live[r] = std::make_shared<ReqCtx>();
}

// ---- uploads -------------------------------------------------------------------------------------

bool sinkWrite(UploadCtx& u, const uint8_t* d, size_t n) {
  if (u.writeFailed || !n) return !u.writeFailed;
  const bool ok = u.info.firmware ? otaWrite(u.ota, d, n) : u.part.write(d, n) == n;
  if (!ok) u.writeFailed = true;
  return ok;
}

void flushStaged(UploadCtx& u) {
  if (u.staged.empty()) return;
  sinkWrite(u, u.staged.data(), u.staged.size());
  std::vector<uint8_t>().swap(u.staged);
}

void closeSink(UploadCtx& u, bool keep) {
  if (u.part) u.part.close();
  if (u.ota) {
    if (!keep) otaAbort(u.ota);
    if (!keep) u.ota = nullptr;
  }
  if (!keep && !u.info.firmware && u.verdict.load() == kAccept) fsRemove("/media/" + u.info.params.name + ".part");
  if (u.bracket) {
    display_flash_write_end();
    u.bracket = false;
  }
}

void startUpload(UploadCtx& u, AsyncWebServerRequest* r, size_t total) {
  u.req = basics(r);
  u.info.firmware = r->url() == "/api/firmware";
  u.info.total = total;
  if (u.info.firmware) {
    u.needHead = kFirmwareHead;
    if (total > otaMaxSize()) {
      u.early = ApiError::make(413, "Too large (limit " + std::to_string(otaMaxSize() / 1024) + " KB)");
    }
  } else {
    u.needHead = kGifHead;
    u.early = netcore::parseUploadQuery(u.req.param("name"), u.req.param("peak_a"), u.req.param("frames"), &u.info.params);
    if (u.early.ok() && total > netcore::kMaxUpload) u.early = ApiError::make(413, "Too large (limit 8192 KB)");
  }
}

void postVerify(const std::shared_ptr<UploadCtx>& u) {
  if (u->verifyPosted) return;
  if (u->early.ok()) {
    u->early = u->info.firmware ? otaCheckHeader(u->head.data(), u->head.size())
                                : netcore::checkGifHeader(u->head.data(), u->head.size());
  }
  u->verifyPosted = true;
  if (!post(new Job{JobKind::kVerify, {}, {}, u})) {
    u->verdictError = ApiError::make(503, "The wall is busy. Try again in a moment.", "busy");
    u->verdict.store(kReject);
  }
}

void uploadChunk(AsyncWebServerRequest* r, ReqCtx& c, const uint8_t* data, size_t len, size_t total) {
  if (!c.up) {
    c.up = std::make_shared<UploadCtx>();
    startUpload(*c.up, r, total);
  }
  UploadCtx& u = *c.up;
  if (u.head.size() < u.needHead) {
    const size_t take = std::min(u.needHead - u.head.size(), len);
    u.head.insert(u.head.end(), data, data + take);
  }
  if (!u.verifyPosted && (u.head.size() >= u.needHead || u.received + len >= total)) postVerify(c.up);

  // A verdict slower than the staging limit (it never should be): wait for it rather than
  // buffer without bound, but stay well under the task watchdog.
  for (int waited = 0; u.verdict.load() == kPending && u.staged.size() > kStageMax && waited < 3000; waited += 10) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  std::lock_guard<std::mutex> l(u.mu);
  u.received += len;
  if (u.done || !u.early.ok()) return;  // will be refused: read and drop
  const int v = u.verdict.load();
  if (v == kReject) return;
  if (v == kPending) {
    if (u.staged.size() > kStageMax) {
      u.writeFailed = true;  // answered as a failed write; nothing unverified reached flash
      return;
    }
    u.staged.insert(u.staged.end(), data, data + len);
    return;
  }
  flushStaged(u);
  sinkWrite(u, data, len);
}

// api task: the PIN and the store's checks, then open where the bytes will go.
void runVerify(UploadCtx& u) {
  ApiError e = uploadAuthorize(u.req, u.info, u.early);
  File part;
  OtaSink* ota = nullptr;
  bool bracket = false;
  if (e.ok()) {
    display_flash_write_begin();
    bracket = true;
    if (u.info.firmware) {
      ota = otaBegin(u.info.total, &e);
    } else {
      part = LittleFS.open(("/media/" + u.info.params.name + ".part").c_str(), "w");
      if (!part) e = ApiError::make(500, "Couldn't save on the wall's storage. Try again.", "storage_error");
    }
    if (!e.ok()) {
      display_flash_write_end();
      bracket = false;
      uploadRelease(u.info);
    }
  }
  std::lock_guard<std::mutex> l(u.mu);
  u.part = part;
  u.ota = ota;
  u.bracket = bracket;
  u.verdictError = e;
  u.verdict.store(e.ok() ? kAccept : kReject);
  if (u.done) {  // the client left while we were deciding
    closeSink(u, false);
    if (e.ok()) uploadRelease(u.info);
    return;
  }
  if (e.ok()) flushStaged(u);
}

void runFinish(AsyncWebServerRequestPtr req, UploadCtx& u) {
  HttpResp resp;
  {
    std::lock_guard<std::mutex> l(u.mu);
    if (u.done) return;
    u.done = true;
    if (u.verdict.load() != kAccept) {
      std::vector<uint8_t>().swap(u.staged);
      resp.error(u.verdictError);
    } else {
      flushStaged(u);
      if (u.writeFailed || u.received != u.info.total) {
        closeSink(u, false);
        uploadRelease(u.info);
        resp.error(u.info.firmware ? ApiError::make(500, "The update couldn't be written. Try again.", "storage_error")
                                   : ApiError::make(507, "Not enough space on the board. Delete something first.", "no_space"));
      } else if (u.info.firmware) {
        std::string version;
        OtaSink* s = u.ota;
        u.ota = nullptr;
        closeSink(u, true);
        const ApiError e = otaFinish(s, &version);
        if (!e.ok()) {
          resp.error(e);
        } else {
          JsonDocument out;
          out["ok"] = true;
          out["version"] = version;
          out["rebooting"] = true;
          resp.json(200, out.as<JsonVariantConst>());
          restartSoon(1500);
        }
      } else {
        closeSink(u, true);
        uploadMediaFinish(u.info, &resp);
      }
    }
  }
  send(req, resp);
}

void runAbort(UploadCtx& u) {
  std::lock_guard<std::mutex> l(u.mu);
  if (u.done) return;
  u.done = true;
  std::vector<uint8_t>().swap(u.staged);
  if (u.verdict.load() == kAccept) {
    closeSink(u, false);
    uploadRelease(u.info);
  }
}

void apiTask(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    Job* j = nullptr;
    if (xQueueReceive(jobs, &j, pdMS_TO_TICKS(1000)) == pdTRUE && j) {
      switch (j->kind) {
        case JobKind::kApi: {
          HttpResp resp;
          handleApi(j->http, &resp);
          send(j->req, resp);
          break;
        }
        case JobKind::kVerify:
          runVerify(*j->up);
          break;
        case JobKind::kFinish:
          runFinish(j->req, *j->up);
          break;
        case JobKind::kAbort:
          runAbort(*j->up);
          break;
      }
      delete j;
    }
    heartbeat.fetch_add(1);
    esp_task_wdt_reset();
  }
}

// ---- static files ---------------------------------------------------------------------------------------

void serveStatic(AsyncWebServerRequest* r, const std::string& path) {
  if (path.compare(0, 7, "/media/") == 0) {
    const std::string name = path.substr(7);
    if (!netcore::isMediaName(name) || !LittleFS.exists(("/media/" + name).c_str())) {
      r->send(404, "application/json", kNoSuchFile);
      return;
    }
    r->send(LittleFS, ("/media/" + name).c_str(), "image/gif");
    return;
  }
  if (path.compare(0, 4, "/ig/") == 0) {
    std::string gen, file;
    // The pattern admits only digits, one slash and a fixed name shape, so the path
    // resolves directly inside ig/<gen>/: no "..", no encoded slash, no NUL.
    if (!netcore::parseTilePath(path.substr(4), &gen, &file) || !LittleFS.exists(("/ig/" + gen + "/" + file).c_str())) {
      r->send(404, "application/json", kNoSuchTile);
      return;
    }
    r->send(LittleFS, ("/ig/" + gen + "/" + file).c_str(), "image/bmp");
    return;
  }
  if (path == "/setup") {
    r->send(200, "text/html; charset=utf-8", kSetupPage);
    return;
  }
  const std::string want = path == "/" ? "/index.html" : path;
  for (size_t i = 0; i < kWebAssetCount; i++) {
    const WebAsset& a = kWebAssets[i];
    if (want != a.path) continue;
    const AsyncWebHeader* inm = r->getHeader("If-None-Match");
    AsyncWebServerResponse* resp =
        inm && inm->value() == kWebEtag ? r->beginResponse(304) : r->beginResponse(200, a.type, a.gz, a.size);
    if (resp->code() == 200) resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("ETag", kWebEtag);
    r->send(resp);
    return;
  }
  r->send(404, "application/json", kNoSuchFile);
}

class Handler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest*) const override { return true; }
  bool isRequestHandlerTrivial() const override { return false; }

  void handleBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) override {
    std::shared_ptr<ReqCtx> c = ctxFor(r, index == 0);
    if (isUpload(r)) {
      uploadChunk(r, *c, data, len, total);
      return;
    }
    if (c->tooLarge || c->body.size() + len > netcore::kMaxJsonBody || total > netcore::kMaxJsonBody) {
      c->tooLarge = true;
      std::string().swap(c->body);
      return;
    }
    if (c->body.empty() && total) c->body.reserve(total);  // > 4 KB lands in PSRAM
    c->body.append(reinterpret_cast<const char*>(data), len);
  }

  void handleRequest(AsyncWebServerRequest* r) override {
    if (r->method() == AsyncWebRequestMethod::HTTP_OPTIONS) {
      // A CORS preflight gets a plain refusal, so browsers block cross-origin writes.
      r->send(405, "application/json", "{\"error\":\"Method not allowed\",\"code\":\"method_not_allowed\"}");
      return;
    }
    const std::string path = str(r->url());
    const bool get = r->method() == AsyncWebRequestMethod::HTTP_GET || r->method() == AsyncWebRequestMethod::HTTP_HEAD;
    if (isUpload(r)) {
      std::shared_ptr<ReqCtx> c = ctxFor(r, r->contentLength() == 0);
      if (!c->up) uploadChunk(r, *c, nullptr, 0, 0);  // no body at all
      postVerify(c->up);
      AsyncWebServerRequestPtr weak = r->pause();
      if (!post(new Job{JobKind::kFinish, weak, {}, c->up})) r->send(503, "application/json", kBusy);
      return;
    }
    if (path.compare(0, 5, "/api/") == 0 || !get) {
      HttpReq h = basics(r);
      auto it = live.find(r);
      if (it != live.end() && r->contentLength() > 0) {
        h.body = std::move(it->second->body);
        h.bodyTooLarge = it->second->tooLarge;
      }
      AsyncWebServerRequestPtr weak = r->pause();
      if (!post(new Job{JobKind::kApi, weak, std::move(h), nullptr})) r->send(503, "application/json", kBusy);
      return;
    }
    serveStatic(r, path);
  }
};

}  // namespace

uint32_t apiHeartbeat() { return heartbeat.load(); }

void serverBegin() {
  jobs = xQueueCreate(kQueueDepth, sizeof(Job*));
  // Core 0, below async_tcp (10) so request parsing stays responsive while a PIN hashes.
  xTaskCreatePinnedToCore(apiTask, "api", 12 * 1024, nullptr, 5, nullptr, 0);
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-store");
  server.addHandler(new Handler());
  server.begin();
  ESP_LOGI(TAG, "listening on port 80");
}

}  // namespace net
}  // namespace wall
