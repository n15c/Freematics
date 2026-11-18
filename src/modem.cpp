#include "log.h"
#include "modem.h"
#include <sys/time.h>
#include "util.h"

#define DBG(...) Log.printf(__VA_ARGS__)

// ---------------------------------------------------------------------------
// low level
// ---------------------------------------------------------------------------
bool Modem::at(const char* cmd, uint32_t timeout, const char* expect)
{
  if (cmd) {
    m_sys->xbPurge();
    m_sys->xbWrite(cmd);
    m_sys->xbWrite("\r");
  }
  const char* answers[] = { expect ? expect : "\r\nOK", "\r\nERROR", "+CME ERROR" };
  m_buf[0] = 0;
  int r = m_sys->xbReceive(m_buf, sizeof(m_buf), timeout, answers, 3);
  return r == 1;
}

// wait for an unsolicited result (or a token in the current buffer)
bool Modem::waitFor(const char* token, uint32_t timeout)
{
  if (strstr(m_buf, token)) {
    completeLine(token);
    return true;
  }
  const char* answers[] = { token, "\r\nERROR", "+CME ERROR" };
  int r = m_sys->xbReceive(m_buf, sizeof(m_buf), timeout, answers, 3);
  if (r != 1) return false;
  completeLine(token);
  return true;
}

// make sure the line containing token has been received completely
void Modem::completeLine(const char* token, uint32_t timeout)
{
  uint32_t t = millis();
  for (;;) {
    char* p = strstr(m_buf, token);
    if (!p || strstr(p, "\r\n")) return;
    if (millis() - t > timeout) return;
    int len = strlen(m_buf);
    if (len >= (int)sizeof(m_buf) - 2) return;
    int n = m_sys->xbRead(m_buf + len, sizeof(m_buf) - len - 1, 50);
    if (n > 0) m_buf[len + n] = 0;
  }
}

static void copyField(char* dst, size_t size, const char* src, const char* stops)
{
  size_t i = 0;
  while (src[i] && !strchr(stops, src[i]) && i < size - 1) { dst[i] = src[i]; i++; }
  dst[i] = 0;
}

// ---------------------------------------------------------------------------
// power / identification
// ---------------------------------------------------------------------------
bool Modem::alive()
{
  for (int i = 0; i < 2; i++) if (at("AT", 300)) return true;
  return false;
}

bool Modem::begin(FreematicsESP32* sys)
{
  m_sys = sys;
  m_type = MODEM_UNKNOWN;
  m_httpInit = false;
  m_shConnected = false;
  m_gnss = false;
  bool on = alive();
  for (int n = 0; !on && n < 8; n++) {
    m_sys->xbTogglePower(200);
    // SIM7600 needs ~10 s to boot, SIM7070 ~5 s
    for (uint32_t t = millis(); millis() - t < 4000 && !on; ) on = alive();
  }
  if (!on) {
    DBG("[CELL] no modem found\n");
    return false;
  }
  at("ATE0");
  at("AT+CMEE=2");
  if (at("ATI", 1000)) {
    const char* p = strstr(m_buf, "Model:");
    if (p) {
      p += 6;
      while (*p == ' ') p++;
      const char* u = strchr(p, '_');
      if (u && u - p < 10) p = u + 1;
      copyField(m_model, sizeof(m_model), p, "\r\n ");
    } else if ((p = strstr(m_buf, "SIM7"))) {
      copyField(m_model, sizeof(m_model), p, "\r\n ");
    }
  }
  if (!m_model[0] && at("AT+SIMCOMATI", 1000)) {
    const char* p = strstr(m_buf, "SIM7");
    if (p) copyField(m_model, sizeof(m_model), p, "\r\n _");
  }
  if (strstr(m_model, "7070") || strstr(m_model, "7080") || strstr(m_model, "7000")) m_type = MODEM_SIM7070;
  else if (strstr(m_model, "7670")) m_type = MODEM_SIM7670;
  else m_type = MODEM_SIM7600;
  if (!m_model[0]) strcpy(m_model, "SIMCOM");

  if (at("AT+CGSN", 1000)) {
    for (char* p = m_buf; *p; p++) {
      if (isdigit((unsigned char)*p)) { copyField(m_imei, sizeof(m_imei), p, "\r\n "); break; }
    }
  }
  DBG("[CELL] %s IMEI:%s\n", m_model, m_imei);
  return true;
}

void Modem::powerOff()
{
  if (!m_sys) return;
  gnssStop();
  bool ok;
  if (m_type == MODEM_SIM7070) ok = at("AT+CPOWD=1", 3000, "NORMAL POWER DOWN");
  else ok = at("AT+CPOF", 3000);
  if (!ok) m_sys->xbTogglePower(2600);
  m_type = MODEM_UNKNOWN;
  m_httpInit = false;
  m_shConnected = false;
  m_ip[0] = 0;
  DBG("[CELL] powered off\n");
}

bool Modem::setRadio(bool on)
{
  m_httpInit = false;
  m_shConnected = false;
  if (!on) m_ip[0] = 0;
  return at(on ? "AT+CFUN=1" : "AT+CFUN=0", 10000);
}

bool Modem::prepareSim(const char* pin)
{
  bool ready = false;
  for (int n = 0; n < 10 && !ready; n++) {
    if (at("AT+CPIN?", 1000)) {
      if (strstr(m_buf, "READY")) ready = true;
      else if (strstr(m_buf, "SIM PIN") && pin && *pin) {
        char c[32];
        snprintf(c, sizeof(c), "AT+CPIN=\"%s\"", pin);
        at(c, 5000);
        pin = nullptr; // try the PIN only once to avoid locking the SIM
        delay(1000);
      } else if (strstr(m_buf, "SIM PUK")) {
        DBG("[CELL] SIM locked (PUK required)\n");
        return false;
      }
    }
    if (!ready) delay(500);
  }
  if (!ready) {
    DBG("[CELL] SIM not ready: %s\n", m_buf);
    return false;
  }
  if (at(m_type == MODEM_SIM7070 ? "AT+CCID" : "AT+CICCID", 1000)) {
    for (char* p = m_buf; *p; p++) {
      if (isdigit((unsigned char)*p)) { copyField(m_iccid, sizeof(m_iccid), p, "\r\n F"); break; }
    }
  }
  return true;
}

void Modem::configure(const Settings& s)
{
  char c[200];
  // the module keeps its state across ESP32 restarts: make sure the radio is on
  at("AT+CFUN=1", 10000);
  // network time & zone updates (used for the clock before GNSS has a fix)
  at("AT+CTZU=1");
  // registration URCs off, we poll
  at("AT+CREG=0");
  at("AT+CGREG=0");
  at("AT+CEREG=0");

  int cnmp = s.netMode == NET_LTE ? 38 : s.netMode == NET_GSM ? 13 : 2;
  snprintf(c, sizeof(c), "AT+CNMP=%d", cnmp);
  at(c, 3000);
  if (m_type == MODEM_SIM7070) at("AT+CMNB=3", 3000); // CAT-M and NB-IoT

  if (s.apn[0]) {
    snprintf(c, sizeof(c), "AT+CGDCONT=1,\"IP\",\"%s\"", s.apn);
    at(c, 3000);
    if (m_type != MODEM_SIM7070 && s.apnUser[0]) {
      snprintf(c, sizeof(c), "AT+CGAUTH=1,1,\"%s\",\"%s\"", s.apnPass, s.apnUser);
      at(c, 3000);
    }
  }
  // make sure the module selects operators automatically: a manually selected
  // (home) operator is not available abroad and would prevent registration
  if (at("AT+COPS?", 3000)) {
    const char* p = strstr(m_buf, "+COPS:");
    if (p && atoi(p + 6) != 0) {
      DBG("[CELL] switching to automatic operator selection\n");
      at("AT+COPS=0", 60000);
    }
  }
  at("AT+COPS=3,0"); // long alphanumeric operator names
}

int Modem::regStat(const char* cmd, const char* prefix)
{
  if (!at(cmd, 1000)) return -1;
  const char* p = strstr(m_buf, prefix);
  if (!p) return -1;
  p = strchr(p, ',');
  return p ? atoi(p + 1) : -1;
}

bool Modem::pollRegistration()
{
  int creg = regStat("AT+CREG?", "+CREG:");
  int cgreg = regStat("AT+CGREG?", "+CGREG:");
  int cereg = regStat("AT+CEREG?", "+CEREG:");
  snprintf(m_regInfo, sizeof(m_regInfo), "CREG:%d CGREG:%d CEREG:%d", creg, cgreg, cereg);
  bool reg = false;
  m_roaming = false;
  int st[] = {cereg, cgreg, creg};
  for (int v : st) {
    if (v == 1 || v == 5) {
      reg = true;
      if (v == 5) m_roaming = true;
    }
  }
  if (cereg == 3 || cgreg == 3 || creg == 3) DBG("[CELL] registration denied (%s)\n", m_regInfo);
  return reg;
}

bool Modem::checkAttached()
{
  if (m_type == MODEM_SIM7070) {
    if (!at("AT+CNACT?", 2000)) return false;
    const char* p = strstr(m_buf, "+CNACT: 0,1,\"");
    if (!p) return false;
    copyField(m_ip, sizeof(m_ip), p + 13, "\"");
  } else {
    if (!at("AT+CGPADDR=1", 2000)) return false;
    const char* p = strstr(m_buf, "+CGPADDR:");
    if (!p || !(p = strchr(p, ','))) return false;
    p++;
    if (*p == '"') p++;
    copyField(m_ip, sizeof(m_ip), p, "\"\r\n");
  }
  bool ok = m_ip[0] && strcmp(m_ip, "0.0.0.0") && m_ip[0] != '0';
  if (!ok) m_ip[0] = 0;
  return ok;
}

bool Modem::attach(const Settings& s)
{
  char c[200];
  if (m_type == MODEM_SIM7070) {
    if (s.apn[0]) {
      if (s.apnUser[0]) snprintf(c, sizeof(c), "AT+CNCFG=0,1,\"%s\",\"%s\",\"%s\",3", s.apn, s.apnUser, s.apnPass);
      else snprintf(c, sizeof(c), "AT+CNCFG=0,1,\"%s\"", s.apn);
      at(c, 3000);
    }
    if (checkAttached()) return true;
    at("AT+CNACT=0,1", 15000);
    for (int i = 0; i < 20; i++) {
      if (checkAttached()) return true;
      delay(500);
    }
    return false;
  }
  if (checkAttached()) return true;
  at("AT+CGATT=1", 30000);
  at("AT+CGACT=1,1", 30000);
  for (int i = 0; i < 20; i++) {
    if (checkAttached()) return true;
    delay(500);
  }
  return false;
}

void Modem::detach()
{
  m_httpInit = false;
  m_shConnected = false;
  if (m_type == MODEM_SIM7070) at("AT+CNACT=0,0", 5000);
  else at("AT+CGACT=0,1", 10000);
  m_ip[0] = 0;
}

int Modem::rssi()
{
  if (!at("AT+CSQ", 1000)) return 0;
  const char* p = strstr(m_buf, "+CSQ:");
  if (!p) return 0;
  int csq = atoi(p + 5);
  return (csq == 99 || csq < 0) ? 0 : csq * 2 - 113;
}

void Modem::updateNetInfo()
{
  if (at("AT+COPS?", 2000)) {
    const char* p = strstr(m_buf, ",\"");
    if (p) copyField(m_op, sizeof(m_op), p + 2, "\"");
    else m_op[0] = 0;
  }
  if (at("AT+CPSI?", 2000)) {
    const char* p = strstr(m_buf, "+CPSI:");
    if (p) {
      p += 6;
      while (*p == ' ') p++;
      copyField(m_netType, sizeof(m_netType), p, ",\r\n");
    }
  }
}

bool Modem::syncClock()
{
  if (!at("AT+CCLK?", 1000)) return false;
  const char* p = strstr(m_buf, "+CCLK: \"");
  if (!p) return false;
  p += 8;
  int yy, MM, dd, hh, mm, ss, tz = 0;
  char sign = '+';
  if (sscanf(p, "%d/%d/%d,%d:%d:%d%c%d", &yy, &MM, &dd, &hh, &mm, &ss, &sign, &tz) < 6) return false;
  if (yy < 24 || yy > 99) return false;
  uint32_t date = dd * 10000 + MM * 100 + yy;
  uint32_t tm = hh * 1000000 + mm * 10000 + ss * 100;
  uint32_t local = gnssToUnix(date, tm);
  if (!local) return false;
  int32_t offset = tz * 15 * 60 * (sign == '-' ? -1 : 1);
  struct timeval tv = { (time_t)(local - offset), 0 };
  settimeofday(&tv, nullptr);
  return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
bool Modem::http(const char* url, const char* body, int& status)
{
  status = 0;
  if (m_type == MODEM_SIM7070) return httpSim7070(url, body, status);
  return httpSimcomA(url, body, status);
}

bool Modem::httpSimcomA(const char* url, const char* body, int& status)
{
  bool https = !strncmp(url, "https", 5);
  if (!m_httpInit) {
    at("AT+HTTPTERM", 1000);
    if (!at("AT+HTTPINIT", 5000)) {
      DBG("[CELL] HTTPINIT failed: %s\n", m_buf);
      return false;
    }
    if (https) {
      at("AT+CSSLCFG=\"sslversion\",0,4");
      at("AT+CSSLCFG=\"authmode\",0,0");
      at("AT+CSSLCFG=\"enableSNI\",0,1");
      at("AT+HTTPPARA=\"SSLCFG\",0");
    }
    m_httpInit = true;
  }
  // URL (may be long, send in pieces to keep the stack small)
  m_sys->xbPurge();
  m_sys->xbWrite("AT+HTTPPARA=\"URL\",\"");
  m_sys->xbWrite(url);
  m_sys->xbWrite("\"\r");
  if (!at(nullptr, 2000)) {
    DBG("[CELL] HTTPPARA URL failed: %s\n", m_buf);
    m_httpInit = false;
    return false;
  }
  if (body) {
    char c[64];
    at("AT+HTTPPARA=\"CONTENT\",\"application/x-www-form-urlencoded\"");
    int len = strlen(body);
    snprintf(c, sizeof(c), "AT+HTTPDATA=%d,5000", len);
    if (!at(c, 3000, "DOWNLOAD")) {
      m_httpInit = false;
      return false;
    }
    m_sys->xbWrite(body, len);
    if (!at(nullptr, 6000)) {
      m_httpInit = false;
      return false;
    }
  }
  m_sys->xbPurge();
  m_sys->xbWrite(body ? "AT+HTTPACTION=1\r" : "AT+HTTPACTION=0\r");
  m_buf[0] = 0;
  if (!waitFor("+HTTPACTION:", HTTP_TIMEOUT_MS + 15000)) {
    DBG("[CELL] no HTTPACTION result: %s\n", m_buf);
    m_httpInit = false;
    return false;
  }
  const char* p = strstr(m_buf, "+HTTPACTION:");
  p = p ? strchr(p, ',') : nullptr;
  if (!p) return false;
  status = atoi(p + 1);
  // 6xx are module side network errors (DNS, connect, timeout ...)
  if (status >= 600) {
    DBG("[CELL] HTTP module error %d\n", status);
    m_httpInit = false;
    return false;
  }
  return true;
}

// open (or reuse) the SIM7070 HTTP session for the host of url; path = request path
bool Modem::shSession(const char* url, const char*& path)
{
  // split url into base (scheme://host:port) and path
  const char* hostStart = strstr(url, "://");
  if (!hostStart) return false;
  hostStart += 3;
  path = strchr(hostStart, '/');
  const char* q = strchr(hostStart, '?');
  if (!path || (q && q < path)) path = q;
  char base[96];
  int blen = path ? path - url : strlen(url);
  if (blen >= (int)sizeof(base)) return false;
  memcpy(base, url, blen);
  base[blen] = 0;
  bool https = !strncmp(url, "https", 5);

  if (m_shConnected && strcmp(base, m_shBase)) {
    at("AT+SHDISC", 3000);
    m_shConnected = false;
  }
  if (m_shConnected) {
    if (!at("AT+SHSTATE?", 1000) || !strstr(m_buf, "+SHSTATE: 1")) m_shConnected = false;
  }
  if (!m_shConnected) {
    char c[180];
    at("AT+SHDISC", 2000);
    if (https) {
      // host name for SNI
      char host[64];
      copyField(host, sizeof(host), base + (strstr(base, "://") - base) + 3, ":/");
      at("AT+CSSLCFG=\"sslversion\",1,3");
      at("AT+CSSLCFG=\"ignorertctime\",1,1");
      snprintf(c, sizeof(c), "AT+CSSLCFG=\"sni\",1,\"%s\"", host);
      at(c);
      at("AT+SHSSL=1,\"\"");
    }
    snprintf(c, sizeof(c), "AT+SHCONF=\"URL\",\"%s\"", base);
    if (!at(c, 2000)) return false;
    at("AT+SHCONF=\"BODYLEN\",1024");
    at("AT+SHCONF=\"HEADERLEN\",350");
    if (!at("AT+SHCONN", HTTP_TIMEOUT_MS)) {
      DBG("[CELL] SHCONN failed: %s\n", m_buf);
      return false;
    }
    strlcpy(m_shBase, base, sizeof(m_shBase));
    m_shConnected = true;
  }
  return true;
}

bool Modem::httpSim7070(const char* url, const char* body, int& status)
{
  const char* path = nullptr;
  if (!shSession(url, path)) return false;
  at("AT+SHCHEAD");
  at("AT+SHAHEAD=\"Connection\",\"keep-alive\"");
  if (body) {
    char c[64];
    at("AT+SHAHEAD=\"Content-Type\",\"application/x-www-form-urlencoded\"");
    int len = strlen(body);
    snprintf(c, sizeof(c), "AT+SHBOD=%d,10000", len);
    if (!at(c, 3000, ">")) return false;
    m_sys->xbWrite(body, len);
    if (!at(nullptr, 5000)) return false;
  }
  m_sys->xbPurge();
  m_sys->xbWrite("AT+SHREQ=\"");
  m_sys->xbWrite(path ? path : "/");
  m_sys->xbWrite(body ? "\",3\r" : "\",1\r");
  m_buf[0] = 0;
  if (!waitFor("+SHREQ:", HTTP_TIMEOUT_MS + 5000)) {
    m_shConnected = false;
    return false;
  }
  const char* p = strstr(m_buf, "+SHREQ:");
  p = p ? strchr(p, ',') : nullptr;
  if (!p) return false;
  status = atoi(p + 1);
  if (status >= 600) {
    m_shConnected = false;
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// binary downloads (firmware): ranged GET, body read in pieces
// ---------------------------------------------------------------------------
// read raw bytes until token appeared and the line after it is complete;
// returns offset of the first byte after that line in m_raw, -1 on timeout
int Modem::rawLine(const char* token, uint32_t timeout, int& value)
{
  m_rawLen = 0;
  uint32_t t = millis();
  int tl = strlen(token);
  while (millis() - t < timeout) {
    int n = m_sys->xbRead((char*)m_raw + m_rawLen, sizeof(m_raw) - m_rawLen, 20);
    if (n > 0) m_rawLen += n;
    for (int i = 0; i + tl <= m_rawLen; i++) {
      if (memcmp(m_raw + i, token, tl)) continue;
      int j = i + tl;
      int e = j;
      while (e + 1 < m_rawLen && !(m_raw[e] == '\r' && m_raw[e + 1] == '\n')) e++;
      if (e + 1 >= m_rawLen) break; // line not complete yet
      // value: last number on the line ("+HTTPREAD: DATA,1024" / "+SHREAD: 1024")
      value = 0;
      for (int k = j; k < e; k++) {
        if (isdigit(m_raw[k])) value = value * 10 + (m_raw[k] - '0');
        else value = 0;
      }
      return e + 2;
    }
    if (m_rawLen >= (int)sizeof(m_raw)) {
      // keep the tail (token may straddle)
      memmove(m_raw, m_raw + m_rawLen - 64, 64);
      m_rawLen = 64;
    }
  }
  return -1;
}

// after rawLine(): copy n data bytes (partly already in m_raw) into out
bool Modem::rawData(int offset, uint8_t* out, int n, uint32_t timeout)
{
  int have = m_rawLen - offset;
  if (have > n) have = n;
  if (have > 0) memcpy(out, m_raw + offset, have);
  int got = have > 0 ? have : 0;
  uint32_t t = millis();
  while (got < n && millis() - t < timeout) {
    int r = m_sys->xbRead((char*)out + got, n - got, 50);
    if (r > 0) { got += r; t = millis(); }
  }
  return got == n;
}

int Modem::httpGetBinary(const char* url, uint32_t from, uint32_t len, uint8_t* out, int maxLen, int& status)
{
  status = 0;
  char c[96];
  int bodyLen = -1;
  bool ranged = len > 0;
  if (m_type == MODEM_SIM7070) {
    const char* path = nullptr;
    if (!shSession(url, path)) return -1;
    at("AT+SHCHEAD");
    at("AT+SHAHEAD=\"Connection\",\"keep-alive\"");
    if (ranged) {
      snprintf(c, sizeof(c), "AT+SHAHEAD=\"Range\",\"bytes=%lu-%lu\"", (unsigned long)from, (unsigned long)(from + len - 1));
      at(c);
    }
    m_sys->xbPurge();
    m_sys->xbWrite("AT+SHREQ=\"");
    m_sys->xbWrite(path ? path : "/");
    m_sys->xbWrite("\",1\r");
    m_buf[0] = 0;
    if (!waitFor("+SHREQ:", HTTP_TIMEOUT_MS + 5000)) { m_shConnected = false; return -1; }
    const char* p = strstr(m_buf, "+SHREQ:");
    p = p ? strchr(p, ',') : nullptr;
    if (!p) return -1;
    status = atoi(p + 1);
    p = strchr(p + 1, ',');
    bodyLen = p ? atoi(p + 1) : 0;
  } else {
    if (!m_httpInit) {
      at("AT+HTTPTERM", 1000);
      if (!at("AT+HTTPINIT", 5000)) return -1;
      at("AT+CSSLCFG=\"sslversion\",0,4");
      at("AT+CSSLCFG=\"authmode\",0,0");
      at("AT+CSSLCFG=\"enableSNI\",0,1");
      at("AT+HTTPPARA=\"SSLCFG\",0");
      m_httpInit = true;
    }
    m_sys->xbPurge();
    m_sys->xbWrite("AT+HTTPPARA=\"URL\",\"");
    m_sys->xbWrite(url);
    m_sys->xbWrite("\"\r");
    if (!at(nullptr, 2000)) { m_httpInit = false; return -1; }
    if (ranged) snprintf(c, sizeof(c), "AT+HTTPPARA=\"USERDATA\",\"Range: bytes=%lu-%lu\"", (unsigned long)from, (unsigned long)(from + len - 1));
    else strcpy(c, "AT+HTTPPARA=\"USERDATA\",\"\"");
    at(c);
    m_sys->xbPurge();
    m_sys->xbWrite("AT+HTTPACTION=0\r");
    m_buf[0] = 0;
    bool ok = waitFor("+HTTPACTION:", HTTP_TIMEOUT_MS + 15000);
    if (ranged) at("AT+HTTPPARA=\"USERDATA\",\"\""); // do not leak into later requests
    if (!ok) { m_httpInit = false; return -1; }
    const char* p = strstr(m_buf, "+HTTPACTION:");
    p = p ? strchr(p, ',') : nullptr;
    if (!p) return -1;
    status = atoi(p + 1);
    p = strchr(p + 1, ',');
    bodyLen = p ? atoi(p + 1) : 0;
    if (status >= 600) { m_httpInit = false; return -1; }
  }
  if (status != 200 && status != 206) return -1;
  if (bodyLen > maxLen) bodyLen = maxLen;
  // read the body in pieces
  int pos = 0;
  while (pos < bodyLen) {
    int piece = bodyLen - pos > 1024 ? 1024 : bodyLen - pos;
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
      m_sys->xbPurge();
      if (m_type == MODEM_SIM7070) snprintf(c, sizeof(c), "AT+SHREAD=%d,%d\r", pos, piece);
      else snprintf(c, sizeof(c), "AT+HTTPREAD=%d,%d\r", pos, piece);
      m_sys->xbWrite(c);
      int n = 0;
      int off = rawLine(m_type == MODEM_SIM7070 ? "+SHREAD:" : "+HTTPREAD:", 5000, n);
      if (off < 0 || n <= 0) continue;
      if (n > piece) n = piece;
      ok = rawData(off, out + pos, n, 3000);
      if (ok) pos += n;
    }
    if (!ok) return -1;
  }
  return bodyLen;
}

// ---------------------------------------------------------------------------
// GNSS of the modem
// ---------------------------------------------------------------------------
static float nmeaDegree(const char* s)
{
  // ddmm.mmmmm / dddmm.mmmmm
  double v = atof(s);
  int deg = (int)(v / 100);
  return deg + (v - deg * 100) / 60.0;
}

bool Modem::gnssStart()
{
  if (m_gnss) return true;
  if (m_type == MODEM_SIM7070) {
    // note: SIM7070 cannot use GNSS and LTE at the same time
    if (at("AT+CGNSPWR=1", 2000)) { m_gnssCmdSet = 2; m_gnss = true; }
  } else {
    if (at("AT+CGPS=1,1", 3000) || at("AT+CGPS?", 1000, "+CGPS: 1")) { m_gnssCmdSet = 1; m_gnss = true; }
    else if (at("AT+CGNSSPWR=1", 3000)) { m_gnssCmdSet = 3; m_gnss = true; }
  }
  if (m_gnss) DBG("[CELL] modem GNSS on\n");
  return m_gnss;
}

void Modem::gnssStop()
{
  if (!m_gnss) return;
  if (m_gnssCmdSet == 2) at("AT+CGNSPWR=0");
  else if (m_gnssCmdSet == 3) at("AT+CGNSSPWR=0");
  else at("AT+CGPS=0", 3000);
  m_gnss = false;
}

bool Modem::gnssPoll(GPS_DATA& gd)
{
  if (!m_gnss) return false;
  memset(&gd, 0, sizeof(gd));
  char* f[20];
  int nf = 0;
  if (m_gnssCmdSet == 1) {
    // +CGPSINFO: lat,N,lon,E,ddmmyy,hhmmss.s,alt,speed(knots),course
    if (!at("AT+CGPSINFO", 1000)) return false;
    char* p = strstr(m_buf, "+CGPSINFO:");
    if (!p) return false;
    p += 10;
    while (*p == ' ') p++;
    char* e = strpbrk(p, "\r\n");
    if (e) *e = 0;
    for (char* t = p; t && nf < 20; ) { f[nf++] = t; t = strchr(t, ','); if (t) *t++ = 0; }
    if (nf < 9 || !*f[0]) return false;
    gd.lat = nmeaDegree(f[0]) * (*f[1] == 'S' ? -1 : 1);
    gd.lng = nmeaDegree(f[2]) * (*f[3] == 'W' ? -1 : 1);
    gd.date = atol(f[4]);
    gd.time = (uint32_t)(atof(f[5]) * 100);
    gd.alt = atof(f[6]);
    gd.speed = atof(f[7]);
    gd.heading = atoi(f[8]);
    gd.sat = 4;
  } else if (m_gnssCmdSet == 3) {
    // +CGNSSINFO: mode,gpsSV,gloSV,bdSV,lat,N,lon,E,ddmmyy,hhmmss.s,alt,speed(knots),course,pdop,hdop,vdop
    if (!at("AT+CGNSSINFO", 1000)) return false;
    char* p = strstr(m_buf, "+CGNSSINFO:");
    if (!p) return false;
    p += 11;
    while (*p == ' ') p++;
    char* e = strpbrk(p, "\r\n");
    if (e) *e = 0;
    for (char* t = p; t && nf < 20; ) { f[nf++] = t; t = strchr(t, ','); if (t) *t++ = 0; }
    if (nf < 15 || !*f[4]) return false;
    // newer firmware reports decimal degrees, older ddmm.mmmm
    double lat = atof(f[4]), lon = atof(f[6]);
    if (strchr(f[4], '.') && strchr(f[4], '.') - f[4] > 2) { lat = nmeaDegree(f[4]); lon = nmeaDegree(f[6]); }
    gd.lat = lat * (*f[5] == 'S' ? -1 : 1);
    gd.lng = lon * (*f[7] == 'W' ? -1 : 1);
    gd.date = atol(f[8]);
    gd.time = (uint32_t)(atof(f[9]) * 100);
    gd.alt = atof(f[10]);
    gd.speed = atof(f[11]);
    gd.heading = atoi(f[12]);
    gd.sat = atoi(f[1]) + atoi(f[2]) + atoi(f[3]);
    gd.hdop = (uint8_t)(atof(f[14]) * 10);
  } else {
    // +CGNSINF: run,fix,yyyyMMddhhmmss.sss,lat,lon,alt,speed(km/h),course,fixmode,,hdop,pdop,vdop,,satsView,satsUsed
    if (!at("AT+CGNSINF", 1000)) return false;
    char* p = strstr(m_buf, "+CGNSINF:");
    if (!p) return false;
    p += 9;
    while (*p == ' ') p++;
    char* e = strpbrk(p, "\r\n");
    if (e) *e = 0;
    for (char* t = p; t && nf < 20; ) { f[nf++] = t; t = strchr(t, ','); if (t) *t++ = 0; }
    if (nf < 16 || atoi(f[1]) != 1) return false;
    const char* ts = f[2];
    if (strlen(ts) < 14) return false;
    int yyyy = atoi(String(ts).substring(0, 4).c_str());
    int MM = atoi(String(ts).substring(4, 6).c_str());
    int dd = atoi(String(ts).substring(6, 8).c_str());
    gd.date = dd * 10000 + MM * 100 + (yyyy % 100);
    gd.time = atol(String(ts).substring(8, 14).c_str()) * 100;
    gd.lat = atof(f[3]);
    gd.lng = atof(f[4]);
    gd.alt = atof(f[5]);
    gd.speed = atof(f[6]) / 1.852f;
    gd.heading = atoi(f[7]);
    gd.hdop = (uint8_t)(atof(f[10]) * 10);
    gd.sat = atoi(f[15]);
  }
  gd.ts = millis();
  return gd.lat != 0 || gd.lng != 0;
}
