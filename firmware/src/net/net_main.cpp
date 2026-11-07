// net_begin / net_loop: brings the network half up in order and runs its housekeeping.
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <atomic>

#include "net_internal.h"
#include "wall_shared.h"

namespace wall {
namespace net {

namespace {

const char* const TAG = "net";

constexpr int kBootPin = 0;            // the BOOT button, active low
constexpr uint32_t kBootWindowMs = 1500;  // time to press BOOT after the wall lights up
constexpr uint32_t kSetupHoldMs = 5000;
constexpr uint32_t kPinResetHoldMs = 10000;

std::atomic<int64_t> restartAt{-1};

// The schedule is minute-granular; 15 s keeps a change within a quarter minute of its time.
constexpr int64_t kScheduleEveryMs = 15000;
int64_t scheduleAt = 0;

struct BootAction {
  bool setup = false;
  bool clearPin = false;
};

// BOOT is a strapping pin: held while power is applied, the chip enters its ROM
// download mode and this firmware never runs. So "held at power-up" means pressed
// within kBootWindowMs of the wall lighting up, then held.
BootAction readBootButton() {
  BootAction a;
  pinMode(kBootPin, INPUT_PULLUP);
  const uint32_t t0 = millis();
  while (digitalRead(kBootPin) == HIGH) {
    if (millis() - t0 > kBootWindowMs) return a;
    delay(10);
  }
  display_show_message("KEEP HOLDING BOOT", "5 S: SETUP WIFI   10 S: CLEAR PIN");
  const uint32_t pressed = millis();
  bool told5 = false;
  while (digitalRead(kBootPin) == LOW) {
    const uint32_t held = millis() - pressed;
    if (held >= kPinResetHoldMs) break;
    if (held >= kSetupHoldMs && !told5) {
      display_show_message("SETUP WIFI WILL OPEN", "HOLD 5 S MORE TO CLEAR THE PIN");
      told5 = true;
    }
    delay(20);
  }
  const uint32_t held = millis() - pressed;
  a.setup = held >= kSetupHoldMs;
  a.clearPin = held >= kPinResetHoldMs;
  if (!a.setup) display_show_message("", "");
  return a;
}

}  // namespace

void restartSoon(uint32_t ms) { restartAt.store(monotonicMs() + ms); }

}  // namespace net

void net_begin() {
  using namespace net;
  // A TLS handshake or a PBKDF2 can legitimately take a few seconds on core 0; give
  // the watchdog room for that while still catching a task that is truly stuck.
  esp_task_wdt_config_t wdt = {15000, 1u << 0, true};
  esp_task_wdt_reconfigure(&wdt);

  const BootAction boot = readBootButton();
  if (!fsBegin()) display_show_message("STORAGE ERROR", "SETTINGS WILL NOT BE KEPT");
  timeBegin();
  storeBegin();  // publishes the first content
  if (boot.clearPin) {
    storeClearPin();
    display_show_message("ADMIN PIN CLEARED", "SET A NEW ONE FROM THE SETUP WIFI");
    delay(4000);
    ESP_LOGW(TAG, "admin PIN cleared by the BOOT button");
  }
  wifiBegin(boot.setup || boot.clearPin);
  serverBegin();
  igBegin();
  ESP_LOGI(TAG, "network half up; internal free %u, PSRAM free %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void net_loop() {
  using namespace net;
  wifiLoop();
  timeLoop();
  otaHealthLoop(wifiConnected());
  if (monotonicMs() >= scheduleAt && scheduleTick()) scheduleAt = monotonicMs() + kScheduleEveryMs;
  const int64_t at = restartAt.load();
  if (at >= 0 && monotonicMs() >= at) {
    ESP_LOGI(TAG, "restarting");
    delay(100);
    ESP.restart();
  }
}

}  // namespace wall
