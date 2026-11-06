// The web console, gzipped into the app image by scripts/embed_web.py (generated into
// web_assets.gen.cpp before every build), so the UI and the API always ship together.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wall {
namespace net {

struct WebAsset {
  const char* path;  // "/index.html", "/vendor/omggif.js", ...
  const char* type;
  const uint8_t* gz;
  size_t size;
};

extern const WebAsset kWebAssets[];
extern const size_t kWebAssetCount;
extern const char* const kWebEtag;    // one ETag for the whole bundle, quoted
extern const char* const kUiVersion;  // UI_VERSION from mock_server.py + bundle hash

}  // namespace net
}  // namespace wall
