// Firmware update over HTTP (POST /api/firmware) with rollback.
//
// The pioarduino Arduino libraries (framework-arduinoespressif32-libs, IDF v5.5.5) are
// built with CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y, and the bootloader they ship
// (bootloader_opi_80m.elf) contains the otadata PENDING_VERIFY/ABORTED handling. So a new
// image boots in PENDING_VERIFY; if it resets before it is marked valid, the bootloader
// marks it ABORTED and boots the previous slot.
//
// Arduino's initArduino() would mark the image valid at once unless verifyRollbackLater()
// returns true. It does here: the image is marked valid only after it has run healthily
// (network up, render task drawing, api task alive) for a minute, and rolled back if that
// hasn't happened within ten.
#include <Arduino.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <string.h>

#include <atomic>

#include "net_internal.h"
#include "wall_shared.h"

extern "C" bool verifyRollbackLater() { return true; }

namespace wall {
namespace net {

using netcore::ApiError;

struct OtaSink {
  esp_ota_handle_t handle = 0;
  const esp_partition_t* part = nullptr;
};

namespace {

const char* const TAG = "ota";

constexpr int64_t kHealthyForMs = 60 * 1000;
constexpr int64_t kGiveUpMs = 10 * 60 * 1000;

std::atomic<bool> inProgress{false};
bool pendingVerify = false;
bool checked = false;
int64_t healthySince = -1;
uint32_t lastBeat = 0;
int64_t lastCheck = 0;

ApiError notFirmware() { return ApiError::make(400, "That isn't a firmware file for this wall"); }

}  // namespace

size_t otaMaxSize() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  return p ? p->size : 0;
}

ApiError otaCheckHeader(const uint8_t* d, size_t n) {
  // Image header (24 bytes), first segment header (8), then esp_app_desc_t:
  // magic_word at +0, version at +16, project_name at +48.
  constexpr size_t kDesc = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
  if (n < kDesc + 80) return notFirmware();
  esp_image_header_t h;
  memcpy(&h, d, sizeof h);
  uint32_t magic;
  memcpy(&magic, d + kDesc, sizeof magic);
  if (h.magic != ESP_IMAGE_HEADER_MAGIC || h.chip_id != ESP_CHIP_ID_ESP32S3 || magic != ESP_APP_DESC_MAGIC_WORD) {
    return notFirmware();
  }
  char project[33] = {};
  memcpy(project, d + kDesc + 48, 32);
  if (strncmp(project, esp_app_get_description()->project_name, 32) != 0) return notFirmware();
  return {};
}

OtaSink* otaBegin(size_t total, ApiError* err) {
  if (inProgress.exchange(true)) {
    *err = ApiError::make(409, "Another update is being written. Wait for it to finish.", "busy");
    err->busy = "firmware";
    return nullptr;
  }
  auto* s = new OtaSink();
  s->part = esp_ota_get_next_update_partition(nullptr);
  // Sequential writes erase sector by sector as data arrives, instead of erasing the
  // whole 4 MB slot up front (seconds with the flash cache stalled).
  if (!s->part || total > s->part->size || esp_ota_begin(s->part, OTA_WITH_SEQUENTIAL_WRITES, &s->handle) != ESP_OK) {
    delete s;
    inProgress.store(false);
    *err = ApiError::make(500, "The wall couldn't start the update. Try again.", "storage_error");
    return nullptr;
  }
  ESP_LOGI(TAG, "writing %u bytes to %s", static_cast<unsigned>(total), s->part->label);
  return s;
}

bool otaWrite(OtaSink* s, const uint8_t* data, size_t n) { return s && esp_ota_write(s->handle, data, n) == ESP_OK; }

ApiError otaFinish(OtaSink* s, std::string* version) {
  ApiError e;
  const esp_err_t end = esp_ota_end(s->handle);  // checks the image's checksum and SHA-256
  if (end == ESP_ERR_OTA_VALIDATE_FAILED || end == ESP_ERR_INVALID_SIZE) {
    e = ApiError::make(400, "That firmware file is damaged or incomplete");
  } else if (end != ESP_OK || esp_ota_set_boot_partition(s->part) != ESP_OK) {
    e = ApiError::make(500, "The update couldn't be written. Try again.", "storage_error");
  } else {
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(s->part, &desc) == ESP_OK) *version = desc.version;
    ESP_LOGI(TAG, "update %s staged in %s; rebooting", version->c_str(), s->part->label);
  }
  delete s;
  inProgress.store(false);
  return e;
}

void otaAbort(OtaSink* s) {
  if (!s) return;
  esp_ota_abort(s->handle);
  delete s;
  inProgress.store(false);
}

void otaHealthLoop(bool wifiUp) {
  if (!checked) {
    checked = true;
    esp_ota_img_states_t state;
    const esp_partition_t* running = esp_ota_get_running_partition();
    pendingVerify = esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    if (pendingVerify) ESP_LOGW(TAG, "new firmware on trial: it must run healthily for a minute");
  }
  if (!pendingVerify) return;
  const int64_t now = monotonicMs();
  if (now - lastCheck < 1000) return;
  lastCheck = now;
  // The render task draws a frame at every screen change, so frames > 0 says it started
  // and got through a full frame; a hung or crashed render task resets the board (task
  // watchdog / panic), and a reset while on trial rolls back in the bootloader.
  const uint32_t beat = apiHeartbeat();
  const bool healthy = (wifiUp || wifiSetupMode()) && render_status().frames > 0 && beat != lastBeat;
  lastBeat = beat;
  if (!healthy) {
    healthySince = -1;
  } else if (healthySince < 0) {
    healthySince = now;
  } else if (now - healthySince >= kHealthyForMs) {
    esp_ota_mark_app_valid_cancel_rollback();
    pendingVerify = false;
    ESP_LOGI(TAG, "new firmware marked valid");
    return;
  }
  if (now > kGiveUpMs) {
    noteError("The new firmware didn't come up healthy; going back to the previous one");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

void otaStatusJson(JsonObject out) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  out["partition"] = running ? running->label : "";
  out["pending_verify"] = pendingVerify;
  out["max_bytes"] = otaMaxSize();
}

}  // namespace net
}  // namespace wall
