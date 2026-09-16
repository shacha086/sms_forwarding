#include "ble_provisioning.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "web_handlers.h"
#include "wifi_manager.h"

// Service: WiFi provisioning
// SSID and password are staged separately; writing "connect" applies both.
static const char* SERVICE_UUID = "7d2ea28a-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* SSID_UUID    = "7d2ea28b-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* PASS_UUID    = "7d2ea28c-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* COMMAND_UUID = "7d2ea28d-f7bd-485a-bd9d-92ad6ecfe93e";
static const char* STATUS_UUID  = "7d2ea28e-f7bd-485a-bd9d-92ad6ecfe93e";

#ifndef BLE_PROVISIONING_PASSKEY
#define BLE_PROVISIONING_PASSKEY 123456
#endif

static NimBLECharacteristic* statusCharacteristic = nullptr;
static char stagedSsid[33] = {0};
static char stagedPassword[64] = {0};
static volatile bool connectRequested = false;
static bool bleActive = false;
static unsigned long bleStartedAt = 0;
static portMUX_TYPE provisioningMux = portMUX_INITIALIZER_UNLOCKED;

static String escapeJson(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value.charAt(i);
    if (c == '\\' || c == '"') escaped += '\\';
    if (c == '\n') {
      escaped += "\\n";
    } else if (c == '\r') {
      escaped += "\\r";
    } else {
      escaped += c;
    }
  }
  return escaped;
}

static String statusJson(const char* state, const char* message) {
  String json = "{\"state\":\"" + String(state) + "\",\"message\":\"" + String(message) + "\"";
  json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"ssid\":\"" + escapeJson(getConfiguredWiFiSsid()) + "\"";
  if (WiFi.status() == WL_CONNECTED) json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  return json;
}

static void publishStatus(const char* state, const char* message) {
  if (!statusCharacteristic) return;
  String json = statusJson(state, message);
  statusCharacteristic->setValue(json.c_str());
  statusCharacteristic->notify();
}

class ProvisioningCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    const std::string value = characteristic->getValue();
    String incoming(value.c_str());

    portENTER_CRITICAL(&provisioningMux);
    if (characteristic->getUUID().equals(NimBLEUUID(SSID_UUID)) && incoming.length() <= 32) {
      memcpy(stagedSsid, incoming.c_str(), incoming.length());
      stagedSsid[incoming.length()] = '\0';
    } else if (characteristic->getUUID().equals(NimBLEUUID(PASS_UUID)) && incoming.length() <= 63) {
      memcpy(stagedPassword, incoming.c_str(), incoming.length());
      stagedPassword[incoming.length()] = '\0';
    } else if (characteristic->getUUID().equals(NimBLEUUID(COMMAND_UUID)) && incoming == "connect") {
      connectRequested = true;
    }
    portEXIT_CRITICAL(&provisioningMux);
  }

  void onRead(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    (void)connInfo;
    if (characteristic->getUUID().equals(NimBLEUUID(STATUS_UUID))) {
      String json = statusJson(WiFi.status() == WL_CONNECTED ? "connected" : "ready", "");
      characteristic->setValue(json.c_str());
    }
  }
};

static ProvisioningCharacteristicCallbacks provisioningCallbacks;

void bleProvisioningBegin() {
  if (bleActive) return;

  String deviceName = "SMS-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  deviceName.toUpperCase();
  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(BLE_PROVISIONING_PASSKEY);

  NimBLEServer* server = NimBLEDevice::createServer();
  server->advertiseOnDisconnect(true);
  NimBLEService* service = server->createService(SERVICE_UUID);

  const uint16_t secureWrite = NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC;
  NimBLECharacteristic* ssid = service->createCharacteristic(SSID_UUID, secureWrite, 32);
  NimBLECharacteristic* password = service->createCharacteristic(PASS_UUID, secureWrite, 63);
  NimBLECharacteristic* command = service->createCharacteristic(COMMAND_UUID, secureWrite, 16);
  statusCharacteristic = service->createCharacteristic(
      STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY, 160);

  ssid->setCallbacks(&provisioningCallbacks);
  password->setCallbacks(&provisioningCallbacks);
  command->setCallbacks(&provisioningCallbacks);
  statusCharacteristic->setCallbacks(&provisioningCallbacks);
  server->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(SERVICE_UUID);
  advertising->setName(deviceName.c_str());
  advertising->start();

  bleActive = true;
  bleStartedAt = millis();
  logCaptureLn(String("BLE 配网已开启: ") + deviceName + ", PIN: " + String(BLE_PROVISIONING_PASSKEY));
}

void bleProvisioningLoop() {
  if (!bleActive) {
    if (WiFi.status() != WL_CONNECTED) bleProvisioningBegin();
    return;
  }

  bool shouldConnect = false;
  char ssidBuffer[33] = {0};
  char passwordBuffer[64] = {0};
  portENTER_CRITICAL(&provisioningMux);
  if (connectRequested) {
    shouldConnect = true;
    connectRequested = false;
    memcpy(ssidBuffer, stagedSsid, sizeof(ssidBuffer));
    memcpy(passwordBuffer, stagedPassword, sizeof(passwordBuffer));
  }
  portEXIT_CRITICAL(&provisioningMux);

  if (shouldConnect) {
    String ssid(ssidBuffer);
    String password(passwordBuffer);
    if (!saveWiFiCredentials(ssid, password)) {
      publishStatus("error", "invalid credentials");
    } else {
      publishStatus("connecting", "");
      bool connected = connectConfiguredWiFi(15000);
      publishStatus(connected ? "connected" : "error", connected ? "" : "connection failed");
    }
  }

  // BLE is a provisioning channel, not a permanent service. Turn it off after
  // five minutes on a healthy WiFi connection to reclaim controller/host RAM.
  if (WiFi.status() == WL_CONNECTED && millis() - bleStartedAt > 300000) {
    NimBLEDevice::deinit(true);
    statusCharacteristic = nullptr;
    bleActive = false;
    logCaptureLn(String("BLE 配网窗口已关闭，重启设备可再次开启"));
  }
}

bool isBleProvisioningActive() {
  return bleActive;
}
