// The API's rules, as mock_server.py states them: errors, write authorization, revisions,
// validation of every body, PR stamping, and the public views of settings and Instagram.
// Pure functions over ArduinoJson values, so tests/test_mock_server.py's cases run on the
// host against exactly the code the device runs.
#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

#include <deque>
#include <functional>
#include <set>
#include <string>

#include "nc_pin.h"

namespace netcore {

constexpr size_t kMaxJsonBody = 256 * 1024;
constexpr size_t kMaxUpload = 8 * 1024 * 1024;
constexpr double kMaxPeakA = 40.0;
constexpr int kMaxAthletes = 500;
constexpr int kMaxSlides = 100;
constexpr int kMinSeconds = 5, kMaxSeconds = 999;
constexpr const char* kLifts[5] = {"snatch", "clean_jerk", "total", "back_squat", "front_squat"};
constexpr const char* kSlideTypes[4] = {"whiteboard", "media", "instagram_story", "instagram_posts"};
constexpr const char* kSorts[3] = {"board", "total", "name"};

struct ApiError {
  int status = 0;  // 0: no error
  std::string code;
  std::string message;
  long long rev = -1;       // stale: the current revision
  std::string busy;         // busy: the job running
  int retryAfter = -1;      // locked: seconds, also the Retry-After header

  bool ok() const { return status == 0; }
  static ApiError make(int status, std::string message, std::string code = "");
  void toJson(JsonObject out) const;
};

// ---- authorization (API.md 4.2) --------------------------------------------------------

struct WriteRequest {
  std::string ip;
  bool hasPinHeader = false;
  std::string pin;              // the X-Admin-Pin value
  bool firstPinRequest = false; // PUT /api/settings whose body is an object with exactly one key, new_pin
  bool fromSetupAp = false;     // the device only accepts the first PIN from its access point
  bool deviceRules = false;     // false: behave like the mock (first PIN from anywhere)
};

// Lockout, no-PIN, missing header, then the PIN itself. `verify` does the expensive hash
// and is only called when every cheap check passed. Counts failures and resets on success.
ApiError authorizeWrite(PinGuard& guard, JsonObjectConst settings, const WriteRequest& req,
                        const std::function<bool(const std::string&)>& verify);

bool pinSet(JsonObjectConst settings);
bool isFirstPinBody(JsonVariantConst body);

// ---- bodies -------------------------------------------------------------------------------

// Parse a request body as the mock's json_body() does. An empty body is JSON null.
ApiError parseJsonBody(const char* data, size_t n, JsonDocument& doc);
// object_body(): an object, and with `listKey` an object whose `listKey` is an array.
ApiError requireObject(JsonVariantConst body, const char* listKey = nullptr);

// A config file read back from flash: valid UTF-8, parses, and is a JSON object.
bool parseConfigObject(const char* data, size_t n, JsonDocument& doc);

ApiError checkRev(JsonVariantConst body, long long current);
long long revOf(JsonVariantConst doc);  // a missing or non-integer rev reads as 1

ApiError validateAthletes(JsonArrayConst in, JsonArray out);
// Server-side PR dates. today "" (clock not valid) stamps nothing; stored dates are kept.
void stampPrs(JsonArray athletes, JsonArrayConst before, const std::string& today);
ApiError validatePlaylist(JsonArrayConst in, const std::set<std::string>& mediaNames, JsonArray out);

// Apply a PUT /api/settings body to `settings` (a copy: on error the caller drops it).
ApiError applySettings(JsonObjectConst body, JsonObject settings, const std::function<void(uint8_t*, size_t)>& random);
void defaultSettings(JsonObject out);
void fillSettingsDefaults(JsonObject s);  // missing keys take their defaults
void settingsPublic(JsonObjectConst s, JsonObject out);

// PUT /api/instagram. `tokenTouched`: the body had access_token (accounts are forgotten).
ApiError applyInstagram(JsonObjectConst body, JsonObject ig, bool* tokenTouched);
void defaultInstagram(JsonObject out);

ApiError parseControl(JsonVariantConst body, std::string* action);

// ---- media --------------------------------------------------------------------------------

struct UploadParams {
  std::string name;
  bool hasPeak = false;
  double peak = 0;    // rounded to 2 decimals
  long long frames = -1;  // < 0: null
};
// name, peak_a and frames from the query string; a null pointer means the parameter is absent.
ApiError parseUploadQuery(const std::string* name, const std::string* peak, const std::string* frames, UploadParams* out);
// The first bytes of the body: GIF signature, at least 13 bytes, 256x96 logical screen.
ApiError checkGifHeader(const uint8_t* data, size_t n);

struct Storage {
  uint64_t used = 0, total = 0;
  uint64_t usable() const { return total * 9 / 10; }  // floor(total * 0.9)
};
void storageJson(const Storage& s, JsonObject out);

// ---- errors kept for /api/status --------------------------------------------------------------

class ErrorRing {
 public:
  explicit ErrorRing(size_t cap = 20) : cap_(cap) {}
  void add(const std::string& time, const std::string& message);  // time "" is written as null
  void toJson(JsonArray out) const;
  size_t size() const { return items_.size(); }

 private:
  size_t cap_;
  std::deque<std::pair<std::string, std::string>> items_;
};

// Remove anything that must never reach the coach or a log: the token and URLs.
std::string scrubMessage(std::string msg, const std::string& token);

}  // namespace netcore
