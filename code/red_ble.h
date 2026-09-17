#ifndef RED_BLE_H
#define RED_BLE_H

#include <Arduino.h>

class NimBLEServer;
class NimBLEService;

// Returns the registered service, or nullptr on failure.
NimBLEService* redBleAddService(NimBLEServer* server);

// Runs slow UICC work outside NimBLE callbacks.
void redBleLoop();

// Called by the shared BLE server callbacks.
void redBleOnConnect(uint16_t connHandle, uint16_t mtu);
void redBleOnDisconnect(uint16_t connHandle);
void redBleOnMtuChange(uint16_t connHandle, uint16_t mtu);

#endif
