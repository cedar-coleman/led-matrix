// Wall-clock time: the PCF85063 RTC at boot, NTP once Wi-Fi is up, the RTC rewritten
// after every NTP sync. Until one of them gives a plausible time, the wall knows no date:
// nothing is stamped and no ages are drawn.
//
// The local date is computed from the zone's POSIX rule by netcore (nc_tz), not with
// setenv("TZ") + localtime_r, because the render task asks from core 1 while the api task
// may change the zone; newlib's TZ state is one unsynchronised global.
#include <Arduino.h>
#include <Wire.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

#include "net_internal.h"
#include "wall_shared.h"

namespace wall {
namespace net {

namespace {

const char* const TAG = "time";

// I2C for the on-board RTC. Source: Waveshare's ESP-IDF BSP for this board,
// github.com/waveshareteam/ESP32-S3-RGB-Matrix at commit 6e92fbe,
// example/idf_v5.5.2/components/bsp/esp32_s3_matrix/include/bsp/config.h:
//   #define BSP_I2C_SCL (GPIO_NUM_48)   #define BSP_I2C_SDA (GPIO_NUM_47)
// (I2C port 0, 400 kHz in esp32_s3_matrix.h). board_pins.h also reserves 47/48 for I2C.
constexpr int kSda = 47;
constexpr int kScl = 48;
constexpr uint8_t kRtcAddr = 0x51;  // PCF85063A fixed address
constexpr uint8_t kRegSeconds = 0x04;

std::atomic<bool> valid{false};
std::atomic<int> zone{netcore::kDefaultZone};
std::atomic<bool> synced{false};
bool rtcOk = false;
bool sntpStarted = false;

uint8_t fromBcd(uint8_t v) { return static_cast<uint8_t>((v >> 4) * 10 + (v & 15)); }
uint8_t toBcd(int v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

bool plausible(int64_t t) { return t >= buildUnix() && t < buildUnix() + 40LL * 365 * 86400; }

bool rtcRead(int64_t* out) {
  Wire.beginTransmission(kRtcAddr);
  Wire.write(kRegSeconds);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(static_cast<int>(kRtcAddr), 7) != 7) return false;
  uint8_t r[7];
  for (uint8_t& b : r) b = static_cast<uint8_t>(Wire.read());
  rtcOk = true;
  // OS (bit 7 of seconds): the oscillator stopped since the time was set, e.g. the
  // backup cell ran flat. Whatever the registers hold is then not a time.
  if (r[0] & 0x80) return false;
  const int sec = fromBcd(r[0] & 0x7F), min = fromBcd(r[1] & 0x7F), hour = fromBcd(r[2] & 0x3F);
  const int day = fromBcd(r[3] & 0x3F), month = fromBcd(r[5] & 0x1F), year = 2000 + fromBcd(r[6]);
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 59) return false;
  *out = netcore::daysFromCivil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
  return true;
}

bool rtcWrite(int64_t t) {
  const int64_t days = t / 86400, secs = t % 86400;
  int y;
  unsigned m, d;
  netcore::civilFromDays(days, &y, &m, &d);
  if (y < 2000 || y > 2099) return false;
  Wire.beginTransmission(kRtcAddr);
  Wire.write(kRegSeconds);
  Wire.write(toBcd(static_cast<int>(secs % 60)));  // bit 7 clear: clears OS
  Wire.write(toBcd(static_cast<int>(secs / 60 % 60)));
  Wire.write(toBcd(static_cast<int>(secs / 3600)));  // 24-hour mode is the chip's default
  Wire.write(toBcd(static_cast<int>(d)));
  Wire.write(static_cast<uint8_t>((days + 4) % 7));  // 1970-01-01 was a Thursday (4)
  Wire.write(toBcd(static_cast<int>(m)));
  Wire.write(toBcd(y - 2000));
  return Wire.endTransmission() == 0;
}

void onSync(struct timeval*) { synced.store(true); }

int monthIndex(const char* m) {
  static const char* const kNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* at = strstr(kNames, m);
  return at ? static_cast<int>(at - kNames) / 3 + 1 : 1;
}

}  // namespace

int64_t buildUnix() {
  // __DATE__ is "Nov  6 2026": midnight UTC of the day this file was compiled. A clock
  // that says it's earlier than that has not been set.
  static const int64_t t = [] {
    char mon[4] = {};
    int day = 1, year = 2026;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    return netcore::daysFromCivil(year, static_cast<unsigned>(monthIndex(mon)), static_cast<unsigned>(day)) * 86400;
  }();
  return t;
}

int64_t monotonicMs() { return esp_timer_get_time() / 1000; }

void timeBegin() {
  Wire.begin(kSda, kScl, 400000);
  int64_t t = 0;
  if (rtcRead(&t) && plausible(t)) {
    struct timeval tv = {static_cast<time_t>(t), 0};
    settimeofday(&tv, nullptr);
    valid.store(true);
    ESP_LOGI(TAG, "clock from the RTC: %s", netcore::isoUtc(t).c_str());
  } else {
    ESP_LOGW(TAG, "%s", rtcOk ? "RTC time not valid; waiting for NTP" : "RTC did not answer; waiting for NTP");
  }
}

void timeLoop() {
  if (!sntpStarted && wifiConnected()) {
    sntp_set_time_sync_notification_cb(onSync);
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");  // UTC; zones are ours
    sntpStarted = true;
  }
  if (synced.exchange(false)) {
    const int64_t now = time(nullptr);
    if (plausible(now)) {
      valid.store(true);
      if (rtcWrite(now)) {
        ESP_LOGI(TAG, "NTP: %s, RTC updated", netcore::isoUtc(now).c_str());
      } else {
        ESP_LOGW(TAG, "NTP: %s, but the RTC write failed", netcore::isoUtc(now).c_str());
      }
    }
  }
}

void timeSetZone(int z) { zone.store(z < 0 || z >= netcore::kZoneCount ? netcore::kDefaultZone : z); }

std::string nowIso() { return valid.load() ? netcore::isoUtc(time(nullptr)) : ""; }

std::string todayLocal() { return valid.load() ? netcore::localDate(zone.load(), time(nullptr)) : ""; }

}  // namespace net

// ---- wall_shared.h: called from the render task on core 1 ------------------------------------
// time() reads the system clock under newlib's own lock; the zone and the flag are atomics.

bool wall_time_valid() { return net::valid.load(); }
std::string wall_local_date() { return net::todayLocal(); }
int64_t wall_unix_time() { return net::valid.load() ? static_cast<int64_t>(time(nullptr)) : 0; }

}  // namespace wall
