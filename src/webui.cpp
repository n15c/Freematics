#include "webui.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <ArduinoJson.h>
#include "config.h"
#include "settings.h"
#include "live.h"
#include "queue.h"
#include "vehicle.h"
#include "uplink.h"
#include "webui_page.h"

static WebServer server(WEB_PORT);
static uint32_t s_restartAt = 0;
static bool s_otaOk = false;

static bool auth()
{
  if (!cfg.webPass[0]) return true;
  if (server.authenticate("admin", cfg.webPass)) return true;
  server.requestAuthentication(BASIC_AUTH, "Freematics");
  return false;
}

static void sendJson(JsonDocument& doc, int code = 200)
{
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", out);
}

static void handleRoot()
{
  if (!auth()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html", WEB_PAGE);
}

static void handleStatus()
{
  if (!auth()) return;
  static LiveData l;
  liveLock();
  l = live;
  liveUnlock();
  uint32_t now = millis();
  JsonDocument doc;
  doc["fw"] = FW_VERSION;
  doc["id"] = deviceId();
  doc["uptime"] = (now - l.bootMs) / 1000;
  doc["time"] = (uint32_t)time(nullptr);
  doc["heap"] = ESP.getFreeHeap();
  static const char* states[] = {"boot", "driving", "idle", "standby"};
  doc["state"] = states[l.powerState < 4 ? l.powerState : 0];
  doc["ignition"] = l.ignition;
  doc["motion"] = l.motion;
  doc["voltage"] = serialized(String(l.voltage, 2));
  doc["devType"] = l.devType;
  doc["mems"] = l.memsOk;
  doc["temp"] = l.deviceTemp;

  JsonObject v = doc["vehicle"].to<JsonObject>();
  v["obd"] = l.obdConnected;
  v["protocol"] = l.obdProtocol;
  v["vin"] = l.vin;
  JsonArray dtc = v["dtc"].to<JsonArray>();
  for (int i = 0; i < l.dtcCount && i < 6; i++) {
    char code[8];
    uint16_t d = l.dtc[i];
    static const char sys[] = {'P', 'C', 'B', 'U'};
    snprintf(code, sizeof(code), "%c%04X", sys[d >> 14], d & 0x3FFF);
    dtc.add(code);
  }
  JsonArray vals = v["values"].to<JsonArray>();
  for (int i = 0; i < l.valueCount; i++) {
    JsonObject o = vals.add<JsonObject>();
    o["key"] = l.values[i].key;
    o["value"] = l.values[i].value;
    o["age"] = (now - l.values[i].ts) / 1000;
  }

  JsonObject g = doc["gnss"].to<JsonObject>();
  g["present"] = l.gnssPresent;
  g["source"] = l.gnssFromModem ? "modem" : "receiver";
  g["fix"] = l.gnssFix;
  if (l.gnssFixMs) {
    g["lat"] = serialized(String(l.lat, 6));
    g["lon"] = serialized(String(l.lon, 6));
    g["alt"] = l.alt;
    g["speed"] = l.speed;
    g["course"] = l.course;
    g["sat"] = l.sat;
    g["hdop"] = l.hdop;
    g["age"] = (now - l.gnssFixMs) / 1000;
    g["utc"] = l.gnssUtc;
  }
  g["rejected"] = l.gnssRejected;

  JsonObject n = doc["net"].to<JsonObject>();
  static const char* links[] = {"none", "wifi", "cell"};
  n["link"] = links[l.link < 3 ? l.link : 0];
  n["wifi"] = l.wifiConnected;
  n["wifiIp"] = l.wifiIp;
  n["apIp"] = l.apIp;
  n["modem"] = l.modemPresent;
  n["model"] = l.modemModel;
  n["imei"] = l.imei;
  n["iccid"] = l.iccid;
  n["operator"] = l.op;
  n["netType"] = l.netType;
  n["cellIp"] = l.cellIp;
  n["registered"] = l.registered;
  n["roaming"] = l.roaming;
  n["rssi"] = l.rssi;
  n["sent"] = l.sent;
  n["failed"] = l.failed;
  n["lastCode"] = l.lastHttpCode;
  n["lastUpload"] = l.lastUploadMs ? (now - l.lastUploadMs) / 1000 : -1;
  n["error"] = l.lastError;
  n["queue"] = queue.size();
  n["queueFile"] = queue.fileCount();
  n["dropped"] = queue.dropped();
  sendJson(doc);
}

static void handleGetConfig()
{
  if (!auth()) return;
  JsonDocument doc;
  settingsToJson(cfg, doc.to<JsonObject>(), true);
  sendJson(doc);
}

static void handlePostConfig()
{
  if (!auth()) return;
  JsonDocument in;
  if (deserializeJson(in, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  // apply to a copy first, then save and restart so every module starts clean
  static Settings next;
  next = cfg;
  settingsFromJson(next, in.as<JsonObjectConst>());
  cfg = next;
  bool ok = settingsSave();
  JsonDocument doc;
  doc["ok"] = ok;
  doc["restart"] = ok;
  sendJson(doc, ok ? 200 : 500);
  if (ok) s_restartAt = millis() + 1500;
}

static void handleReset()
{
  if (!auth()) return;
  settingsDefaults(cfg);
  settingsSave();
  server.send(200, "application/json", "{\"ok\":true,\"restart\":true}");
  s_restartAt = millis() + 1500;
}

static void handleRestart()
{
  if (!auth()) return;
  server.send(200, "application/json", "{\"ok\":true}");
  s_restartAt = millis() + 1000;
}

static void handleQueueClear()
{
  if (!auth()) return;
  queue.clear();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleCanMonitor()
{
  if (!auth()) return;
  int sec = server.hasArg("seconds") ? server.arg("seconds").toInt() : 30;
  if (sec < 1) sec = 1;
  if (sec > 300) sec = 300;
  vehicle.startMonitor(sec);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleCanFrames()
{
  if (!auth()) return;
  JsonDocument doc;
  doc["active"] = vehicle.monitoring();
  vehicle.monitorJson(doc["frames"].to<JsonArray>());
  sendJson(doc);
}

static void handleUpdateDone()
{
  if (!auth()) return;
  server.sendHeader("Connection", "close");
  server.send(s_otaOk ? 200 : 500, "application/json", s_otaOk ? "{\"ok\":true}" : "{\"ok\":false}");
  if (s_otaOk) s_restartAt = millis() + 1500;
}

static void handleUpdateUpload()
{
  if (cfg.webPass[0] && !server.authenticate("admin", cfg.webPass)) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] %s\n", up.filename.c_str());
    s_otaOk = false;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    s_otaOk = Update.end(true);
    if (!s_otaOk) Update.printError(Serial);
    else Serial.printf("[OTA] done, %u bytes\n", up.totalSize);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
  }
}

static void webTask(void*)
{
  // the TCP/IP stack is brought up by the first WiFi.mode() call (uplink task)
  while (WiFi.getMode() == WIFI_OFF) delay(100);
  server.begin();
  for (;;) {
    server.handleClient();
    if (s_restartAt && (int32_t)(millis() - s_restartAt) > 0) systemRestart();
    delay(2);
  }
}

void webBegin()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/reset", HTTP_POST, handleReset);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/queue/clear", HTTP_POST, handleQueueClear);
  server.on("/api/can/monitor", HTTP_POST, handleCanMonitor);
  server.on("/api/can/frames", HTTP_GET, handleCanFrames);
  server.on("/api/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() {
    // captive-portal style: everything else goes to the UI
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 0);
}
