// GNSS handling: standalone receiver (external/internal) or the modem's GNSS.
#pragma once
#include <FreematicsPlus.h>

struct Fix {
  double lat;
  double lon;
  float alt;       // m
  float speed;     // km/h
  float hdop;
  uint16_t course; // deg
  uint8_t sat;
  uint32_t utc;    // unix time
  uint32_t ms;     // millis() when received
};

class Gnss {
public:
  // start the standalone receiver; false if none found
  bool begin(FreematicsESP32* sys);
  void end();
  bool standalone() { return m_standalone; }
  // receiver attached to the co-processor: cannot stay on in standby
  bool viaCoprocessor() { return m_standalone && m_sys && m_sys->gpsUsesLink(); }
  // poll the receiver; true if a new plausible fix was accepted
  bool update();
  // called from the uplink task with data from the modem's GNSS
  void feedModem(const GPS_DATA& gd);
  // last accepted fix (valid if fix.ms != 0)
  const Fix& last() { return m_fix; }
  bool hasFix(uint32_t maxAgeMs) { return m_fix.ms && millis() - m_fix.ms < maxAgeMs; }
  uint32_t rejected() { return m_rejected; }
  uint16_t restarts() { return m_restarts; }
  // log a status line (called periodically while driving)
  void logStatus();
  // drop the plausibility anchor (e.g. after standby: the car may have been moved)
  void resetFilter() { m_candCount = 0; m_anchorValid = false; m_startMs = millis(); m_firstFixLogged = false; }
private:
  bool accept(const GPS_DATA& gd, uint32_t now);
  bool plausible(double lat, double lon, uint32_t ms, double refLat, double refLon, uint32_t refMs);
  FreematicsESP32* m_sys = nullptr;
  bool m_standalone = false;
  uint32_t m_lastData = 0;      // last sign of life of the receiver (NMEA flowing)
  uint32_t m_lastAliveCheck = 0;
  uint32_t m_startMs = 0;       // receiver (re)started, for time-to-first-fix
  bool m_firstFixLogged = false;
  uint32_t m_lastStatusLog = 0;
  uint16_t m_restarts = 0;
  uint8_t m_deadChecks = 0;
  bool m_hw = false;            // a standalone receiver was found
  bool m_ended = false;         // switched off on purpose (standby)
  uint32_t m_lastBeginTry = 0;
  bool linkAlive();
  void restart(const char* why);
  uint32_t m_lastGpsTime = 0xffffffff;
  uint16_t m_lastSentences = 0;
  Fix m_fix = {};
  bool m_anchorValid = false;
  // re-anchoring: consecutive mutually consistent fixes that failed the check
  double m_candLat = 0, m_candLon = 0;
  uint32_t m_candMs = 0;
  uint8_t m_candCount = 0;
  uint32_t m_rejected = 0;
  // modem GNSS hand-over
  SemaphoreHandle_t m_mutex = nullptr;
  GPS_DATA m_modem = {};
  bool m_modemNew = false;
};

extern Gnss gnss;
