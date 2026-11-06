#include "nc_ig.h"

#include <ctype.h>
#include <string.h>

#include <algorithm>

#include "nc_text.h"

namespace netcore {

namespace {

const char* str(JsonVariantConst v) { return v.is<const char*>() ? v.as<const char*>() : nullptr; }

// Python's `x or y` on JSON values: empty strings and null are falsy.
const char* truthy(JsonVariantConst v) {
  const char* s = str(v);
  return s && *s ? s : nullptr;
}

}  // namespace

std::string urlEncode(const std::string& s) {
  // urllib.parse.quote with its default safe "/" plus the unreserved set.
  static const char* hexd = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '_' || c == '.' || c == '-' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hexd[c >> 4];
      out += hexd[c & 15];
    }
  }
  return out;
}

std::string graphUrl(const std::string& path, const std::string& token) {
  return std::string(kGraph) + "/" + path + (path.find('?') == std::string::npos ? "?" : "&") +
         "access_token=" + urlEncode(token);
}

std::string profilePath(const std::string& account) { return account + "?fields=username"; }
std::string storiesPath(const std::string& account) { return account + "/stories?fields=" + kMediaFields; }
std::string postsPath(const std::string& account) {
  return account + "/media?fields=" + kMediaFields + ",like_count,permalink&limit=2";
}
// Braces percent-encoded: the HTTP client won't send them raw, and Graph decodes them.
std::string accountsPath() { return "me/accounts?fields=name,instagram_business_account%7Bid,username%7D"; }
std::string mePath() { return "me?fields=name,instagram_business_account%7Bid,username%7D"; }

bool planStories(JsonVariantConst answer, std::vector<IgDownload>* out) {
  std::vector<JsonVariantConst> raw;
  for (JsonVariantConst m : answer["data"].as<JsonArrayConst>()) {
    if (static_cast<int>(raw.size()) >= kMaxStories) break;
    raw.push_back(m);
  }
  std::stable_sort(raw.begin(), raw.end(), [](JsonVariantConst a, JsonVariantConst b) {
    const char* ta = str(a["timestamp"]);
    const char* tb = str(b["timestamp"]);
    return strcmp(ta ? ta : "", tb ? tb : "") < 0;
  });
  for (JsonVariantConst m : raw) {
    const char* type = str(m["media_type"]);
    const bool video = type && !strcmp(type, "VIDEO");
    const char* url = truthy(video ? m["thumbnail_url"] : m["media_url"]);
    if (!url) continue;
    const char* id = str(m["id"]);
    if (!id) return false;
    IgDownload d;
    d.id = id;
    d.timestamp = str(m["timestamp"]) ? str(m["timestamp"]) : "";
    d.url = url;
    d.kind = video ? "video" : "image";
    out->push_back(d);
  }
  return true;
}

bool planPosts(JsonVariantConst answer, std::vector<IgDownload>* out) {
  int n = 0;
  for (JsonVariantConst m : answer["data"].as<JsonArrayConst>()) {
    if (n++ >= kMaxPosts) break;
    const char* type = str(m["media_type"]);
    const bool video = type && !strcmp(type, "VIDEO");
    const char* url = video ? (truthy(m["thumbnail_url"]) ? truthy(m["thumbnail_url"]) : truthy(m["media_url"]))
                            : truthy(m["media_url"]);
    if (!url) continue;
    const char* id = str(m["id"]);
    if (!id) return false;
    IgDownload d;
    d.id = id;
    d.timestamp = str(m["timestamp"]) ? str(m["timestamp"]) : "";
    d.url = url;
    const char* product = str(m["media_product_type"]);
    if (video) {
      d.kind = product && !strcmp(product, "REELS") ? "reel" : "video";
    } else {
      d.kind = type && !strcmp(type, "CAROUSEL_ALBUM") ? "carousel" : "image";
    }
    d.hasLikes = isJsonInt(m["like_count"]);
    if (d.hasLikes) d.likes = m["like_count"].as<long long>();
    d.hasPermalink = str(m["permalink"]) != nullptr;
    if (d.hasPermalink) d.permalink = str(m["permalink"]);
    out->push_back(d);
  }
  return true;
}

bool parseAccounts(JsonVariantConst answer, bool singlePage, JsonArray out) {
  auto one = [&](JsonVariantConst pg) -> bool {
    JsonVariantConst iba = pg["instagram_business_account"];
    if (iba.isNull() || (iba.is<JsonObjectConst>() && iba.as<JsonObjectConst>().size() == 0)) return true;
    const char* id = str(iba["id"]);
    if (!id) return false;
    JsonObject o = out.add<JsonObject>();
    if (str(pg["name"])) {
      o["page"] = str(pg["name"]);
    } else {
      o["page"] = nullptr;
    }
    o["id"] = id;
    if (str(iba["username"])) {
      o["username"] = str(iba["username"]);
    } else {
      o["username"] = nullptr;
    }
    return true;
  };
  if (singlePage) return one(answer);
  for (JsonVariantConst pg : answer["data"].as<JsonArrayConst>()) {
    if (!one(pg)) return false;
  }
  return true;
}

std::string graphHttpError(int status, const char* body, size_t n) {
  JsonDocument filter;
  filter["error"]["message"] = true;
  JsonDocument doc;
  if (body && n && !deserializeJson(doc, body, n, DeserializationOption::Filter(filter))) {
    if (const char* msg = str(doc["error"]["message"])) return std::string("Instagram said: ") + msg;
  }
  return "Instagram said: HTTP " + std::to_string(status);
}

}  // namespace netcore
