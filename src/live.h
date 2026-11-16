// Live values shared between tasks (main loop, uplink task, web UI).
#pragma once
#include <Arduino.h>
#include "config.h"

enum PowerState : uint8_t { PS_BOOT = 0, PS_DRIVING, PS_IDLE, PS_STANDBY };
enum LinkType : uint8_t { LINK_NONE = 0, LINK_WIFI, LINK_CELL };

struct LiveValue {
  char key[24];
  float value;
  uint32_t ts;   // millis() of last update, 0 = never
};

#define LIVE_MAX_VALUES (MAX_OBD_PIDS + MAX_CAN_SIGNALS)

struct LiveData {
  // power / vehicle
  uint8_t powerState;
  bool ignition;
  bool motion;
  float voltage;
  float parkedVoltage;
  bool obdConnected;
  uint8_t obdProtocol;
  char vin[18];
  uint8_t dtcCount;
  uint16_t dtc[6];
  uint8_t devType;
  bool memsOk;
  int deviceTemp;
  LiveValue values[LIVE_MAX_VALUES];
  uint8_t valueCount;

  // GNSS
  bool gnssPresent;
  bool gnssFix;
  bool gnssFromModem;
  double lat, lon;
  float alt, speed, hdop;
  uint16_t course;
  uint8_t sat;
  uint32_t gnssUtc;     // unix time of last fix
  uint32_t gnssFixMs;   // millis() of last fix
  uint32_t gnssRejected;

  // network
  uint8_t link;         // LinkType
  bool wifiConnected;
  char wifiIp[16];
  char apIp[16];
  bool modemPresent;
  char modemModel[16];
  char imei[16];
  char iccid[24];
  char op[32];
  char netType[16];
  char cellIp[16];
  bool roaming;
  bool registered;
  int rssi;
  char lastError[64];
  uint32_t lastUploadMs;
  int lastHttpCode;
  uint32_t sent;
  uint32_t failed;

  uint32_t bootMs;
};

extern LiveData live;
extern SemaphoreHandle_t liveMutex;

inline void liveLock() { xSemaphoreTake(liveMutex, portMAX_DELAY); }
inline void liveUnlock() { xSemaphoreGive(liveMutex); }

void liveSetValue(const char* key, float value);
void liveSetError(const char* fmt, ...);
