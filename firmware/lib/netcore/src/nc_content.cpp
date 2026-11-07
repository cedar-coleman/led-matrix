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
  if (!strcmp(t, "new_prs")) return wall::SlideType::kNewPrs;
  if (!strcmp(t, "announcements")) return wall::SlideType::kAnnouncements;
  return wall::SlideType::kWhiteboard;
}

}  // namespace

const char* slideTypeName(wall::SlideType t) {
  switch (t) {
    case wall::SlideType::kMedia:
      return "media";
    case wall::SlideType::kInstagramStory:
      return "instagram_story";
    case wall::SlideType::kInstagramPosts:
      return "instagram_posts";
    case wall::SlideType::kNewPrs:
      return "new_prs";
    case wall::SlideType::kAnnouncements:
      return "announcements";
    default:
      return "whiteboard";
  }
}

namespace {

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
      JsonVariantConst p = a["pr_prev"][kLifts[k]];
      w.has_prev[k] = isJsonNumber(p);
      w.prev_kg[k] = w.has_prev[k] ? p.as<float>() : 0;
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

  for (JsonVariantConst a : src.announcements["announcements"].as<JsonArrayConst>()) {
    wall::Announcement w;
    w.id = str(a["id"]);
    w.text = str(a["text"]);
    w.enabled = !(a["enabled"].is<bool>() && !a["enabled"].as<bool>());
    w.start_date = str(a["start_date"]);
    w.end_date = str(a["end_date"]);
    c.announcements.push_back(std::move(w));
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
  JsonDocument power;  // a stored value the API wouldn't accept keeps the default supplies
  if (validatePower(st["power"], power.to<JsonObject>()).ok()) {
    c.settings.power.supplies = power["supplies"].as<uint8_t>();
    c.settings.power.rated_a = power["rated_a"].as<double>();
    c.settings.power.budget_pct = power["budget_pct"].as<uint8_t>();
  }

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
    // Only known previous values come back: Content keeps no difference between null and absent.
    JsonObject prev = o["pr_prev"].to<JsonObject>();
    for (int k = 0; k < wall::kLiftCount; k++) {
      if (a.has_prev[k]) prev[kLifts[k]] = a.prev_kg[k];
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

void announcementsToJson(const std::vector<wall::Announcement>& list, JsonArray out) {
  for (const wall::Announcement& a : list) {
    JsonObject o = out.add<JsonObject>();
    o["id"] = a.id;
    o["text"] = a.text;
    o["enabled"] = a.enabled;
    for (const auto& kv : {std::make_pair("start_date", &a.start_date), std::make_pair("end_date", &a.end_date)}) {
      if (kv.second->empty()) {
        o[kv.first] = nullptr;
      } else {
        o[kv.first] = *kv.second;
      }
    }
  }
}

}  // namespace netcore
