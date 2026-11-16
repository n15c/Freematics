// SIMCOM cellular modem driver (SIM7600, SIM7670, SIM7070/7080) on the
// Freematics ONE+ xBee socket. Roaming aware: registration is accepted on
// CS (CREG), PS (CGREG) or EPS/LTE (CEREG), home (1) or roaming (5).
#pragma once
#include <FreematicsPlus.h>
#include "settings.h"

enum ModemType : uint8_t { MODEM_UNKNOWN = 0, MODEM_SIM7600, MODEM_SIM7670, MODEM_SIM7070 };

class Modem {
public:
  // power on (if needed) and identify the module
  bool begin(FreematicsESP32* sys);
  void powerOff();
  bool present() { return m_type != MODEM_UNKNOWN; }
  bool alive();
  // radio on/off (AT+CFUN), keeps SIM and settings, much faster than a power cycle
  bool setRadio(bool on);
  // SIM check / PIN; returns false if the SIM is missing or locked
  bool prepareSim(const char* pin);
  // network mode, APN, automatic operator selection
  void configure(const Settings& s);
  // one registration poll; true if registered (home or roaming)
  bool pollRegistration();
  bool roaming() { return m_roaming; }
  // activate the data context; fills ip()
  bool attach(const Settings& s);
  bool checkAttached();
  void detach();
  // GET (body == nullptr) or POST form body; returns false on transport error
  bool http(const char* url, const char* body, int& status);
  int rssi();
  void updateNetInfo();
  bool syncClock();
  // modem GNSS
  bool gnssStart();
  void gnssStop();
  bool gnssPoll(GPS_DATA& gd);

  const char* model() { return m_model; }
  const char* imei() { return m_imei; }
  const char* iccid() { return m_iccid; }
  const char* ip() { return m_ip; }
  const char* op() { return m_op; }
  const char* netType() { return m_netType; }
  const char* regInfo() { return m_regInfo; }
  ModemType type() { return m_type; }
private:
  bool at(const char* cmd, uint32_t timeout = 1000, const char* expect = nullptr);
  bool waitFor(const char* token, uint32_t timeout);
  void completeLine(const char* token, uint32_t timeout = 500);
  int regStat(const char* cmd, const char* prefix);
  bool httpSimcomA(const char* url, const char* body, int& status);   // SIM7600/7670 AT+HTTP*
  bool httpSim7070(const char* url, const char* body, int& status);   // SIM7070 AT+SH*
  FreematicsESP32* m_sys = nullptr;
  ModemType m_type = MODEM_UNKNOWN;
  char m_buf[640];
  char m_model[16] = {0};
  char m_imei[16] = {0};
  char m_iccid[24] = {0};
  char m_ip[16] = {0};
  char m_op[32] = {0};
  char m_netType[16] = {0};
  char m_regInfo[48] = {0};
  bool m_roaming = false;
  bool m_httpInit = false;
  bool m_gnss = false;
  uint8_t m_gnssCmdSet = 0;
  // SIM7070 HTTP session
  char m_shBase[96] = {0};
  bool m_shConnected = false;
};
