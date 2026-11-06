#include "nc_api.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>

#include "nc_text.h"
#include "nc_tz.h"

namespace netcore {

namespace {

const char* const kDefaultCodes[][2] = {{"400", "invalid"},   {"401", "pin_required"}, {"403", "forbidden"},
                                        {"404", "not_found"}, {"409", "conflict"},     {"413", "too_large"},
                                        {"429", "locked"},    {"507", "no_space"}};

std::string joined(const char* const* items, size_t n) {
  std::string out;
  for (size_t i = 0; i < n; i++) out += (i ? ", " : "") + std::string(items[i]);
  return out;
}

bool isKg(JsonVariantConst v) {
  if (v.isNull()) return true;
  if (!isJsonNumber(v)) return false;
  const double d = v.as<double>();
  return d > 0 && d <= 500 && floor(d * 2) == d * 2;
}

// A string value, or nullptr when the value is not a string.
const char* str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : nullptr; }

// Counts what ArduinoJson consumed, so trailing garbage ("{} x") can be refused as
// Python's json.loads refuses it.
struct CountingReader {
  const char* p;
  size_t n, pos = 0;
  int read() { return pos < n ? static_cast<unsigned char>(p[pos++]) : -1; }
  size_t readBytes(char* buf, size_t len) {
    size_t k = 0;
    while (k < len && pos < n) buf[k++] = p[pos++];
    return k;
  }
};

bool jsonSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

ApiError ApiError::make(int status, std::string message, std::string code) {
  ApiError e;
  e.status = status;
  e.message = std::move(message);
  if (code.empty()) {
    code = "error";
    const std::string s = std::to_string(status);
    for (const auto& c : kDefaultCodes) {
      if (s == c[0]) code = c[1];
    }
  }
  e.code = std::move(code);
  return e;
}

void ApiError::toJson(JsonObject out) const {
  out["error"] = message;
  out["code"] = code;
  if (rev >= 0) out["rev"] = rev;
  if (!busy.empty()) out["busy"] = busy;
  if (retryAfter >= 0) out["retry_after_s"] = retryAfter;
}

bool pinSet(JsonObjectConst s) {
  const char* hash = str(s["pin_hash"]);
  const char* salt = str(s["pin_salt"]);
  return hash && *hash && salt && *salt && isJsonInt(s["pin_iter"]);
}

bool isFirstPinBody(JsonVariantConst body) {
  if (!body.is<JsonObjectConst>()) return false;
  JsonObjectConst o = body.as<JsonObjectConst>();
  return o.size() == 1 && hasKey(o, "new_pin");
}

ApiError authorizeWrite(PinGuard& guard, JsonObjectConst settings, const WriteRequest& req,
                        const std::function<bool(const std::string&)>& verify) {
  const int wait = guard.waitSeconds(req.ip);
  if (wait > 0) {
    ApiError e = ApiError::make(429, "Too many wrong PINs. Try again in " + std::to_string(wait) + " s.", "locked");
    e.retryAfter = wait;
    return e;
  }
  if (!pinSet(settings)) {
    if (req.firstPinRequest) {
      if (!req.deviceRules || req.fromSetupAp) return {};
      return ApiError::make(403, "Set the admin PIN from the wall's setup Wi-Fi", "pin_not_set");
    }
    return ApiError::make(403, "Set an admin PIN first", "pin_not_set");
  }
  if (!req.hasPinHeader || req.pin.empty()) return ApiError::make(401, "Admin PIN required", "pin_required");
  if (req.pin.size() > kMaxPinHeader || !verify(req.pin)) {
    guard.fail(req.ip);
    return ApiError::make(401, "That PIN is not right", "pin_wrong");
  }
  guard.succeed(req.ip);
  return {};
}

ApiError parseJsonBody(const char* data, size_t n, JsonDocument& doc) {
  doc.clear();
  if (n == 0) return {};  // json.loads(b"null")
  const ApiError bad = ApiError::make(400, "Body is not valid JSON");
  if (!utf8Valid(data, n)) return bad;
  CountingReader r{data, n};
  if (deserializeJson(doc, r)) return bad;
  for (size_t i = r.pos; i < n; i++) {
    if (!jsonSpace(data[i])) return bad;
  }
  return {};
}

ApiError requireObject(JsonVariantConst body, const char* listKey) {
  const bool ok = body.is<JsonObjectConst>() && (!listKey || body[listKey].is<JsonArrayConst>());
  if (ok) return {};
  return ApiError::make(400, listKey ? std::string("Expected {\"rev\": n, \"") + listKey + "\": [...]}" : "Expected an object");
}

bool parseConfigObject(const char* data, size_t n, JsonDocument& doc) {
  if (n == 0 || !parseJsonBody(data, n, doc).ok()) return false;
  return doc.is<JsonObject>();
}

ApiError checkRev(JsonVariantConst body, long long current) {
  JsonVariantConst rev = body["rev"];
  if (isJsonInt(rev) && rev.as<long long>() == current) return {};
  ApiError e = ApiError::make(409, "Someone else saved changes. Reload to see them.", "stale");
  e.rev = current;
  return e;
}

long long revOf(JsonVariantConst doc) {
  JsonVariantConst rev = doc["rev"];
  return isJsonInt(rev) ? rev.as<long long>() : 1;
}

ApiError validateAthletes(JsonArrayConst in, JsonArray out) {
  if (in.size() > static_cast<size_t>(kMaxAthletes)) return ApiError::make(400, "At most 500 athletes");
  std::set<std::string> seen;
  int i = 0;
  for (JsonVariantConst a : in) {
    i++;
    const std::string n = std::to_string(i);
    if (!a.is<JsonObjectConst>()) return ApiError::make(400, "Athlete " + n + " is not an object");
    const char* id = str(a["id"]);
    if (!id || !isId(id) || seen.count(id)) return ApiError::make(400, "Athlete " + n + ": missing or duplicate id");
    seen.insert(id);
    const char* name = str(a["name"]);
    const std::string trimmed = name ? pyStrip(name) : "";
    if (!name || trimmed.empty() || utf8Length(trimmed) > 40) {
      return ApiError::make(400, "Athlete " + n + ": name must be 1 to 40 characters");
    }
    JsonObject clean = out.add<JsonObject>();
    clean["id"] = id;
    clean["name"] = trimmed;
    clean["visible"] = !(a["visible"].is<bool>() && !a["visible"].as<bool>());
    for (const char* k : kLifts) {
      JsonVariantConst v = a[k];
      if (!isKg(v)) {
        return ApiError::make(400, std::string(name) + ": " + k + " must be blank or 0.5 to 500 kg in 0.5 kg steps");
      }
      clean[k] = v;  // keeps int or float, as the client sent it
    }
    const bool hasNotes = hasKey(a.as<JsonObjectConst>(), "notes");
    const char* notes = hasNotes ? str(a["notes"]) : "";
    if (!notes || utf8Length(notes) > 200) {
      return ApiError::make(400, std::string(name) + ": notes must be text up to 200 characters");
    }
    clean["notes"] = notes;
  }
  return {};
}

void stampPrs(JsonArray athletes, JsonArrayConst before, const std::string& today) {
  std::map<std::string, JsonObjectConst> old;  // last one wins, as in the mock's dict
  for (JsonVariantConst o : before) {
    if (const char* id = str(o["id"])) old[id] = o.as<JsonObjectConst>();
  }
  for (JsonObject a : athletes) {
    auto it = old.find(a["id"].as<const char*>());
    JsonObject dates = a["pr_dates"].to<JsonObject>();
    if (it == old.end()) continue;  // new athlete: no dates
    const JsonObjectConst o = it->second;
    if (o["pr_dates"].is<JsonObjectConst>()) {
      for (JsonPairConst kv : o["pr_dates"].as<JsonObjectConst>()) {
        for (const char* lift : kLifts) {
          if (kv.key() == lift) dates[lift] = kv.value();
        }
      }
    }
    if (today.empty()) continue;
    for (const char* lift : kLifts) {
      JsonVariantConst v = a[lift], ov = o[lift];
      if (v.isNull()) continue;
      if (!isJsonNumber(ov) || v.as<double>() > ov.as<double>()) dates[lift] = today;
    }
  }
}

ApiError validatePlaylist(JsonArrayConst in, const std::set<std::string>& mediaNames, JsonArray out) {
  if (in.size() > static_cast<size_t>(kMaxSlides)) return ApiError::make(400, "At most 100 slides");
  std::set<std::string> seen;
  int i = 0;
  for (JsonVariantConst s : in) {
    i++;
    const std::string n = std::to_string(i);
    const char* id = s.is<JsonObjectConst>() ? str(s["id"]) : nullptr;
    if (!id || !isId(id) || seen.count(id)) return ApiError::make(400, "Slide " + n + ": missing or duplicate id");
    seen.insert(id);
    JsonVariantConst d = s["duration_s"];
    if (!isJsonInt(d) || d.as<long long>() < kMinSeconds || d.as<long long>() > kMaxSeconds) {
      return ApiError::make(400, "Slide " + n + ": duration must be a whole number of seconds from 5 to 999");
    }
    const char* type = str(s["type"]);
    bool known = false;
    for (const char* t : kSlideTypes) known = known || (type && strcmp(type, t) == 0);
    if (type && strcmp(type, "media") == 0) {
      const char* file = str(s["file"]);
      if (!file || !mediaNames.count(file)) {
        return ApiError::make(400, "Slide " + n + ": media file " + pyRepr(s["file"]) + " is not on the board");
      }
    } else if (!known) {
      return ApiError::make(400, "Slide " + n + ": type must be one of " + joined(kSlideTypes, 4));
    }
    JsonObject clean = out.add<JsonObject>();
    clean["id"] = id;
    clean["type"] = type;
    clean["duration_s"] = d;
    clean["enabled"] = !(s["enabled"].is<bool>() && !s["enabled"].as<bool>());
    if (strcmp(type, "media") == 0) clean["file"] = s["file"];
  }
  return {};
}

void defaultSettings(JsonObject out) {
  out["title"] = "THE STRENGTH TANK";
  out["brightness"] = 50;
  out["sort"] = "board";
  out["pr_highlight_days"] = 7;
  out["timezone"] = kZones[kDefaultZone].iana;
  out["pin_salt"] = nullptr;
  out["pin_hash"] = nullptr;
  out["pin_iter"] = nullptr;
}

void fillSettingsDefaults(JsonObject s) {
  JsonDocument d;
  defaultSettings(d.to<JsonObject>());
  // DEFAULT_SETTINGS | file: defaults first, in their order, then anything else the file had.
  JsonDocument merged;
  JsonObject m = merged.to<JsonObject>();
  for (JsonPairConst kv : d.as<JsonObjectConst>()) {
    if (hasKey(s, kv.key().c_str())) {
      m[kv.key()] = JsonVariantConst(s[kv.key()]);
    } else {
      m[kv.key()] = kv.value();
    }
  }
  for (JsonPairConst kv : JsonObjectConst(s)) {
    if (!hasKey(m, kv.key().c_str())) m[kv.key()] = kv.value();
  }
  s.clear();
  for (JsonPairConst kv : JsonObjectConst(m)) s[kv.key()] = kv.value();
}

void settingsPublic(JsonObjectConst s, JsonObject out) {
  for (JsonPairConst kv : s) {
    if (strncmp(kv.key().c_str(), "pin_", 4) != 0) out[kv.key()] = kv.value();
  }
  out["pin_set"] = pinSet(s);
}

ApiError applySettings(JsonObjectConst body, JsonObject s, const std::function<void(uint8_t*, size_t)>& random) {
  if (hasKey(body, "title")) {
    const char* t = str(body["title"]);
    if (!t || utf8Length(t) > 32) return ApiError::make(400, "Title must be text up to 32 characters");
    s["title"] = t;
  }
  if (hasKey(body, "brightness")) {
    JsonVariantConst b = body["brightness"];
    if (!isJsonInt(b) || b.as<long long>() < 5 || b.as<long long>() > 100) {
      return ApiError::make(400, "Brightness must be 5 to 100");
    }
    s["brightness"] = b;
  }
  if (hasKey(body, "sort")) {
    const char* v = str(body["sort"]);
    bool ok = false;
    for (const char* k : kSorts) ok = ok || (v && strcmp(v, k) == 0);
    if (!ok) return ApiError::make(400, "Sort must be one of " + joined(kSorts, 3));
    s["sort"] = v;
  }
  if (hasKey(body, "pr_highlight_days")) {
    JsonVariantConst d = body["pr_highlight_days"];
    if (!isJsonInt(d) || d.as<long long>() < 0 || d.as<long long>() > 60) {
      return ApiError::make(400, "PR highlight must be 0 to 60 days");
    }
    s["pr_highlight_days"] = d;
  }
  if (hasKey(body, "timezone")) {
    const char* tz = str(body["timezone"]);
    if (!tz || zoneIndex(tz) < 0) {
      const char* names[kZoneCount];
      for (int i = 0; i < kZoneCount; i++) names[i] = kZones[i].iana;
      return ApiError::make(400, "Time zone must be one of " + joined(names, kZoneCount));
    }
    s["timezone"] = tz;
  }
  if (hasKey(body, "new_pin")) {
    const char* pin = str(body["new_pin"]);
    if (!pin || !isPin(pin)) return ApiError::make(400, "The PIN must be 6 to 12 digits");
    const PinRecord r = makePinRecord(pin, random);
    s["pin_salt"] = r.salt;
    s["pin_iter"] = r.iterations;
    s["pin_hash"] = r.hash;
  }
  return {};
}

void defaultInstagram(JsonObject out) {
  out["enabled"] = true;
  out["account_id"] = "";
  out["access_token"] = "";
  out["username"] = "thestrengthtank";
  out["refresh_minutes"] = 15;
  out["last_fetch"] = nullptr;
  out["last_error"] = nullptr;
  out["sample"] = false;  // the device has no sample tiles (API.md section 12)
  out["stories"].to<JsonArray>();
  out["posts"].to<JsonArray>();
}

ApiError applyInstagram(JsonObjectConst body, JsonObject ig, bool* tokenTouched) {
  *tokenTouched = false;
  if (hasKey(body, "enabled")) ig["enabled"] = body["enabled"].is<bool>() && body["enabled"].as<bool>();
  if (hasKey(body, "account_id")) {
    const char* a = str(body["account_id"]);
    if (!a || !isAccountId(a)) return ApiError::make(400, "The Instagram account ID is a number, up to 32 digits");
    ig["account_id"] = a;
  }
  if (hasKey(body, "refresh_minutes")) {
    JsonVariantConst m = body["refresh_minutes"];
    if (!isJsonInt(m) || m.as<long long>() < 5 || m.as<long long>() > 1440) {
      return ApiError::make(400, "Refresh every 5 to 1440 minutes");
    }
    ig["refresh_minutes"] = m;
  }
  if (hasKey(body, "access_token")) {
    const char* t = str(body["access_token"]);
    if (!t || !isAccessToken(t)) return ApiError::make(400, "That doesn't look like an access token");
    ig["access_token"] = t;
    *tokenTouched = true;
    if (!*t) {
      ig["last_error"] = nullptr;
      ig["last_fetch"] = nullptr;
      ig["stories"].to<JsonArray>();
      ig["posts"].to<JsonArray>();
      ig["sample"] = false;
    }
  }
  return {};
}

ApiError parseControl(JsonVariantConst body, std::string* action) {
  const char* a = body.is<JsonObjectConst>() ? str(body["action"]) : nullptr;
  for (const char* ok : {"next", "prev", "pause", "resume"}) {
    if (a && strcmp(a, ok) == 0) {
      *action = a;
      return {};
    }
  }
  return ApiError::make(400, "action must be next, prev, pause or resume");
}

ApiError parseUploadQuery(const std::string* name, const std::string* peak, const std::string* frames, UploadParams* out) {
  *out = UploadParams();
  out->name = name ? *name : "";
  if (!isMediaName(out->name)) {
    return ApiError::make(400, "File name must be lowercase letters, digits, . _ - and end in .gif");
  }
  if (peak) {
    // Python's float(): surrounding whitespace allowed, "nan"/"inf" parse (and are then refused).
    std::string t = *peak;
    while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t lead = 0;
    while (lead < t.size() && isspace(static_cast<unsigned char>(t[lead]))) lead++;
    t = t.substr(lead);
    char* end = nullptr;
    double v = NAN;
    if (!t.empty() && t.find_first_of("xX") == std::string::npos) {
      v = strtod(t.c_str(), &end);
      if (*end) v = NAN;
    }
    if (!isfinite(v) || v < 0 || v > kMaxPeakA) return ApiError::make(400, "peak_a must be a number from 0 to 40");
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", v);  // correctly rounded, like Python's round(v, 2)
    out->hasPeak = true;
    out->peak = strtod(buf, nullptr);
  }
  out->frames = -1;
  if (frames && !frames->empty() && frames->size() < 7) {
    bool all = true;
    for (char c : *frames) all = all && c >= '0' && c <= '9';
    if (all) out->frames = atoll(frames->c_str());
  }
  return {};
}

ApiError checkGifHeader(const uint8_t* d, size_t n) {
  if (n < 13 || (memcmp(d, "GIF87a", 6) != 0 && memcmp(d, "GIF89a", 6) != 0)) return ApiError::make(400, "Not a GIF file");
  const int w = d[6] | d[7] << 8, h = d[8] | d[9] << 8;
  if (w != 256 || h != 96) {
    return ApiError::make(400, "GIF must be 256x96, got " + std::to_string(w) + "x" + std::to_string(h));
  }
  return {};
}

void storageJson(const Storage& s, JsonObject out) {
  out["used_bytes"] = s.used;
  out["total_bytes"] = s.total;
  out["usable_bytes"] = s.usable();
}

void ErrorRing::add(const std::string& time, const std::string& message) {
  items_.emplace_back(time, message);
  while (items_.size() > cap_) items_.pop_front();
}

void ErrorRing::toJson(JsonArray out) const {
  for (const auto& e : items_) {
    JsonObject o = out.add<JsonObject>();
    if (e.first.empty()) {
      o["time"] = nullptr;
    } else {
      o["time"] = e.first;
    }
    o["message"] = e.second;
  }
}

std::string scrubMessage(std::string msg, const std::string& token) {
  if (token.size() >= 4) {
    for (size_t at; (at = msg.find(token)) != std::string::npos;) msg.replace(at, token.size(), "[token]");
  }
  for (const char* scheme : {"https://", "http://"}) {
    for (size_t at; (at = msg.find(scheme)) != std::string::npos;) {
      size_t end = at;
      while (end < msg.size() && !isspace(static_cast<unsigned char>(msg[end]))) end++;
      msg.replace(at, end - at, "[link]");
    }
  }
  if (utf8Length(msg) > 300) {
    size_t cut = 300;
    while (cut > 0 && (static_cast<unsigned char>(msg[cut]) & 0xC0) == 0x80) cut--;
    msg = msg.substr(0, cut) + "...";
  }
  return msg;
}

}  // namespace netcore
