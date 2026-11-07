// JSON documents (as stored and served) to and from wall::Content (what the renderer draws).
#pragma once

#include <ArduinoJson.h>

#include <string>
#include <vector>

#include "wall_shared.h"

namespace netcore {

// The raw RGB565 tile the display reads, next to the BMP the console reads:
// "12/story-0.bmp" -> "/ig/12/story-0.565". "" for anything that isn't a tile path.
std::string tileRawPath(const std::string& file);

struct ContentSources {
  JsonObjectConst settings;
  JsonObjectConst athletes;    // {"rev", "athletes"}
  JsonObjectConst playlist;    // {"rev", "slides"}
  JsonObjectConst announcements;  // {"rev", "announcements"}
  JsonObjectConst mediaIndex;  // {"<name>.gif": {"peak_a", ...}}
  std::vector<std::string> mediaFiles;  // GIFs actually in /media
  JsonObjectConst instagram;
  bool igTokenSet = false;
};

wall::Content buildContent(const ContentSources& src, uint32_t version);

// The reverse, for the parts with a JSON form (used by tests to prove the round trip).
void athletesToJson(const std::vector<wall::Athlete>& athletes, JsonArray out);
void slidesToJson(const std::vector<wall::Slide>& slides, JsonArray out);
void announcementsToJson(const std::vector<wall::Announcement>& list, JsonArray out);

// The API's name for a slide type ("new_prs"), as in playlist.json and /api/status.
const char* slideTypeName(wall::SlideType t);

}  // namespace netcore
