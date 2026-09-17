#include "ble_provisioning.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>

#include "web_handlers.h"
#include "wifi_manager.h"
#include "red_ble.h"

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
static NimBLEServer* bleServer = nullptr;
static String bleDeviceSuffix;
static bool advertisingRedName = true;
static unsigned long lastAdvertisingNameSwitchAt = 0;
static portMUX_TYPE provisioningMux = portMUX_INITIALIZER_UNLOCKED;

static const unsigned long ADVERTISING_NAME_INTERVAL_MS = 1500;

static String advertisedDeviceName(bool redName) {
  return String(redName ? "ESTKme-" : "SMS-") + bleDeviceSuffix;
}

static bool applyAdvertisingName(bool redName, bool refresh) {
  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  if (!advertising) return false;

  // NimBLEAdvertisementData::setName appends a field.  Rebuild both legacy
  // advertising payloads so repeated name changes never accumulate fields.
  advertising->clearData();
  advertising->enableScanResponse(true);
  const bool redServiceAdded = advertising->addServiceUUID("4553");
  const bool provisioningServiceAdded = advertising->addServiceUUID(SERVICE_UUID);
  const String name = advertisedDeviceName(redName);
  const bool nameSet = advertising->setName(name.c_str());
  NimBLEDevice::setDeviceName(name.c_str());
  const bool refreshed = !refresh || advertising->refreshAdvertisingData();
  return redServiceAdded && nameSet && refreshed && provisioningServiceAdded;
}

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

class SharedServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    (void)server;
    redBleOnConnect(connInfo.getConnHandle(), connInfo.getMTU());
  }

  void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
    (void)server;
    (void)reason;
    redBleOnDisconnect(connInfo.getConnHandle());
  }

  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override {
    redBleOnMtuChange(connInfo.getConnHandle(), mtu);
  }
};

static SharedServerCallbacks sharedServerCallbacks;

static bool verifyGattService(NimBLEService* service) {
  if (!service) return false;

  const uint16_t handle = service->getHandle();
  logCaptureLn(String("BLE GATT service=") + service->getUUID().toString().c_str() +
               ", handle=" + String(handle));
  bool registered = handle != 0;
  for (const auto* characteristic : service->getCharacteristics()) {
    const uint16_t valueHandle = characteristic->getHandle();
    logCaptureLn(String("BLE GATT characteristic=") + characteristic->getUUID().toString().c_str() +
                 ", handle=" + String(valueHandle));
    if (valueHandle == 0) registered = false;
  }
  return registered;
}

void bleProvisioningBegin() {
  if (bleActive) return;

  logCaptureLn(String("BLE GATT check v1, build=") + __DATE__ + " " + __TIME__);
  // NekokoLPA2 selects RED BLE v1 for names containing ESTKme but not
  // "ESTKme RED".  Keep the suffix so multiple local devices are distinct.
  bleDeviceSuffix = String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFF), HEX);
  bleDeviceSuffix.toUpperCase();
  String deviceName = advertisedDeviceName(true);
  NimBLEDevice::init(deviceName.c_str());
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(BLE_PROVISIONING_PASSKEY);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&sharedServerCallbacks, false);
  bleServer->advertiseOnDisconnect(true);
  NimBLEService* service = bleServer->createService(SERVICE_UUID);

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
  // Register both services before starting GATT (required by NimBLE 2.3.7).
  const bool provisioningRegistered = service->start();
  NimBLEService* redService = provisioningRegistered ? redBleAddService(bleServer) : nullptr;
  if (!provisioningRegistered || !redService) {
    logCaptureLn(String("BLE service registration failed"));
    NimBLEDevice::deinit(true);
    statusCharacteristic = nullptr;
    bleServer = nullptr;
    return;
  }
  bleServer->start();

  // NimBLE 2.3.7 start() returns void. Assigned handles confirm that GATT
  // actually contains our services instead of trusting the advertising UUIDs.
  const bool provisioningReady = verifyGattService(service);
  const bool redReady = verifyGattService(redService);
  if (!provisioningReady || !redReady) {
    logCaptureLn(String("BLE GATT verification failed; advertising disabled"));
    NimBLEDevice::deinit(true);
    statusCharacteristic = nullptr;
    bleServer = nullptr;
    return;
  }
  logCaptureLn(String("BLE GATT verified: provisioning + RED 4553, address=") +
               NimBLEDevice::getAddress().toString().c_str());

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertisingRedName = true;
  bool advertisingDataSet = applyAdvertisingName(advertisingRedName, false);
  bool started = advertising->start();

  if (!started) {
    logCaptureLn(String("BLE 广播启动失败"));
    NimBLEDevice::deinit(true);
    statusCharacteristic = nullptr;
    bleServer = nullptr;
    return;
  }
  if (!advertisingDataSet) logCaptureLn(String("⚠️ BLE 广播字段不完整"));

  bleActive = true;
  lastAdvertisingNameSwitchAt = millis();
  logCaptureLn(String("BLE 已开启: ESTKme-/SMS-") + bleDeviceSuffix + ", RED v1 + 配网, PIN: " + String(BLE_PROVISIONING_PASSKEY));
}

void bleProvisioningLoop() {
  if (!bleActive) {
    if (WiFi.status() != WL_CONNECTED) bleProvisioningBegin();
    return;
  }

  redBleLoop();

  // A legacy advertisement has one local-name field.  While disconnected,
  // alternate names so both NekokoLPA2 and the original provisioning UI can
  // discover the same peripheral.  Freeze the name during a connection.
  if (bleServer && bleServer->getConnectedCount() == 0 &&
      millis() - lastAdvertisingNameSwitchAt >= ADVERTISING_NAME_INTERVAL_MS) {
    advertisingRedName = !advertisingRedName;
    if (!applyAdvertisingName(advertisingRedName, true)) {
      logCaptureLn(String("BLE 广播名称切换失败"));
    }
    lastAdvertisingNameSwitchAt = millis();
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

  // RED BLE is a reader service, so BLE must remain available after WiFi is
  // connected.  advertiseOnDisconnect() restores connectable advertising.
}

bool isBleProvisioningActive() {
  return bleActive;
}
