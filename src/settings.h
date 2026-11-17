// Runtime configuration, persisted as JSON in NVS and editable in the web UI.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "config.h"

enum NetMode : uint8_t { NET_AUTO = 0, NET_LTE = 1, NET_GSM = 2 };
enum ApMode : uint8_t { AP_ALWAYS = 0, AP_NO_STA = 1, AP_OFF = 2 };
enum ModemStandby : uint8_t { MODEM_STANDBY_OFF = 0, MODEM_STANDBY_SLEEP = 1, MODEM_STANDBY_ONLINE = 2 };
enum HttpMethod : uint8_t { HTTP_GET_QUERY = 0, HTTP_POST_FORM = 1 };

struct ObdPidCfg {
  uint8_t pid;          // mode 01 PID
  uint16_t interval;    // seconds between reads (0 = every cycle)
  char key[24];         // attribute name in the protocol, e.g. "rpm"
};

enum CanSignalMode : uint8_t { CAN_PASSIVE = 0, CAN_REQUEST = 1 };

struct CanSignalCfg {
  char key[24];          // attribute name, e.g. "soc"
  uint8_t mode;          // CanSignalMode
  uint32_t id;           // passive: CAN ID of the broadcast frame
  uint32_t txId;         // request: CAN ID to send the request to (e.g. 0x7E4)
  char request[20];      // request: hex payload, e.g. "22F40D" (UDS) or "015B"
  uint16_t startBit;     // passive: bit position in frame; request: bit position in response data (after SID+DID)
  uint8_t bitLen;        // 1..32
  bool bigEndian;        // Motorola byte order
  bool isSigned;
  float factor;
  float offset;
  uint16_t interval;     // seconds between reads (0 = every cycle)
};

struct Settings {
  // device / protocol
  char deviceId[32];        // OsmAnd "id" (unique id in Traccar); empty = IMEI or chip id
  char serverUrl[160];      // e.g. http://traccar.example.com:5055/
  char liveUrl[160];        // optional secondary target, newest point only (e.g. Home Assistant webhook)
  uint8_t httpMethod;       // HttpMethod
  uint16_t intervalDriving; // s between points while ignition is on
  uint16_t intervalIdle;    // s between points while ignition off but still awake
  uint16_t heartbeatMin;    // min between heartbeats while parked (0 = none)
  uint8_t headingChange;    // degrees: send extra point on course change (0 = off)

  // WiFi
  char wifiSsid[33];
  char wifiPass[65];
  char apSsid[33];          // empty = FREEMATICS-<id>
  char apPass[65];          // >= 8 chars or empty (open)
  uint8_t apMode;           // ApMode
  char webPass[33];         // web UI password (user "admin"), empty = none

  // cellular
  bool cellEnabled;
  char apn[64];
  char apnUser[32];
  char apnPass[32];
  char simPin[9];
  uint8_t netMode;          // NetMode
  uint8_t modemStandby;     // ModemStandby

  // GNSS
  bool gnssKeepOn;          // keep GNSS powered in standby (hot start, ~25 mA)
  bool gnssUseModem;        // use the modem's GNSS if no standalone receiver is found

  // vehicle
  bool obdEnabled;
  uint8_t obdProtocol;      // 0 = auto, else ELM protocol number
  uint8_t pidCount;
  ObdPidCfg pids[MAX_OBD_PIDS];
  bool canEnabled;
  uint8_t canProtocol;      // protocol used for CAN sniffing when OBD is off (6 = 11bit/500k)
  uint8_t canCount;
  CanSignalCfg can[MAX_CAN_SIGNALS];

  // power
  float runVoltage;         // >= this voltage means engine/DC-DC is running
  float wakeDelta;          // voltage rise above parked level that wakes the device
  float motionG;            // motion threshold in g for wakeup
  float lowVoltage;         // battery protection: below this (parked) everything non-essential is switched off
  uint16_t standbyDelay;    // s after ignition off before entering standby
};

extern Settings cfg;

void settingsDefaults(Settings& s);
bool settingsLoad();
bool settingsSave();
void settingsToJson(const Settings& s, JsonObject obj, bool hideSecrets);
// apply JSON (partial updates allowed); secrets equal to "********" are kept
void settingsFromJson(Settings& s, JsonObjectConst obj);

// remembered runtime facts (not part of the user configuration)
uint8_t nvsGetLastProtocol();
void nvsSetLastProtocol(uint8_t proto);
