// LittleFS on the "spiffs" partition: atomic writes, listings and the storage figures.
#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <mutex>

#include "net_internal.h"
#include "wall_shared.h"

namespace wall {
namespace net {

namespace {

const char* const TAG = "storage";

// Writes above this are bracketed with display_flash_write_begin/end. Smaller ones are
// over before the render task would notice the cache stall.
constexpr size_t kBracketBytes = 8 * 1024;

std::mutex storageMutex;
netcore::Storage cached;
int64_t cachedAtMs = -1;

struct FlashBracket {
  bool on;
  explicit FlashBracket(size_t n) : on(n >= kBracketBytes) {
    if (on) display_flash_write_begin();
  }
  ~FlashBracket() {
    if (on) display_flash_write_end();
  }
};

bool endsWith(const std::string& s, const char* tail) {
  const size_t n = strlen(tail);
  return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

}  // namespace

struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t n) override {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
  }
};

ArduinoJson::Allocator* psram() {
  static PsramAllocator a;
  return &a;
}

bool fsBegin() {
  if (LittleFS.begin(false, "/littlefs", 10, "spiffs")) return true;
  // Only an unmountable filesystem is formatted: a first boot, or real damage. Anything
  // that mounts keeps its files, however odd.
  ESP_LOGW(TAG, "LittleFS did not mount; formatting the spiffs partition");
  display_show_message("Preparing storage...", "This happens once");
  display_flash_write_begin();
  const bool ok = LittleFS.format() && LittleFS.begin(false, "/littlefs", 10, "spiffs");
  display_flash_write_end();
  if (!ok) ESP_LOGE(TAG, "LittleFS still won't mount after formatting");
  return ok;
}

bool fsExists(const std::string& path) { return LittleFS.exists(path.c_str()); }

bool fsReadAll(const std::string& path, std::string* out, size_t max) {
  File f = LittleFS.open(path.c_str(), "r");
  if (!f || f.isDirectory()) return false;
  const size_t n = f.size();
  if (n > max) return false;
  out->assign(n, '\0');  // above 4 KB, malloc lands in PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
  const size_t got = n ? f.read(reinterpret_cast<uint8_t*>(&(*out)[0]), n) : 0;
  return got == n;
}

bool fsWriteFile(const std::string& path, const void* data, size_t n) {
  FlashBracket bracket(n);
  File f = LittleFS.open(path.c_str(), "w");
  if (!f) return false;
  const size_t wrote = n ? f.write(static_cast<const uint8_t*>(data), n) : 0;
  f.close();
  fsStorageDirty();
  if (wrote != n) {
    LittleFS.remove(path.c_str());
    return false;
  }
  return true;
}

bool fsWriteAtomic(const std::string& path, const void* data, size_t n) {
  const std::string tmp = path.substr(0, path.rfind('.')) + ".tmp";
  if (!fsWriteFile(tmp, data, n)) {
    ESP_LOGE(TAG, "write %s failed (disk full?)", tmp.c_str());
    return false;
  }
  // LittleFS renames atomically over an existing file.
  if (!LittleFS.rename(tmp.c_str(), path.c_str())) {
    LittleFS.remove(tmp.c_str());
    ESP_LOGE(TAG, "rename to %s failed", path.c_str());
    return false;
  }
  return true;
}

bool fsWriteJsonAtomic(const std::string& path, JsonVariantConst value) {
  std::string text;
  text.reserve(measureJson(value) + 1);
  serializeJson(value, text);
  return fsWriteAtomic(path, text.data(), text.size());
}

bool fsRename(const std::string& from, const std::string& to) {
  const bool ok = LittleFS.rename(from.c_str(), to.c_str());
  fsStorageDirty();
  return ok;
}

bool fsRemove(const std::string& path) {
  const bool ok = LittleFS.remove(path.c_str());
  fsStorageDirty();
  return ok;
}

bool fsMkdir(const std::string& path) { return LittleFS.exists(path.c_str()) || LittleFS.mkdir(path.c_str()); }

void fsRemoveTree(const std::string& path) {
  File f = LittleFS.open(path.c_str());
  if (!f) return;
  if (!f.isDirectory()) {
    f.close();
    LittleFS.remove(path.c_str());
  } else {
    std::vector<std::string> children;
    for (File c = f.openNextFile(); c; c = f.openNextFile()) children.push_back(path + "/" + c.name());
    f.close();
    for (const std::string& c : children) fsRemoveTree(c);
    LittleFS.rmdir(path.c_str());
  }
  fsStorageDirty();
}

std::vector<std::string> fsList(const std::string& dir, bool wantDirs) {
  std::vector<std::string> out;
  File d = LittleFS.open(dir.c_str());
  if (!d || !d.isDirectory()) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (f.isDirectory() == wantDirs) out.push_back(f.name());
  }
  return out;
}

size_t fsSize(const std::string& path) {
  File f = LittleFS.open(path.c_str(), "r");
  return f ? f.size() : 0;
}

int64_t fsMtime(const std::string& path) {
  File f = LittleFS.open(path.c_str(), "r");
  return f ? static_cast<int64_t>(f.getLastWrite()) : 0;
}

netcore::Storage fsStorage() {
  // usedBytes() walks the filesystem's block allocation; the console polls status every
  // few seconds, so keep the answer for a second.
  std::lock_guard<std::mutex> l(storageMutex);
  const int64_t now = monotonicMs();
  if (cachedAtMs < 0 || now - cachedAtMs > 1000) {
    cached.total = LittleFS.totalBytes();
    cached.used = LittleFS.usedBytes();
    cachedAtMs = now;
  }
  return cached;
}

void fsStorageDirty() {
  std::lock_guard<std::mutex> l(storageMutex);
  cachedAtMs = -1;
}

void fsCleanLeftovers() {
  for (const std::string& n : fsList("/media", false)) {
    if (endsWith(n, ".part")) LittleFS.remove(("/media/" + n).c_str());
  }
  for (const std::string& n : fsList("/", false)) {
    if (endsWith(n, ".tmp")) LittleFS.remove(("/" + n).c_str());
  }
  fsStorageDirty();
}

}  // namespace net
}  // namespace wall
