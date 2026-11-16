#include "vehicle.h"
#include "live.h"
#include "util.h"
#include <math.h>

Vehicle vehicle;

// PIDs where the library returns a raw value that needs scaling
static float pidScale(uint8_t pid)
{
  switch (pid) {
  case 0xA6: return 0.1f;   // odometer, 0.1 km
  default: return 1.0f;
  }
}

static bool isExt29(uint8_t proto)
{
  return proto == 0x7 || proto == 0x8 || proto == 0xB || proto == 0xD || proto == 0xF;
}

// ---------------------------------------------------------------------------

void Vehicle::begin(FreematicsESP32* sys)
{
  m_sys = sys;
  m_obd.begin(sys->link);
  m_seenMutex = xSemaphoreCreateMutex();
  for (int i = 0; i < MAX_OBD_PIDS; i++) { m_pidVal[i] = NAN; m_pidTs[i] = 0; m_pidRead[i] = 0; }
  for (int i = 0; i < MAX_CAN_SIGNALS; i++) { m_canVal[i] = NAN; m_canTs[i] = 0; m_canRead[i] = 0; }
}

int Vehicle::cmd(const char* c, char* buf, int bufsize, unsigned timeout)
{
  if (!m_obd.link) return 0;
  return m_obd.link->sendCommand(c, buf, bufsize, timeout);
}

float Vehicle::readVoltage()
{
  if (!m_sys) return 0;
  if (m_sys->devType > 12) {
    // newer units measure the OBD supply directly on the ESP32
    return (float)(analogRead(A0) * 45) / 4095;
  }
  return m_obd.getVoltage();
}

void Vehicle::sleep()
{
  if (!m_obd.link) return;
  m_ready = false;
  m_obdMode = false;
  m_obd.enterLowPowerMode();
  liveLock();
  live.obdConnected = false;
  liveUnlock();
}

void Vehicle::wake()
{
  if (!m_obd.link) return;
  m_obd.leaveLowPowerMode();
  m_lastTry = 0;
}

uint8_t Vehicle::queryProtocol()
{
  char buf[32];
  if (cmd("ATDPN\r", buf, sizeof(buf)) <= 0) return 0;
  // response like "A6" (auto, protocol 6) or "6"
  for (char* p = buf; *p; p++) {
    if (*p == 'A' && hexVal(p[1]) >= 0) return hexVal(p[1]);
    if (hexVal(*p) >= 0 && (p[1] == '\r' || p[1] == 0 || p[1] == '\n')) return hexVal(*p);
  }
  return 0;
}

bool Vehicle::rawInit(uint8_t proto)
{
  char buf[64];
  bool ok = false;
  for (int n = 0; n < 3 && !ok; n++) ok = cmd("ATZ\r", buf, sizeof(buf)) > 0;
  if (!ok) return false;
  cmd("ATE0\r", buf, sizeof(buf));
  cmd("ATH0\r", buf, sizeof(buf));
  char c[16];
  snprintf(c, sizeof(c), "ATSP %X\r", proto);
  return cmd(c, buf, sizeof(buf)) > 0 && strstr(buf, "OK");
}

bool Vehicle::initLink(bool allowAuto)
{
  bool ok = false;
  m_obdMode = false;
  if (cfg.obdEnabled) {
    uint8_t p = cfg.obdProtocol ? cfg.obdProtocol : nvsGetLastProtocol();
    if (p) {
      ok = m_obd.init((OBD_PROTOCOLS)p);
      if (ok) Serial.printf("[OBD] connected with protocol %X\n", p);
    }
    if (!ok && !cfg.obdProtocol && allowAuto) {
      char buf[16];
      cmd("ATSP 0\r", buf, sizeof(buf));
      ok = m_obd.init(PROTO_AUTO);
    }
    if (ok) {
      m_obdMode = true;
      m_proto = queryProtocol();
      if (!m_proto) m_proto = p;
      if (m_proto && !cfg.obdProtocol) nvsSetLastProtocol(m_proto);
      Serial.printf("[OBD] ECU on, protocol %X\n", m_proto);
    }
  }
  if (!ok && cfg.canEnabled && cfg.canCount) {
    ok = rawInit(cfg.canProtocol);
    if (ok) {
      m_proto = cfg.canProtocol;
      Serial.printf("[CAN] listening with protocol %X\n", m_proto);
    }
  }
  m_ext29 = isExt29(m_proto);
  return ok;
}

void Vehicle::readVinDtc()
{
  char buf[128];
  if (m_obd.getVIN(buf, sizeof(buf))) {
    liveLock();
    strlcpy(live.vin, buf, sizeof(live.vin));
    liveUnlock();
    Serial.printf("[OBD] VIN %s\n", buf);
  }
  uint16_t dtc[6] = {0};
  int n = m_obd.readDTC(dtc, 6);
  liveLock();
  live.dtcCount = n > 0 ? n : 0;
  memcpy(live.dtc, dtc, sizeof(dtc));
  liveUnlock();
}

void Vehicle::update(bool ignition)
{
  if (!m_obd.link || (!cfg.obdEnabled && !(cfg.canEnabled && cfg.canCount))) return;
  uint32_t now = millis();
  if (!m_ready) {
    uint32_t retry = ignition ? OBD_RETRY_MS : 15000;
    if (m_lastTry && now - m_lastTry < retry) return;
    m_lastTry = now;
    // the protocol auto search takes seconds: only with ignition on
    m_ready = initLink(ignition);
    liveLock();
    live.obdConnected = m_ready && m_obdMode;
    live.obdProtocol = m_proto;
    liveUnlock();
    if (!m_ready) return;
    m_vinRead = false;
    m_lastTry = now;
  }

  if (m_obdMode) {
    pollPids();
    if (!m_ready) return;
    if (!m_vinRead && millis() - m_lastTry > 10000) {
      // after the first records went out: VIN and DTCs (can take a moment)
      m_vinRead = true;
      readVinDtc();
    }
  }
  if (cfg.canEnabled) {
    pollCanRequests();
    sniffWindow();
  } else if (monitoring()) {
    sniffWindow();
  }
}

void Vehicle::pollPids()
{
  uint32_t now = millis();
  int reads = 0;
  for (int k = 0; k < cfg.pidCount && reads < 4; k++) {
    int i = (m_pidCursor + k) % cfg.pidCount;
    const ObdPidCfg& p = cfg.pids[i];
    uint32_t interval = p.interval ? p.interval * 1000UL : 1000;
    if (m_pidRead[i] && now - m_pidRead[i] < interval) continue;
    m_pidRead[i] = now;
    if (!m_obd.isValidPID(p.pid)) continue;
    int value;
    reads++;
    if (m_obd.readPID(p.pid, value)) {
      m_pidVal[i] = value * pidScale(p.pid);
      m_pidTs[i] = millis();
      liveSetValue(p.key, m_pidVal[i]);
    }
    if (m_obd.errors >= 5) {
      Serial.println("[OBD] ECU not responding");
      m_obd.errors = 0;
      m_ready = false;
      m_obdMode = false;
      m_lastTry = millis();
      liveLock();
      live.obdConnected = false;
      liveUnlock();
      return;
    }
  }
  if (cfg.pidCount) m_pidCursor = (m_pidCursor + 1) % cfg.pidCount;
}

void Vehicle::restoreHeader()
{
  char buf[32];
  if (m_ext29) {
    cmd("ATCP 18\r", buf, sizeof(buf));
    cmd("ATSH DB33F1\r", buf, sizeof(buf));
  } else {
    cmd("ATSH 7DF\r", buf, sizeof(buf));
  }
}

bool Vehicle::requestSignal(const CanSignalCfg& c, float& out)
{
  char buf[256];
  char req[32];
  int reqLen = strlen(c.request);
  if (reqLen < 2 || (reqLen & 1) || !isHexStr(c.request, reqLen)) return false;

  if (c.txId) {
    char hdr[24];
    if (c.txId > 0x7FF) {
      snprintf(hdr, sizeof(hdr), "ATCP %X\r", (unsigned)((c.txId >> 24) & 0x1F));
      cmd(hdr, buf, sizeof(buf));
      snprintf(hdr, sizeof(hdr), "ATSH %06X\r", (unsigned)(c.txId & 0xFFFFFF));
    } else {
      snprintf(hdr, sizeof(hdr), "ATSH %03X\r", (unsigned)c.txId);
    }
    cmd(hdr, buf, sizeof(buf));
  }
  snprintf(req, sizeof(req), "%s\r", c.request);
  int n = cmd(req, buf, sizeof(buf), 1500);
  if (n <= 0 || strstr(buf, "NO DATA") || strstr(buf, "ERROR") || strstr(buf, "UNABLE")) return false;

  uint8_t data[96];
  int dlen = 0;
  if (!parseRequestResponse(buf, c.request, data, sizeof(data), dlen)) return false;
  int64_t raw;
  if (!extractBits(data, dlen, c.startBit, c.bitLen, c.bigEndian, c.isSigned, raw)) return false;
  out = raw * c.factor + c.offset;
  return true;
}

void Vehicle::pollCanRequests()
{
  if (!m_ready) return;
  uint32_t now = millis();
  bool touched = false;
  for (int i = 0; i < cfg.canCount; i++) {
    const CanSignalCfg& c = cfg.can[i];
    if (c.mode != CAN_REQUEST) continue;
    uint32_t interval = c.interval ? c.interval * 1000UL : 2000;
    if (m_canRead[i] && now - m_canRead[i] < interval) continue;
    m_canRead[i] = now;
    touched = true;
    float v;
    if (requestSignal(c, v)) {
      m_canVal[i] = v;
      m_canTs[i] = millis();
      liveSetValue(c.key, v);
    }
  }
  if (touched) restoreHeader();
}

void Vehicle::handleFrame(uint32_t id, const uint8_t* data, uint8_t len)
{
  uint32_t now = millis();
  if (monitoring() && m_seenMutex) {
    xSemaphoreTake(m_seenMutex, portMAX_DELAY);
    int i;
    for (i = 0; i < m_seenCount && m_seen[i].id != id; i++);
    if (i == m_seenCount && m_seenCount < CAN_MONITOR_SLOTS) m_seenCount++;
    if (i < m_seenCount) {
      CanFrameSeen& s = m_seen[i];
      if (s.id != id) { s.id = id; s.count = 0; }
      s.len = len > 8 ? 8 : len;
      memcpy(s.data, data, s.len);
      s.count++;
      s.ts = now;
    }
    xSemaphoreGive(m_seenMutex);
  }
  for (int i = 0; i < cfg.canCount; i++) {
    const CanSignalCfg& c = cfg.can[i];
    if (c.mode != CAN_PASSIVE || c.id != id) continue;
    int64_t raw;
    if (extractBits(data, len, c.startBit, c.bitLen, c.bigEndian, c.isSigned, raw)) {
      m_canVal[i] = raw * c.factor + c.offset;
      m_canTs[i] = now;
      m_canRead[i] = now;
      liveSetValue(c.key, m_canVal[i]);
    }
  }
}

void Vehicle::sniffWindow()
{
  if (!m_ready) return;
  uint32_t now = millis();
  bool mon = monitoring();
  if (now - m_lastSniff < (mon ? 300 : 1000)) return;

  uint32_t full = m_ext29 ? 0x1FFFFFFF : 0x7FF;
  uint32_t filter = 0, mask = full;
  bool any = false;
  for (int i = 0; i < cfg.canCount; i++) {
    const CanSignalCfg& c = cfg.can[i];
    if (c.mode != CAN_PASSIVE) continue;
    uint32_t interval = c.interval ? c.interval * 1000UL : 1000;
    if (m_canRead[i] && now - m_canRead[i] < interval && !mon) continue;
    if (!any) { filter = c.id & full; any = true; }
    else { mask &= ~((c.id & full) ^ filter); filter &= mask; }
  }
  if (mon) { filter = 0; mask = 0; any = true; }
  if (!any) return;
  m_lastSniff = now;

  char buf[64];
  cmd("ATH1\r", buf, sizeof(buf));
  m_obd.setHeaderMask(mask);
  m_obd.setHeaderFilter(filter);
  m_obd.link->send("ATM1\r");

  char line[96];
  int ll = 0;
  uint32_t t0 = millis();
  uint32_t window = mon ? 400 : CAN_SNIFF_WINDOW_MS;
  while (millis() - t0 < window) {
    int c = m_obd.link->read();
    if (c < 0) continue;
    if (c == '\r' || c == '\n' || c == '>') {
      line[ll] = 0;
      uint32_t id;
      uint8_t data[8], len;
      if (ll && parseSniffLine(line, m_ext29, id, data, len) && len) handleFrame(id, data, len);
      ll = 0;
    } else if (ll < (int)sizeof(line) - 1) {
      line[ll++] = (char)c;
    }
  }
  m_obd.link->send("ATM0\r");
  // drain everything up to the prompt
  uint32_t quiet = millis();
  while (millis() - quiet < 60) {
    int c = m_obd.link->read();
    if (c >= 0) quiet = millis();
    if (c == '>') break;
  }
  cmd("ATH0\r", buf, sizeof(buf));
  // restore automatic receive filtering for normal OBD requests
  if (cmd("ATAR\r", buf, sizeof(buf)) <= 0 || !strstr(buf, "OK")) {
    // not supported: open the filter completely (responses are matched by content)
    m_obd.setHeaderMask(0);
    m_obd.setHeaderFilter(0);
  }
}

void Vehicle::appendAttrs(char* buf, size_t size, uint32_t maxAge)
{
  uint32_t now = millis();
  for (int i = 0; i < cfg.pidCount; i++) {
    if (m_pidTs[i] && now - m_pidTs[i] <= maxAge) appendKV(buf, size, cfg.pids[i].key, m_pidVal[i]);
  }
  for (int i = 0; i < cfg.canCount; i++) {
    if (m_canTs[i] && now - m_canTs[i] <= maxAge) appendKV(buf, size, cfg.can[i].key, m_canVal[i]);
  }
}

void Vehicle::startMonitor(uint16_t seconds)
{
  if (m_seenMutex) {
    xSemaphoreTake(m_seenMutex, portMAX_DELAY);
    m_seenCount = 0;
    xSemaphoreGive(m_seenMutex);
  }
  m_monitorUntil = millis() + seconds * 1000UL;
  if (!m_monitorUntil) m_monitorUntil = 1;
}

void Vehicle::monitorJson(JsonArray arr)
{
  if (!m_seenMutex) return;
  xSemaphoreTake(m_seenMutex, portMAX_DELAY);
  uint32_t now = millis();
  for (int i = 0; i < m_seenCount; i++) {
    const CanFrameSeen& s = m_seen[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"] = s.id;
    char hex[3 * 8 + 1];
    int n = 0;
    for (int k = 0; k < s.len; k++) n += snprintf(hex + n, sizeof(hex) - n, k ? " %02X" : "%02X", s.data[k]);
    hex[n] = 0;
    o["data"] = hex;
    o["count"] = s.count;
    o["age"] = now - s.ts;
  }
  xSemaphoreGive(m_seenMutex);
}
