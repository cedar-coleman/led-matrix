// The Instagram fetch, minus the network: Graph URLs, which items to download and in
// what order, and the messages a failure leaves in last_error (fetch_instagram and
// fetch_accounts in mock_server.py).
#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

namespace netcore {

constexpr const char* kGraph = "https://graph.facebook.com/v23.0";
constexpr const char* kMediaFields = "id,media_type,media_product_type,media_url,thumbnail_url,timestamp";
constexpr int kMaxStories = 5;
constexpr int kMaxPosts = 2;

// Graph URL for `path` with the token appended. The result carries the token: it must
// never be logged or put in a message.
std::string graphUrl(const std::string& path, const std::string& token);
std::string urlEncode(const std::string& s);

std::string profilePath(const std::string& account);
std::string storiesPath(const std::string& account);
std::string postsPath(const std::string& account);
std::string accountsPath();
std::string mePath();

struct IgDownload {
  std::string id, timestamp, url, kind;  // kind as the API reports it
  bool hasLikes = false;
  long long likes = 0;
  std::string permalink;  // "" = null
  bool hasPermalink = false;
};

// Stories: the first five returned, oldest first, items without an image skipped.
// Posts: the first two, in the order returned (newest first). False when an item has
// no id (the mock fails the whole refresh then).
bool planStories(JsonVariantConst graphAnswer, std::vector<IgDownload>* out);
bool planPosts(JsonVariantConst graphAnswer, std::vector<IgDownload>* out);

// me/accounts (data list) or me (one page) to [{"page", "id", "username"}].
bool parseAccounts(JsonVariantConst graphAnswer, bool singlePage, JsonArray out);

// Message for a Graph call that answered an HTTP error.
std::string graphHttpError(int status, const char* body, size_t n);

}  // namespace netcore
