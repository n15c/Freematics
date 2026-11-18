// One position report as it is queued and uploaded.
#pragma once
#include <Arduino.h>
#include "config.h"

enum RecordEvent : uint8_t {
  EV_NONE = 0,
  EV_IGNITION_ON,
  EV_IGNITION_OFF,
  EV_HEARTBEAT,
  EV_BOOT,
  EV_WAKE,
};

struct Record {
  uint32_t ts;        // unix time (UTC, s); 0 = unknown, server time is used
  float lat;
  float lon;
  float alt;          // m
  float speed;        // km/h
  float hdop;
  uint16_t course;    // deg
  uint8_t sat;
  uint8_t event;      // RecordEvent
  bool valid;         // GNSS fix is current
  bool ignition;
  bool motion;
  int8_t rssi;        // dBm, 0 = unknown
  float power;        // vehicle voltage, 0 = unknown
  char attrs[RECORD_ATTR_LEN]; // "&key=value&key=value" (already URL safe)
};
