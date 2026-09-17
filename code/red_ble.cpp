#include "red_ble.h"

#include <NimBLEDevice.h>
#include <string.h>

#include "esim.h"
#include "web_handlers.h"

namespace {

constexpr char SERVICE_UUID[] = "00004553-0000-1000-8000-00805F9B34FB";
constexpr char TX_UUID[] = "00006D65-0000-1000-8000-00805F9B34FB";
constexpr char RX_UUID[] = "0000544B-0000-1000-8000-00805F9B34FB";

constexpr size_t HEADER_SIZE = 3;
constexpr size_t MAX_PAYLOAD = ESIM_RAW_APDU_MAX_LEN;
constexpr size_t MAX_FRAME = HEADER_SIZE + MAX_PAYLOAD;
constexpr size_t STREAM_CAPACITY = MAX_FRAME * 2;
constexpr uint16_t DEFAULT_MTU = 23;
constexpr uint16_t MAX_MTU = 247;
constexpr uint8_t CMD_CLAIM = 0x02;
constexpr uint8_t CMD_POWER_ON = 0x03;
constexpr uint8_t CMD_APDU = 0x04;

NimBLECharacteristic* s_rxCharacteristic = nullptr;
uint8_t s_streamBuffer[STREAM_CAPACITY];
size_t s_streamLength = 0;
uint8_t s_commandBuffer[MAX_PAYLOAD];
uint8_t s_responseBuffer[MAX_FRAME];
uint16_t s_connHandle = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_mtu = DEFAULT_MTU;
bool s_connected = false;
bool s_resetRequested = false;
bool s_overflowPending = false;
uint8_t s_overflowCommand = 0;
portMUX_TYPE s_redBleMux = portMUX_INITIALIZER_UNLOCKED;

void clearTransportLocked() {
  s_streamLength = 0;
  s_overflowPending = false;
  s_overflowCommand = 0;
}

class RedTxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo& connInfo) override {
    const std::string value = characteristic->getValue();
    if (value.empty()) return;

    portENTER_CRITICAL(&s_redBleMux);
    if (s_connected && s_connHandle != connInfo.getConnHandle()) {
      portEXIT_CRITICAL(&s_redBleMux);
      return;
    }
    if (!s_connected) {
      clearTransportLocked();
      s_resetRequested = true;
    }
    s_connHandle = connInfo.getConnHandle();
    s_mtu = connInfo.getMTU() >= DEFAULT_MTU ? connInfo.getMTU() : DEFAULT_MTU;
    s_connected = true;

    if (value.size() > STREAM_CAPACITY - s_streamLength) {
      s_overflowCommand = s_streamLength > 0
          ? s_streamBuffer[0]
          : static_cast<uint8_t>(value[0]);
      s_streamLength = 0;
      s_overflowPending = true;
    } else {
      memcpy(s_streamBuffer + s_streamLength, value.data(), value.size());
      s_streamLength += value.size();
    }
    portEXIT_CRITICAL(&s_redBleMux);
  }
};

RedTxCallbacks s_txCallbacks;

bool takeNextFrame(uint8_t* command, size_t* payloadLength, bool* invalidLength) {
  bool available = false;
  *invalidLength = false;

  portENTER_CRITICAL(&s_redBleMux);
  if (s_streamLength >= HEADER_SIZE) {
    const size_t length = static_cast<size_t>(s_streamBuffer[1]) |
                          (static_cast<size_t>(s_streamBuffer[2]) << 8);
    if (length > MAX_PAYLOAD) {
      *command = s_streamBuffer[0];
      s_streamLength = 0;
      *payloadLength = 0;
      *invalidLength = true;
      available = true;
    } else if (s_streamLength >= HEADER_SIZE + length) {
      *command = s_streamBuffer[0];
      *payloadLength = length;
      if (length > 0) memcpy(s_commandBuffer, s_streamBuffer + HEADER_SIZE, length);

      const size_t consumed = HEADER_SIZE + length;
      const size_t remaining = s_streamLength - consumed;
      if (remaining > 0) memmove(s_streamBuffer, s_streamBuffer + consumed, remaining);
      s_streamLength = remaining;
      available = true;
    }
  }
  portEXIT_CRITICAL(&s_redBleMux);
  return available;
}

bool takeOverflow(uint8_t* command) {
  bool pending = false;
  portENTER_CRITICAL(&s_redBleMux);
  if (s_overflowPending) {
    *command = s_overflowCommand;
    s_overflowPending = false;
    pending = true;
  }
  portEXIT_CRITICAL(&s_redBleMux);
  return pending;
}

bool notifyFrame(uint8_t command, const uint8_t* payload, size_t payloadLength) {
  if (!s_rxCharacteristic || payloadLength > MAX_PAYLOAD) return false;

  uint16_t connHandle;
  uint16_t mtu;
  bool connected;
  portENTER_CRITICAL(&s_redBleMux);
  connHandle = s_connHandle;
  mtu = s_mtu;
  connected = s_connected;
  portEXIT_CRITICAL(&s_redBleMux);
  if (!connected || connHandle == BLE_HS_CONN_HANDLE_NONE) return false;

  s_responseBuffer[0] = command;
  s_responseBuffer[1] = static_cast<uint8_t>(payloadLength & 0xFF);
  s_responseBuffer[2] = static_cast<uint8_t>((payloadLength >> 8) & 0xFF);
  if (payloadLength > 0 && payload != s_responseBuffer + HEADER_SIZE) {
    memcpy(s_responseBuffer + HEADER_SIZE, payload, payloadLength);
  }

  const size_t frameLength = HEADER_SIZE + payloadLength;
  size_t chunkSize = mtu > 3 ? mtu - 3 : 20;
  if (chunkSize < 20) chunkSize = 20;
  if (chunkSize > MAX_MTU - 3) chunkSize = MAX_MTU - 3;

  for (size_t offset = 0; offset < frameLength; offset += chunkSize) {
    const size_t remaining = frameLength - offset;
    const size_t length = remaining < chunkSize ? remaining : chunkSize;
    bool sent = false;
    for (int attempt = 0; attempt < 10 && !sent; ++attempt) {
      sent = s_rxCharacteristic->notify(s_responseBuffer + offset, length, connHandle);
      if (!sent) delay(8);
    }
    if (!sent) {
      logCaptureLn(String("RED BLE Notify 失败: offset=") + String(offset));
      return false;
    }
    if (offset + length < frameLength) delay(4);
  }
  return true;
}

void sendTransportError(uint8_t command) {
  if (command == CMD_APDU) {
    const uint8_t status[] = {0x6F, 0x00};
    notifyFrame(command, status, sizeof(status));
  } else {
    notifyFrame(command, nullptr, 0);
  }
}

void processFrame(uint8_t command, size_t payloadLength) {
  if (command == CMD_CLAIM) {
    static const uint8_t expected[] = {'E', 'S', 'T', 'K', 'm', 'e'};
    if (payloadLength != sizeof(expected) || memcmp(s_commandBuffer, expected, sizeof(expected)) != 0) {
      logCaptureLn(String("RED BLE Claim 负载不匹配"));
    }
    notifyFrame(CMD_CLAIM, nullptr, 0);
    return;
  }

  if (command == CMD_POWER_ON) {
    uint8_t atr[64];
    size_t atrLength = 0;
    if (!esimRawPowerOn(atr, sizeof(atr), &atrLength)) {
      logCaptureLn(String("RED BLE Power On 失败: ") + esimGetLastError());
      // RED v1 has no error envelope for Power On.  A complete empty frame
      // keeps the client synchronized; a following APDU returns 6F00.
      atrLength = 0;
    }
    notifyFrame(CMD_POWER_ON, atr, atrLength);
    return;
  }

  if (command == CMD_APDU) {
    size_t responseLength = 0;
    if (payloadLength < 4) {
      const uint8_t wrongLength[] = {0x67, 0x00};
      notifyFrame(CMD_APDU, wrongLength, sizeof(wrongLength));
      return;
    }
    if (!esimTransmitRawApdu(s_commandBuffer, payloadLength,
                             s_responseBuffer + HEADER_SIZE, MAX_PAYLOAD,
                             &responseLength)) {
      logCaptureLn(String("RED BLE APDU 失败: ") + esimGetLastError());
      const uint8_t technicalProblem[] = {0x6F, 0x00};
      notifyFrame(CMD_APDU, technicalProblem, sizeof(technicalProblem));
      return;
    }
    notifyFrame(CMD_APDU, s_responseBuffer + HEADER_SIZE, responseLength);
    return;
  }

  logCaptureLn(String("RED BLE 未知命令: 0x") + String(command, HEX));
  notifyFrame(command, nullptr, 0);
}

}  // namespace

NimBLEService* redBleAddService(NimBLEServer* server) {
  s_rxCharacteristic = nullptr;
  if (!server) return nullptr;
  NimBLEService* service = server->createService(SERVICE_UUID);
  NimBLECharacteristic* tx = service->createCharacteristic(
      TX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, MAX_MTU - 3);
  s_rxCharacteristic = service->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::NOTIFY, MAX_MTU - 3);
  tx->setCallbacks(&s_txCallbacks);
  // NimBLE 2.3.7 requires explicit registration; 2.5.1 keeps this as a no-op.
  if (!service->start()) {
    s_rxCharacteristic = nullptr;
    return nullptr;
  }
  return service;
}

void redBleLoop() {
  bool reset = false;
  portENTER_CRITICAL(&s_redBleMux);
  if (s_resetRequested) {
    s_resetRequested = false;
    reset = true;
  }
  portEXIT_CRITICAL(&s_redBleMux);
  if (reset) esimResetRawSession();

  uint8_t command = 0;
  if (takeOverflow(&command)) {
    logCaptureLn(String("RED BLE 接收缓冲区溢出"));
    sendTransportError(command);
    return;
  }

  size_t payloadLength = 0;
  bool invalidLength = false;
  if (!takeNextFrame(&command, &payloadLength, &invalidLength)) return;
  if (invalidLength) {
    logCaptureLn(String("RED BLE 帧长度超过 4096 字节"));
    sendTransportError(command);
    return;
  }
  processFrame(command, payloadLength);
}

void redBleOnConnect(uint16_t connHandle, uint16_t mtu) {
  // A connection becomes the RED session owner only after it writes the RED
  // TX characteristic.  This lets the encrypted provisioning service coexist
  // without stealing or resetting an active LPA session.
  (void)connHandle;
  (void)mtu;
}

void redBleOnDisconnect(uint16_t connHandle) {
  portENTER_CRITICAL(&s_redBleMux);
  if (s_connHandle == connHandle) {
    clearTransportLocked();
    s_connHandle = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = DEFAULT_MTU;
    s_connected = false;
    s_resetRequested = true;
  }
  portEXIT_CRITICAL(&s_redBleMux);
}

void redBleOnMtuChange(uint16_t connHandle, uint16_t mtu) {
  portENTER_CRITICAL(&s_redBleMux);
  if (s_connHandle == connHandle) s_mtu = mtu >= DEFAULT_MTU ? mtu : DEFAULT_MTU;
  portEXIT_CRITICAL(&s_redBleMux);
}
