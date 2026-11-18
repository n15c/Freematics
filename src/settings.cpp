#include "settings.h"
#include <Preferences.h>

Settings cfg;

static const char* NVS_NS = "telelogger";
static const char* SECRET_MASK = "********";

static void setPid(ObdPidCfg& p, uint8_t pid, uint16_t interval, const char* key)
{
  p.pid = pid;
  p.interval = interval;
  strlcpy(p.key, key, sizeof(p.key));
}

void settingsDefaults(Settings& s)
{
  memset(&s, 0, sizeof(s));
  // no default server: nothing is uploaded until a server is configured in the web UI
  s.httpMethod = HTTP_GET_QUERY;
  s.intervalDriving = 5;
  s.intervalIdle = 60;
  s.heartbeatMin = 30;
  s.headingChange = 30;
  s.autoUpdate = true;
  strlcpy(s.updateUrl, OTA_DEFAULT_URL, sizeof(s.updateUrl));

  s.apMode = AP_ALWAYS;
  strlcpy(s.apPass, "freematics", sizeof(s.apPass));

  s.cellEnabled = true;
  s.netMode = NET_AUTO;
  s.modemStandby = MODEM_STANDBY_SLEEP;

  s.gnssKeepOn = true;
  s.gnssKeepMin = 120;
  s.gnssUseModem = true;

  s.obdEnabled = true;
  s.obdProtocol = 0;
  // attribute names follow Traccar's standard position keys where one exists
  setPid(s.pids[s.pidCount++], 0x0D, 0, "obdSpeed");
  setPid(s.pids[s.pidCount++], 0x0C, 0, "rpm");
  setPid(s.pids[s.pidCount++], 0x04, 0, "engineLoad");
  setPid(s.pids[s.pidCount++], 0x11, 0, "throttle");
  setPid(s.pids[s.pidCount++], 0x05, 10, "coolantTemp");
  setPid(s.pids[s.pidCount++], 0x0F, 30, "intakeTemp");
  setPid(s.pids[s.pidCount++], 0x2F, 60, "fuel");
  setPid(s.pids[s.pidCount++], 0x46, 60, "ambientTemp");
  setPid(s.pids[s.pidCount++], 0xA6, 60, "obdOdometer");

  s.canEnabled = false;
  s.canProtocol = 6;

  s.runVoltage = 13.2f;
  s.wakeDelta = 0.4f;
  s.motionG = 0.15f;
  s.lowVoltage = 11.8f;
  s.standbyDelay = 120;
}

// ---------------------------------------------------------------------------
// JSON mapping
// ---------------------------------------------------------------------------
static void putSecret(JsonObject o, const char* key, const char* v, bool hide)
{
  o[key] = (hide && *v) ? SECRET_MASK : v;
}

void settingsToJson(const Settings& s, JsonObject o, bool hide)
{
  o["deviceId"] = s.deviceId;
  o["serverUrl"] = s.serverUrl;
  o["liveUrl"] = s.liveUrl;
  o["httpMethod"] = s.httpMethod;
  o["intervalDriving"] = s.intervalDriving;
  o["intervalIdle"] = s.intervalIdle;
  o["heartbeatMin"] = s.heartbeatMin;
  o["headingChange"] = s.headingChange;
  o["autoUpdate"] = s.autoUpdate;
  o["updateUrl"] = s.updateUrl;

  o["wifiSsid"] = s.wifiSsid;
  putSecret(o, "wifiPass", s.wifiPass, hide);
  o["apSsid"] = s.apSsid;
  putSecret(o, "apPass", s.apPass, hide);
  o["apMode"] = s.apMode;
  putSecret(o, "webPass", s.webPass, hide);

  o["cellEnabled"] = s.cellEnabled;
  o["apn"] = s.apn;
  o["apnUser"] = s.apnUser;
  putSecret(o, "apnPass", s.apnPass, hide);
  putSecret(o, "simPin", s.simPin, hide);
  o["netMode"] = s.netMode;
  o["modemStandby"] = s.modemStandby;

  o["gnssKeepOn"] = s.gnssKeepOn;
  o["gnssKeepMin"] = s.gnssKeepMin;
  o["gnssUseModem"] = s.gnssUseModem;

  o["obdEnabled"] = s.obdEnabled;
  o["obdProtocol"] = s.obdProtocol;
  JsonArray pids = o["pids"].to<JsonArray>();
  for (int i = 0; i < s.pidCount; i++) {
    JsonObject p = pids.add<JsonObject>();
    p["pid"] = s.pids[i].pid;
    p["interval"] = s.pids[i].interval;
    p["key"] = s.pids[i].key;
  }

  o["canEnabled"] = s.canEnabled;
  o["canProtocol"] = s.canProtocol;
  JsonArray can = o["can"].to<JsonArray>();
  for (int i = 0; i < s.canCount; i++) {
    const CanSignalCfg& c = s.can[i];
    JsonObject j = can.add<JsonObject>();
    j["key"] = c.key;
    j["mode"] = c.mode;
    j["id"] = c.id;
    j["txId"] = c.txId;
    j["request"] = c.request;
    j["startBit"] = c.startBit;
    j["bitLen"] = c.bitLen;
    j["bigEndian"] = c.bigEndian;
    j["signed"] = c.isSigned;
    j["factor"] = c.factor;
    j["offset"] = c.offset;
    j["interval"] = c.interval;
  }

  o["runVoltage"] = s.runVoltage;
  o["wakeDelta"] = s.wakeDelta;
  o["motionG"] = s.motionG;
  o["lowVoltage"] = s.lowVoltage;
  o["standbyDelay"] = s.standbyDelay;
}

template <size_t N>
static void getStr(JsonObjectConst o, const char* key, char (&dst)[N], bool secret = false)
{
  JsonVariantConst v = o[key];
  if (v.isNull()) return;
  const char* str = v.as<const char*>();
  if (!str) return;
  if (secret && !strcmp(str, SECRET_MASK)) return; // unchanged
  strlcpy(dst, str, N);
}

template <typename T>
static void getNum(JsonObjectConst o, const char* key, T& dst)
{
  JsonVariantConst v = o[key];
  if (!v.isNull()) dst = v.as<T>();
}

void settingsFromJson(Settings& s, JsonObjectConst o)
{
  getStr(o, "deviceId", s.deviceId);
  getStr(o, "serverUrl", s.serverUrl);
  getStr(o, "liveUrl", s.liveUrl);
  getNum(o, "httpMethod", s.httpMethod);
  getNum(o, "intervalDriving", s.intervalDriving);
  getNum(o, "intervalIdle", s.intervalIdle);
  getNum(o, "heartbeatMin", s.heartbeatMin);
  getNum(o, "headingChange", s.headingChange);
  getNum(o, "autoUpdate", s.autoUpdate);
  getStr(o, "updateUrl", s.updateUrl);

  getStr(o, "wifiSsid", s.wifiSsid);
  getStr(o, "wifiPass", s.wifiPass, true);
  getStr(o, "apSsid", s.apSsid);
  getStr(o, "apPass", s.apPass, true);
  getNum(o, "apMode", s.apMode);
  getStr(o, "webPass", s.webPass, true);

  getNum(o, "cellEnabled", s.cellEnabled);
  getStr(o, "apn", s.apn);
  getStr(o, "apnUser", s.apnUser);
  getStr(o, "apnPass", s.apnPass, true);
  getStr(o, "simPin", s.simPin, true);
  getNum(o, "netMode", s.netMode);
  getNum(o, "modemStandby", s.modemStandby);

  getNum(o, "gnssKeepOn", s.gnssKeepOn);
  getNum(o, "gnssKeepMin", s.gnssKeepMin);
  getNum(o, "gnssUseModem", s.gnssUseModem);

  getNum(o, "obdEnabled", s.obdEnabled);
  getNum(o, "obdProtocol", s.obdProtocol);
  JsonArrayConst pids = o["pids"];
  if (!pids.isNull()) {
    s.pidCount = 0;
    for (JsonObjectConst p : pids) {
      if (s.pidCount >= MAX_OBD_PIDS) break;
      ObdPidCfg& d = s.pids[s.pidCount];
      memset(&d, 0, sizeof(d));
      d.pid = p["pid"] | 0;
      d.interval = p["interval"] | 0;
      strlcpy(d.key, p["key"] | "", sizeof(d.key));
      if (d.pid && d.key[0]) s.pidCount++;
    }
  }

  getNum(o, "canEnabled", s.canEnabled);
  getNum(o, "canProtocol", s.canProtocol);
  JsonArrayConst can = o["can"];
  if (!can.isNull()) {
    s.canCount = 0;
    for (JsonObjectConst j : can) {
      if (s.canCount >= MAX_CAN_SIGNALS) break;
      CanSignalCfg& c = s.can[s.canCount];
      memset(&c, 0, sizeof(c));
      strlcpy(c.key, j["key"] | "", sizeof(c.key));
      c.mode = j["mode"] | 0;
      c.id = j["id"] | 0;
      c.txId = j["txId"] | 0;
      strlcpy(c.request, j["request"] | "", sizeof(c.request));
      c.startBit = j["startBit"] | 0;
      c.bitLen = j["bitLen"] | 8;
      if (c.bitLen < 1) c.bitLen = 1;
      if (c.bitLen > 32) c.bitLen = 32;
      c.bigEndian = j["bigEndian"] | false;
      c.isSigned = j["signed"] | false;
      c.factor = j["factor"] | 1.0f;
      c.offset = j["offset"] | 0.0f;
      c.interval = j["interval"] | 0;
      if (c.key[0]) s.canCount++;
    }
  }

  getNum(o, "runVoltage", s.runVoltage);
  getNum(o, "wakeDelta", s.wakeDelta);
  getNum(o, "motionG", s.motionG);
  getNum(o, "lowVoltage", s.lowVoltage);
  getNum(o, "standbyDelay", s.standbyDelay);

  // sanity limits
  if (s.intervalDriving < 1) s.intervalDriving = 1;
  if (s.intervalIdle < 5) s.intervalIdle = 5;
  if (s.standbyDelay < 10) s.standbyDelay = 10;
  if (s.motionG < 0.03f) s.motionG = 0.03f;
  if (s.apPass[0] && strlen(s.apPass) < 8) s.apPass[0] = 0; // WPA2 needs >= 8 chars
}

// ---------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------
bool settingsLoad()
{
  settingsDefaults(cfg);
  Preferences p;
  if (!p.begin(NVS_NS, true)) return false;
  String json = p.getString("cfg", "");
  p.end();
  if (!json.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  settingsFromJson(cfg, doc.as<JsonObjectConst>());
  return true;
}

bool settingsSave()
{
  JsonDocument doc;
  settingsToJson(cfg, doc.to<JsonObject>(), false);
  String json;
  serializeJson(doc, json);
  Preferences p;
  if (!p.begin(NVS_NS, false)) return false;
  bool ok = p.putString("cfg", json) == json.length();
  p.end();
  return ok;
}

uint8_t nvsGetLastProtocol()
{
  Preferences p;
  if (!p.begin(NVS_NS, true)) return 0;
  uint8_t v = p.getUChar("obdproto", 0);
  p.end();
  return v;
}

void nvsSetLastProtocol(uint8_t proto)
{
  Preferences p;
  if (!p.begin(NVS_NS, false)) return;
  if (p.getUChar("obdproto", 0) != proto) p.putUChar("obdproto", proto);
  p.end();
}
