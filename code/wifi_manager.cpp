#include "wifi_manager.h"

#include <Preferences.h>
#include <WiFi.h>

#include "web_handlers.h"
#include "wifi_config.h"

static String configuredSsid;
static String configuredPassword;
static unsigned long lastReconnectAttempt = 0;

void loadWiFiCredentials() {
  Preferences wifiPreferences;
  wifiPreferences.begin("wifi_config", true);
  configuredSsid = wifiPreferences.getString("ssid", WIFI_SSID);
  configuredPassword = wifiPreferences.getString("password", WIFI_PASS);
  wifiPreferences.end();
}

bool saveWiFiCredentials(const String& ssid, const String& password) {
  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63) {
    return false;
  }

  Preferences wifiPreferences;
  if (!wifiPreferences.begin("wifi_config", false)) return false;
  bool ok = wifiPreferences.putString("ssid", ssid) == ssid.length();
  // An empty password is valid for an open network.
  size_t passwordBytes = wifiPreferences.putString("password", password);
  ok = (password.length() == 0 || passwordBytes == password.length()) && ok;
  wifiPreferences.end();
  if (!ok) return false;

  configuredSsid = ssid;
  configuredPassword = password;
  return true;
}

bool connectConfiguredWiFi(unsigned long timeoutMs) {
  if (!hasConfiguredWiFi()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_FAST_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.disconnect(false, false);
  WiFi.begin(configuredSsid.c_str(), configuredPassword.c_str());
  logCaptureLn(String("连接 WiFi: ") + configuredSsid);

  const unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < timeoutMs) {
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    logCaptureLn(String("WiFi 已连接, IP: ") + WiFi.localIP().toString());
    return true;
  }

  logCaptureLn(String("WiFi 连接失败，可通过 BLE 更新网络"));
  return false;
}

void maintainWiFiConnection() {
  if (WiFi.status() == WL_CONNECTED || !hasConfiguredWiFi()) return;
  if (millis() - lastReconnectAttempt < 30000) return;
  lastReconnectAttempt = millis();
  WiFi.reconnect();
}

const String& getConfiguredWiFiSsid() {
  return configuredSsid;
}

bool hasConfiguredWiFi() {
  return configuredSsid.length() > 0;
}
