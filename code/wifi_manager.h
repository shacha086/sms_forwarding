#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

void loadWiFiCredentials();
bool saveWiFiCredentials(const String& ssid, const String& password);
bool connectConfiguredWiFi(unsigned long timeoutMs);
void maintainWiFiConnection();
const String& getConfiguredWiFiSsid();
bool hasConfiguredWiFi();

#endif
