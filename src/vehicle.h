// Vehicle data: OBD-II PIDs and CAN signals (passive broadcast frames and
// active requests such as UDS 0x22), both through the Freematics co-processor.
#pragma once
#include <FreematicsPlus.h>
#include <ArduinoJson.h>
#include "settings.h"

struct CanFrameSeen {
  uint32_t id;
  uint8_t len;
  uint8_t data[8];
  uint32_t count;
  uint32_t ts;
};

class Vehicle {
public:
  void begin(FreematicsESP32* sys);
  // call regularly while awake; ignition tells how eagerly to (re)connect
  void update(bool ignition);
  bool connected() { return m_ready; }
  bool obdConnected() { return m_ready && m_obdMode; }
  // engine RPM > 0 reported within the last seconds
  bool engineRunning() { return m_rpmTs && millis() - m_rpmTs < 8000 && m_rpm > 0; }
  // vehicle voltage (fast ADC on newer units, ATRV otherwise)
  float readVoltage();
  // co-processor low power mode / wake up
  void sleep();
  // stop OBD/CAN activity but keep the co-processor awake (its GNSS keeps running)
  void idle();
  void wake();
  // append "&key=value" pairs of fresh values (max age in ms)
  void appendAttrs(char* buf, size_t size, uint32_t maxAge);
  // CAN monitor (web UI): sniff all IDs for the given time
  void startMonitor(uint16_t seconds);
  bool monitoring() { return m_monitorUntil && (int32_t)(m_monitorUntil - millis()) > 0; }
  void monitorJson(JsonArray arr);
  // last values (for live view); NAN if unknown
  float pidValue(int i) { return m_pidVal[i]; }
  float canValue(int i) { return m_canVal[i]; }
private:
  bool initLink(bool allowAuto);
  bool rawInit(uint8_t proto);
  uint8_t queryProtocol();
  void readVinDtc();
  void pollPids();
  void pollRpm();
  bool checkObdErrors();
  void pollCanRequests();
  void sniffWindow();
  bool requestSignal(const CanSignalCfg& c, float& out);
  void restoreHeader();
  void handleFrame(uint32_t id, const uint8_t* data, uint8_t len);
  int cmd(const char* c, char* buf, int bufsize, unsigned timeout = 1000);

  FreematicsESP32* m_sys = nullptr;
  COBD m_obd;
  bool m_ready = false;
  bool m_obdMode = false;   // standard OBD session (vs. CAN only)
  bool m_ext29 = false;     // 29-bit identifiers on the bus
  uint8_t m_proto = 0;
  uint32_t m_lastTry = 0;
  bool m_vinRead = false;

  float m_pidVal[MAX_OBD_PIDS];
  uint32_t m_pidTs[MAX_OBD_PIDS];
  uint32_t m_pidRead[MAX_OBD_PIDS];
  uint8_t m_pidCursor = 0;
  int m_rpm = 0;
  uint32_t m_rpmTs = 0;
  uint32_t m_rpmRead = 0;

  float m_canVal[MAX_CAN_SIGNALS];
  uint32_t m_canTs[MAX_CAN_SIGNALS];
  uint32_t m_canRead[MAX_CAN_SIGNALS];
  uint32_t m_lastSniff = 0;

  uint32_t m_monitorUntil = 0;
  CanFrameSeen m_seen[CAN_MONITOR_SLOTS];
  uint8_t m_seenCount = 0;
  SemaphoreHandle_t m_seenMutex = nullptr;
};

extern Vehicle vehicle;
