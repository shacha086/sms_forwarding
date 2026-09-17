#ifndef ESIM_H
#define ESIM_H

#include "globals.h"

#define ESIM_MAX_ICCID_LEN 21
#define ESIM_MAX_ISDP_AID_LEN 33
#define ESIM_MAX_NAME_LEN 64
#define ESIM_RAW_APDU_MAX_LEN 4096

#ifndef ESIM_PROFILE_LOG
#define ESIM_PROFILE_LOG 0
#endif

struct ESimProfile {
  char iccid[ESIM_MAX_ICCID_LEN];
  char isdpAid[ESIM_MAX_ISDP_AID_LEN];
  char nickname[ESIM_MAX_NAME_LEN];
  char serviceProviderName[ESIM_MAX_NAME_LEN];
  char profileName[ESIM_MAX_NAME_LEN];
  int state;
  int profileClass;
};

bool esimInit();
bool esimGetEID(char* eid, size_t bufferSize);
int esimGetProfiles(ESimProfile* profiles, int maxProfiles);
bool esimEnableProfile(const char* iccidOrAid);
bool esimDisableProfile(const char* iccidOrAid);
bool esimDeleteProfile(const char* iccidOrAid);
bool esimSwitchProfile(const char* iccidOrAid);
bool esimGetNotificationCount(int* count);
const char* esimGetLastError();

// Raw basic-channel access used by BLE LPA readers.  These functions do not
// open logical channels, rewrite CLA, follow 61xx, or strip SW1/SW2.
bool esimRawPowerOn(uint8_t* atr, size_t atrCapacity, size_t* atrLen);
bool esimTransmitRawApdu(const uint8_t* command, size_t commandLen,
                         uint8_t* response, size_t responseCapacity,
                         size_t* responseLen);
void esimResetRawSession();

bool handleSerialConsole();
bool handleESimSerialCommand(const String& command);

#endif
