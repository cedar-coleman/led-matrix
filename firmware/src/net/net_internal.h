// Shared between the files of the network half (src/net/). Nothing outside src/net
// includes this; the display half sees only include/wall_shared.h.
//
// Tasks (all on core 0; the render task owns core 1):
//   async_tcp (prio 10, AsyncTCP)  parses HTTP, serves files, streams upload bodies.
//                                  Never hashes a PIN and never touches the store.
//   api       (prio 5)             every /api/ request: PIN checks (PBKDF2), validation,
//                                  config writes, content_publish. Sends the response.
//   ig        (prio 3)             Instagram refresh and account lookups (HTTPS, JPEG).
//   loopTask  (Arduino, core 1)    net_loop(): Wi-Fi backoff, time, OTA health, restarts.
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "netcore.h"

namespace wall {
namespace net {

constexpr const char* kFwVersion = "1.0.0";

// ---- memory -----------------------------------------------------------------------------------

// ArduinoJson allocates in small pools that malloc() would put in internal RAM; the
// config documents (up to 500 athletes) belong in PSRAM.
ArduinoJson::Allocator* psram();

// ---- time (timekeep.cpp) ------------------------------------------------------------------------

void timeBegin();              // RTC read, SNTP started once Wi-Fi is up
void timeLoop();               // writes the RTC after an NTP sync
void timeSetZone(int zone);    // netcore::kZones index
std::string nowIso();          // "" while the clock isn't valid
std::string todayLocal();      // "" while the clock isn't valid
int64_t monotonicMs();
int64_t buildUnix();           // when this firmware was compiled; earlier times are implausible

// ---- storage (storage.cpp) ---------------------------------------------------------------------

bool fsBegin();  // mount LittleFS on "spiffs", formatting only if mounting fails
bool fsExists(const std::string& path);
bool fsReadAll(const std::string& path, std::string* out, size_t max);
// Write `path.tmp` then rename over `path`: a power cut leaves the old or the new file.
bool fsWriteAtomic(const std::string& path, const void* data, size_t n);
bool fsWriteJsonAtomic(const std::string& path, JsonVariantConst value);
bool fsWriteFile(const std::string& path, const void* data, size_t n);
bool fsRename(const std::string& from, const std::string& to);
bool fsRemove(const std::string& path);
bool fsMkdir(const std::string& path);
void fsRemoveTree(const std::string& path);
std::vector<std::string> fsList(const std::string& dir, bool wantDirs);
size_t fsSize(const std::string& path);
int64_t fsMtime(const std::string& path);  // seconds since the epoch, 0 unknown
netcore::Storage fsStorage();               // cached for a second; writes invalidate it
void fsStorageDirty();
void fsCleanLeftovers();                    // media/*.part and *.tmp

// ---- errors kept for /api/status -----------------------------------------------------------------

void noteError(const std::string& message);  // also logged (never with a URL or token)

// ---- requests ------------------------------------------------------------------------------------

struct HttpReq {
  std::string method;  // "GET", "PUT", ...
  std::string path;    // percent-decoded, no query
  std::vector<std::pair<std::string, std::string>> query;  // blank values kept
  bool hasPin = false;
  std::string pin;
  std::string ip;
  bool fromSetupAp = false;
  std::string body;
  bool bodyTooLarge = false;
  bool bodyFormEncoded = false;  // ESPAsyncWebServer consumed it as form fields
  const std::string* param(const char* name) const;
};

struct HttpResp {
  int status = 200;
  std::shared_ptr<std::string> body = std::make_shared<std::string>();
  int retryAfter = -1;
  void json(int status, JsonVariantConst value);
  void error(const netcore::ApiError& e);
};

// ---- the store (store.cpp), used from the api and ig tasks ---------------------------------------

void storeBegin();  // recovery, defaults, prune, first publish
// Works out the display schedule (API.md 7.4) and tells the display half. Called at least
// once a minute from net_loop, and straight after a schedule save or a wake/sleep. False
// if the store was busy (an api request holds it): try again shortly.
bool scheduleTick();
void storeClearPin();  // BOOT held 10 s at power-up
void handleApi(const HttpReq& req, HttpResp* resp);  // api task only

// Uploads: the api task authorizes and runs the checks that need the store, then
// finishes the upload once the body has been streamed.
struct UploadInfo {
  bool firmware = false;
  netcore::UploadParams params;  // media
  size_t total = 0;
};
netcore::ApiError uploadAuthorize(const HttpReq& req, const UploadInfo& info, const netcore::ApiError& early);
void uploadMediaFinish(const UploadInfo& info, HttpResp* resp);  // the .part file is complete
void uploadRelease(const UploadInfo& info);                       // drop a reservation after a failure

// A recursive lock over every config document. Held briefly; never across network I/O.
struct StoreLock {
  StoreLock();
  ~StoreLock();
};

// Instagram state the worker reads and writes under StoreLock.
std::string igToken();
JsonDocument& igDoc();
void igCommit(bool publishContent);   // writes instagram.json; publishes when tiles changed
void igPruneGenerations(const std::string& keep);  // also spares the one being built
void igSetBuilding(const std::string& gen);
std::string igGenerationOf(JsonVariantConst ig);
void publish();

// ---- instagram (instagram.cpp) ---------------------------------------------------------------------

void igBegin();
// 202 / 409 busy, from the api task. Checks for a token are the caller's.
netcore::ApiError igStartJob(const char* kind);
const char* igBusy();  // nullptr, "refresh" or "accounts"
void igAccountsJson(JsonObject out);  // "accounts" and "accounts_error"
void igForgetAccounts();

// ---- firmware update (ota.cpp) -------------------------------------------------------------------

struct OtaSink;
OtaSink* otaBegin(size_t total, netcore::ApiError* err);  // nullptr + err if not possible
bool otaWrite(OtaSink* s, const uint8_t* data, size_t n);
netcore::ApiError otaFinish(OtaSink* s, std::string* version);  // validates and sets the boot partition
void otaAbort(OtaSink* s);
netcore::ApiError otaCheckHeader(const uint8_t* data, size_t n);
size_t otaMaxSize();
void otaHealthLoop(bool wifiUp);
void otaStatusJson(JsonObject out);

// ---- Wi-Fi, server, setup (wifi_setup.cpp, http_server.cpp) ---------------------------------------

void wifiBegin(bool forceSetup);
void wifiLoop();
bool wifiConnected();
bool wifiSetupMode();
void wifiStatusJson(JsonObject out);
netcore::ApiError wifiSave(JsonVariantConst body, bool fromSetupAp);
void setupMessageReshow();  // after a publish, put the setup details back on the wall

void serverBegin();
uint32_t apiHeartbeat();  // increments while the api task is alive

void restartSoon(uint32_t ms);

}  // namespace net
}  // namespace wall
