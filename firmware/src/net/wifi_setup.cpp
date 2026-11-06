// Wi-Fi: the saved network, retried for ever with backoff, or the setup access point.
//
// The access point opens only when there is no saved network or BOOT was held 5 s at
// power-up (net_main.cpp). A dropped network never opens it: an open door that appears
// whenever the gym's router reboots is exactly what an attacker would wait for.
#include <Arduino.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_random.h>

#include "net_internal.h"
#include "wall_shared.h"

namespace wall {
namespace net {

using netcore::ApiError;

namespace {

const char* const TAG = "wifi";
constexpr const char* kApSsid = "GymBoard-Setup";
constexpr const char* kHostname = "gymboard";
constexpr uint32_t kFirstRetryMs = 2000;
constexpr uint32_t kMaxRetryMs = 60000;

bool setup = false;
std::string apPassword;
std::string ssid, password;
uint32_t retryMs = kFirstRetryMs;
int64_t nextAttempt = 0;
bool wasConnected = false;
bool mdnsUp = false;

void loadCredentials() {
  Preferences p;
  if (!p.begin("wifi", true)) return;
  ssid = p.isKey("ssid") ? p.getString("ssid", "").c_str() : "";
  password = p.isKey("pass") ? p.getString("pass", "").c_str() : "";
  p.end();
}

// No 0/O, 1/l/I: it is read off an LED wall and typed on a phone.
std::string randomPassword() {
  static const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
  std::string out;
  for (int i = 0; i < 10; i++) out += kAlphabet[esp_random() % (sizeof kAlphabet - 1)];
  return out;
}

void showSetup() {
  const std::string l1 = std::string("WIFI ") + kApSsid + "  PASS " + apPassword;
  const std::string l2 = "THEN OPEN http://" + std::string(WiFi.softAPIP().toString().c_str()) + "/setup";
  display_show_message(l1.c_str(), l2.c_str());
}

void startMdns() {
  if (mdnsUp) return;
  mdnsUp = MDNS.begin(kHostname);
  if (mdnsUp) MDNS.addService("http", "tcp", 80);
}

}  // namespace

void wifiBegin(bool forceSetup) {
  loadCredentials();
  WiFi.persistent(false);  // credentials live in our own NVS namespace, not the driver's
  WiFi.setHostname(kHostname);
  if (forceSetup || ssid.empty()) {
    setup = true;
    WiFi.mode(WIFI_AP);  // the RF is on from here, so esp_random() is a true RNG
    apPassword = randomPassword();
    WiFi.softAP(kApSsid, apPassword.c_str());  // 8+ character password: WPA2-PSK
    ESP_LOGI(TAG, "setup access point %s at %s", kApSsid, WiFi.softAPIP().toString().c_str());
    showSetup();
    startMdns();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);  // wifiLoop owns retries and their backoff
  WiFi.begin(ssid.c_str(), password.c_str());
  nextAttempt = monotonicMs() + 15000;
  startMdns();
}

void wifiLoop() {
  if (setup) return;
  const bool up = WiFi.status() == WL_CONNECTED;
  const int64_t now = monotonicMs();
  if (up) {
    if (!wasConnected) {
      ESP_LOGI(TAG, "joined %s as %s (http://%s.local)", ssid.c_str(), WiFi.localIP().toString().c_str(), kHostname);
      wasConnected = true;
    }
    retryMs = kFirstRetryMs;
    nextAttempt = now + 15000;
    return;
  }
  if (wasConnected) {
    ESP_LOGW(TAG, "lost %s; retrying", ssid.c_str());
    wasConnected = false;
  }
  if (now < nextAttempt) return;
  WiFi.disconnect(false, false);
  WiFi.begin(ssid.c_str(), password.c_str());
  nextAttempt = now + retryMs;
  retryMs = std::min<uint32_t>(retryMs * 2, kMaxRetryMs);
}

bool wifiConnected() { return !setup && WiFi.status() == WL_CONNECTED; }
bool wifiSetupMode() { return setup; }

void wifiStatusJson(JsonObject out) {
  if (wifiConnected()) {
    out["ssid"] = ssid;
    out["rssi"] = WiFi.RSSI();
    out["ip"] = WiFi.localIP().toString().c_str();
  } else {
    out["ssid"] = nullptr;
    out["rssi"] = nullptr;
  }
  out["setup_ap"] = setup;  // device-only, additive
}

void setupMessageReshow() {
  if (setup) showSetup();
}

ApiError wifiSave(JsonVariantConst body, bool fromSetupAp) {
  if (!fromSetupAp) return ApiError::make(403, "Change the Wi-Fi from the wall's setup Wi-Fi", "setup_only");
  if (!body.is<JsonObjectConst>()) return ApiError::make(400, "Expected an object");
  const char* s = body["ssid"].is<const char*>() ? body["ssid"].as<const char*>() : nullptr;
  const char* p = body["password"].is<const char*>() ? body["password"].as<const char*>() : "";
  if (!body["password"].isNull() && !body["password"].is<const char*>()) p = nullptr;
  if (!s || !*s || strlen(s) > 32) return ApiError::make(400, "The network name must be 1 to 32 characters");
  const size_t n = p ? strlen(p) : 0;
  if (!p || (n != 0 && (n < 8 || n > 63))) {
    return ApiError::make(400, "The Wi-Fi password must be empty (open network) or 8 to 63 characters");
  }
  Preferences prefs;
  if (!prefs.begin("wifi", false)) return ApiError::make(500, "Couldn't save on the wall's storage. Try again.", "storage_error");
  const bool ok = prefs.putString("ssid", s) == strlen(s) && (n == 0 ? (prefs.putString("pass", "") , true) : prefs.putString("pass", p) == n);
  prefs.end();
  if (!ok) return ApiError::make(500, "Couldn't save on the wall's storage. Try again.", "storage_error");
  const std::string l1 = std::string("JOINING ") + s;
  display_show_message(l1.c_str(), "THE WALL WILL RESTART");
  restartSoon(2000);
  return {};
}

}  // namespace net
}  // namespace wall
