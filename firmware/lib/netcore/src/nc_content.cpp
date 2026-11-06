#include "nc_content.h"

#include <string.h>

#include "nc_api.h"
#include "nc_text.h"

namespace netcore {

namespace {

const char* str(JsonVariantConst v, const char* fallback = "") {
  return v.is<const char*>() ? v.as<const char*>() : fallback;
}

wall::SlideType slideType(const char* t) {
  if (!strcmp(t, "media")) return wall::SlideType::kMedia;
  if (!strcmp(t, "instagram_story")) return wall::SlideType::kInstagramStory;
  if (!strcmp(t, "instagram_posts")) return wall::SlideType::kInstagramPosts;
  return wall::SlideType::kWhiteboard;
}

const char* slideTypeName(wall::SlideType t) {
  switch (t) {
    case wall::SlideType::kMedia:
      return "media";
    case wall::SlideType::kInstagramStory:
      return "instagram_story";
    case wall::SlideType::kInstagramPosts:
      return "instagram_posts";
    default:
      return "whiteboard";
  }
}

wall::IgKind igKind(const char* k) {
  if (!strcmp(k, "video")) return wall::IgKind::kVideo;
  if (!strcmp(k, "reel")) return wall::IgKind::kReel;
  if (!strcmp(k, "carousel")) return wall::IgKind::kCarousel;
  return wall::IgKind::kImage;
}

void igItems(JsonVariantConst list, std::vector<wall::IgItem>* out, size_t max) {
  for (JsonVariantConst it : list.as<JsonArrayConst>()) {
    if (out->size() >= max) break;
    wall::IgItem item;
    item.tile_path = tileRawPath(str(it["file"]));
    item.timestamp = str(it["timestamp"]);
    item.kind = igKind(str(it["kind"]));
    item.like_count = isJsonInt(it["like_count"]) ? static_cast<int32_t>(it["like_count"].as<long long>()) : -1;
    out->push_back(item);
  }
}

}  // namespace

std::string tileRawPath(const std::string& file) {
  std::string gen, name;
  if (!parseTilePath(file, &gen, &name)) return "";
  return "/ig/" + gen + "/" + name.substr(0, name.size() - 4) + ".565";
}

wall::Content buildContent(const ContentSources& src, uint32_t version) {
  wall::Content c;
  c.version = version;

  for (JsonVariantConst a : src.athletes["athletes"].as<JsonArrayConst>()) {
    wall::Athlete w;
    w.id = str(a["id"]);
    w.name = str(a["name"]);
    w.visible = !(a["visible"].is<bool>() && !a["visible"].as<bool>());
    for (int k = 0; k < wall::kLiftCount; k++) {
      JsonVariantConst v = a[kLifts[k]];
      w.has[k] = isJsonNumber(v);
      w.kg[k] = w.has[k] ? v.as<float>() : 0;
      w.pr_date[k] = str(a["pr_dates"][kLifts[k]]);
    }
    c.athletes.push_back(std::move(w));
  }

  for (JsonVariantConst s : src.playlist["slides"].as<JsonArrayConst>()) {
    wall::Slide w;
    w.id = str(s["id"]);
    w.type = slideType(str(s["type"]));
    const long long d = isJsonInt(s["duration_s"]) ? s["duration_s"].as<long long>() : 8;
    w.duration_s = static_cast<uint16_t>(d < 0 ? 0 : d > 65535 ? 65535 : d);
    w.enabled = !(s["enabled"].is<bool>() && !s["enabled"].as<bool>());
    w.file = str(s["file"]);
    c.slides.push_back(std::move(w));
  }

  JsonObjectConst st = src.settings;
  c.settings.title = str(st["title"], "THE STRENGTH TANK");
  c.settings.brightness = static_cast<uint8_t>(isJsonInt(st["brightness"]) ? st["brightness"].as<int>() : 50);
  const char* sort = str(st["sort"], "board");
  c.settings.sort = !strcmp(sort, "total") ? wall::SortMode::kTotal
                    : !strcmp(sort, "name") ? wall::SortMode::kName
                                            : wall::SortMode::kBoard;
  c.settings.pr_highlight_days =
      static_cast<uint8_t>(isJsonInt(st["pr_highlight_days"]) ? st["pr_highlight_days"].as<int>() : 7);

  for (const std::string& name : src.mediaFiles) {
    wall::MediaInfo m;
    m.name = name;
    JsonVariantConst peak = src.mediaIndex[name]["peak_a"];
    m.peak_a = isJsonNumber(peak) ? peak.as<float>() : -1;
    c.media.push_back(std::move(m));
  }

  JsonObjectConst ig = src.instagram;
  c.instagram.enabled = !ig["enabled"].is<bool>() || ig["enabled"].as<bool>();
  c.instagram.username = str(ig["username"]);
  if (src.igTokenSet) {
    igItems(ig["stories"], &c.instagram.stories, 5);
    igItems(ig["posts"], &c.instagram.posts, 2);
  }
  return c;
}

void athletesToJson(const std::vector<wall::Athlete>& athletes, JsonArray out) {
  for (const wall::Athlete& a : athletes) {
    JsonObject o = out.add<JsonObject>();
    o["id"] = a.id;
    o["name"] = a.name;
    o["visible"] = a.visible;
    JsonObject dates;
    for (int k = 0; k < wall::kLiftCount; k++) {
      if (a.has[k]) {
        o[kLifts[k]] = a.kg[k];
      } else {
        o[kLifts[k]] = nullptr;
      }
    }
    dates = o["pr_dates"].to<JsonObject>();
    for (int k = 0; k < wall::kLiftCount; k++) {
      if (!a.pr_date[k].empty()) dates[kLifts[k]] = a.pr_date[k];
    }
  }
}

void slidesToJson(const std::vector<wall::Slide>& slides, JsonArray out) {
  for (const wall::Slide& s : slides) {
    JsonObject o = out.add<JsonObject>();
    o["id"] = s.id;
    o["type"] = slideTypeName(s.type);
    o["duration_s"] = s.duration_s;
    o["enabled"] = s.enabled;
    if (s.type == wall::SlideType::kMedia) o["file"] = s.file;
  }
}

}  // namespace netcore
