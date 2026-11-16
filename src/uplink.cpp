#include "uplink.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include "config.h"
#include "settings.h"
#include "live.h"
#include "queue.h"
#include "osmand.h"
#include "modem.h"
#include "gnss.h"

enum CellState : uint8_t { CS_DISABLED = 0, CS_OFF, CS_SLEEP, CS_SEARCH, CS_ATTACH, CS_READY };

static FreematicsESP32* s_sys = nullptr;
static Modem modem;
static volatile bool s_standby = false;
static volatile bool s_modemGnss = false;
static char s_deviceId[32];

static CellState s_cell = CS_OFF;
static uint32_t s_cellT0 = 0;
static uint32_t s_cellRetryAt = 0;
static uint32_t s_lastPowerCycle = 0;
static uint8_t s_attachFails = 0;
static uint8_t s_sendFails = 0;
static uint32_t s_wifiUpSince = 0;
static bool s_wifiOn = false;
static bool s_apOn = false;
static bool s_mdns = false;
static bool s_ntp = false;

static char s_query[RECORD_ATTR_LEN + 420];
static char s_url[RECORD_ATTR_LEN + 600];

const char* deviceId()
{
  return cfg.deviceId[0] ? cfg.deviceId : s_deviceId;
}

void uplinkSetStandby(bool standby) { s_standby = standby; }
void uplinkUseModemGnss(bool on) { s_modemGnss = on; }

void systemRestart()
{
  queue.persist();
  delay(200);
  ESP.restart();
}

static bool clockValid() { return time(nullptr) > 1700000000; }

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifiApply(bool awake, bool wantSta)
{
  bool sta = wantSta && cfg.wifiSsid[0];
  bool staUp = sta && WiFi.status() == WL_CONNECTED;
  bool ap = awake && (cfg.apMode == AP_ALWAYS || (cfg.apMode == AP_NO_STA && !staUp));
  if (!sta && !ap) {
    if (s_wifiOn) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      s_wifiOn = s_apOn = false;
      s_wifiUpSince = 0;
      Serial.println("[WIFI] off");
    }
    return;
  }
  wifi_mode_t mode = sta && ap ? WIFI_AP_STA : sta ? WIFI_STA : WIFI_AP;
  if (!s_wifiOn || WiFi.getMode() != mode) {
    WiFi.mode(mode);
    s_wifiOn = true;
    if (sta) {
      WiFi.setAutoReconnect(true);
      WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
      Serial.printf("[WIFI] joining %s\n", cfg.wifiSsid);
    }
  }
  if (ap && !s_apOn) {
    char ssid[33];
    if (cfg.apSsid[0]) strlcpy(ssid, cfg.apSsid, sizeof(ssid));
    else snprintf(ssid, sizeof(ssid), "FREEMATICS-%s", deviceId() + strlen(deviceId()) - 4);
    WiFi.softAP(ssid, cfg.apPass[0] ? cfg.apPass : nullptr);
    s_apOn = true;
    Serial.printf("[WIFI] AP %s at %s\n", ssid, WiFi.softAPIP().toString().c_str());
    if (!s_mdns && MDNS.begin(MDNS_NAME)) {
      MDNS.addService("http", "tcp", WEB_PORT);
      s_mdns = true;
    }
  } else if (!ap && s_apOn) {
    WiFi.softAPdisconnect(true);
    s_apOn = false;
  }
}

static bool wifiStaUp()
{
  if (!cfg.wifiSsid[0] || !s_wifiOn) return false;
  bool up = WiFi.status() == WL_CONNECTED;
  if (up && !s_wifiUpSince) {
    s_wifiUpSince = millis();
    Serial.printf("[WIFI] connected, IP %s\n", WiFi.localIP().toString().c_str());
    if (!s_ntp) {
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      s_ntp = true;
    }
  } else if (!up && s_wifiUpSince) {
    s_wifiUpSince = 0;
    Serial.println("[WIFI] disconnected");
  }
  return up;
}

// ---------------------------------------------------------------------------
// cellular state machine (non blocking steps)
// ---------------------------------------------------------------------------
static void cellRecover(const char* why)
{
  liveSetError("cell: %s (%s)", why, modem.regInfo());
  uint32_t now = millis();
  if (!s_lastPowerCycle || now - s_lastPowerCycle > MODEM_MIN_POWERCYCLE_MS) {
    s_lastPowerCycle = now;
    modem.powerOff();
    s_cell = CS_OFF;
    s_cellRetryAt = now + 3000;
  } else {
    // soft recovery: radio off/on triggers a fresh network search
    modem.setRadio(false);
    delay(2000);
    modem.setRadio(true);
    s_cell = CS_SEARCH;
    s_cellT0 = millis();
  }
}

static void cellService()
{
  uint32_t now = millis();
  switch (s_cell) {
  case CS_DISABLED:
    return;
  case CS_OFF:
    if ((int32_t)(now - s_cellRetryAt) < 0) return;
    if (!modem.begin(s_sys)) {
      s_cellRetryAt = now + 60000;
      liveSetError("no cellular module");
      return;
    }
    liveLock();
    live.modemPresent = true;
    strlcpy(live.modemModel, modem.model(), sizeof(live.modemModel));
    strlcpy(live.imei, modem.imei(), sizeof(live.imei));
    liveUnlock();
    if (!modem.prepareSim(cfg.simPin)) {
      liveSetError("SIM card missing or locked");
      modem.powerOff();
      s_cellRetryAt = now + 300000;
      return;
    }
    liveLock();
    strlcpy(live.iccid, modem.iccid(), sizeof(live.iccid));
    liveUnlock();
    modem.configure(cfg);
    if (s_lastPowerCycle == 0) s_lastPowerCycle = now;
    s_cell = CS_SEARCH;
    s_cellT0 = millis();
    Serial.println("[CELL] searching network");
    break;
  case CS_SLEEP:
    modem.setRadio(true);
    s_cell = CS_SEARCH;
    s_cellT0 = millis();
    Serial.println("[CELL] radio on");
    break;
  case CS_SEARCH: {
    static uint32_t lastPoll = 0;
    if (now - lastPoll < 1000) return;
    lastPoll = now;
    bool reg = modem.pollRegistration();
    liveLock();
    live.registered = reg;
    live.roaming = modem.roaming();
    liveUnlock();
    if (reg) {
      Serial.printf("[CELL] registered%s (%s) after %lus\n", modem.roaming() ? " ROAMING" : "",
                    modem.regInfo(), (unsigned long)(now - s_cellT0) / 1000);
      s_cell = CS_ATTACH;
      s_attachFails = 0;
    } else if (now - s_cellT0 > 120000) {
      cellRecover("no network registration");
    }
    break;
  }
  case CS_ATTACH:
    if (modem.attach(cfg)) {
      s_cell = CS_READY;
      s_sendFails = 0;
      modem.updateNetInfo();
      if (!clockValid()) modem.syncClock();
      int rssi = modem.rssi();
      liveLock();
      strlcpy(live.cellIp, modem.ip(), sizeof(live.cellIp));
      strlcpy(live.op, modem.op(), sizeof(live.op));
      strlcpy(live.netType, modem.netType(), sizeof(live.netType));
      live.rssi = rssi;
      liveUnlock();
      Serial.printf("[CELL] online %s %s IP %s RSSI %d\n", modem.op(), modem.netType(), modem.ip(), rssi);
    } else if (++s_attachFails >= 3) {
      cellRecover("data connection failed (check APN)");
    } else {
      modem.detach();
      delay(1000);
    }
    break;
  case CS_READY:
    break;
  }
}

static void cellSleep()
{
  if (s_cell == CS_DISABLED || s_cell == CS_OFF) return;
  if (cfg.modemStandby == MODEM_STANDBY_ONLINE) return;
  if (s_modemGnss && cfg.modemStandby == MODEM_STANDBY_SLEEP && cfg.gnssKeepOn) {
    // keep the module on for GNSS, radio off
    modem.setRadio(false);
    s_cell = CS_SLEEP;
  } else if (cfg.modemStandby == MODEM_STANDBY_SLEEP) {
    modem.setRadio(false);
    s_cell = CS_SLEEP;
    Serial.println("[CELL] radio off (standby)");
  } else {
    modem.powerOff();
    s_cell = CS_OFF;
    s_cellRetryAt = 0;
  }
  liveLock();
  live.registered = false;
  live.cellIp[0] = 0;
  liveUnlock();
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
static bool httpSend(uint8_t link, const char* url, const char* body, int& code)
{
  code = 0;
  if (link == LINK_WIFI) {
    HTTPClient http;
    WiFiClient plain;
    WiFiClientSecure secure;
    bool https = !strncmp(url, "https", 5);
    if (https) secure.setInsecure();
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setConnectTimeout(5000);
    if (!http.begin(https ? (WiFiClient&)secure : plain, url)) return false;
    if (body) {
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      code = http.POST((uint8_t*)body, strlen(body));
    } else {
      code = http.GET();
    }
    http.end();
    return code > 0;
  }
  return modem.http(url, body, code);
}

static bool sendRecord(const Record& r, uint8_t link, const char* base, int& code)
{
  if (!osmandQuery(r, deviceId(), s_query, sizeof(s_query))) return false;
  bool post = cfg.httpMethod == HTTP_POST_FORM;
  if (!osmandUrl(base, s_query, post, s_url, sizeof(s_url))) return false;
  return httpSend(link, s_url, post ? s_query : nullptr, code);
}

// ---------------------------------------------------------------------------
// task
// ---------------------------------------------------------------------------
static void periodic(uint8_t link)
{
  static uint32_t lastInfo = 0, lastGnss = 0;
  uint32_t now = millis();
  if (s_modemGnss && cfg.gnssUseModem && modem.present() && now - lastGnss >= 1000) {
    lastGnss = now;
    if (modem.gnssStart()) {
      GPS_DATA gd;
      if (modem.gnssPoll(gd)) gnss.feedModem(gd);
    }
  }
  if (now - lastInfo >= 30000) {
    lastInfo = now;
    int rssi = 0;
    if (link == LINK_WIFI) rssi = WiFi.RSSI();
    else if (s_cell == CS_READY) {
      rssi = modem.rssi();
      modem.updateNetInfo();
      bool reg = modem.pollRegistration();
      liveLock();
      strlcpy(live.op, modem.op(), sizeof(live.op));
      strlcpy(live.netType, modem.netType(), sizeof(live.netType));
      live.roaming = modem.roaming();
      live.registered = reg;
      liveUnlock();
    }
    liveLock();
    live.rssi = rssi;
    liveUnlock();
  }
}

static void uplinkTask(void*)
{
  bool lastStandby = false;
  uint32_t standbySince = 0;
  uint32_t backoffUntil = 0;
  Record rec;

  wifiApply(true, true);
  for (;;) {
    uint32_t now = millis();
    bool standby = s_standby;
    if (standby != lastStandby) {
      lastStandby = standby;
      standbySince = now;
      if (!standby) {
        Serial.println("[NET] wake up");
        wifiApply(true, true);
      }
    }
    uint32_t qsize = queue.size();
    bool pending = qsize > 0 && cfg.serverUrl[0];

    // parked: only bring the link up to flush the queue / send heartbeats,
    // and give up after 5 minutes (no coverage) until a new record arrives
    static uint32_t tryStart = 0, gaveUpAt = 0;
    if (standby && pending) {
      if (gaveUpAt && qsize == gaveUpAt) pending = false;
      else if (!tryStart) tryStart = now;
      else if (now - tryStart > 300000UL) {
        Serial.println("[NET] standby: no connectivity, giving up until next heartbeat");
        gaveUpAt = qsize;
        tryStart = 0;
        pending = false;
      }
    } else {
      tryStart = 0;
      if (!standby) gaveUpAt = 0;
    }
    if (standby && !pending) {
      if (s_cell == CS_READY || s_cell == CS_SEARCH || s_cell == CS_ATTACH) cellSleep();
      wifiApply(false, false);
      if (s_modemGnss) periodic(LINK_NONE);
      delay(STANDBY_POLL_MS);
      continue;
    }
    if (standby) wifiApply(false, true); // station only, for heartbeats / flushing

    bool wifiUp = wifiStaUp();
    if (!standby) wifiApply(true, true); // AP mode may depend on station state
    uint8_t link = LINK_NONE;
    if (wifiUp) {
      link = LINK_WIFI;
      // WiFi stable: cellular radio not needed
      if (s_cell == CS_READY && now - s_wifiUpSince > 30000 && !standby) cellSleep();
    } else if (cfg.cellEnabled) {
      // in standby give WiFi a short chance first
      if (!(standby && cfg.wifiSsid[0] && now - tryStart < 8000 && s_wifiUpSince == 0 && s_cell != CS_READY)) {
        cellService();
        if (s_cell == CS_READY) link = LINK_CELL;
      }
    }
    // modem GNSS needs the module powered even when WiFi carries the data
    if (s_modemGnss && cfg.gnssUseModem && cfg.cellEnabled && s_cell == CS_OFF && !standby) cellService();
    if (!cfg.cellEnabled && s_cell != CS_DISABLED && s_cell != CS_OFF) {
      modem.powerOff();
      s_cell = CS_OFF;
    }

    liveLock();
    live.link = link;
    live.wifiConnected = wifiUp;
    strlcpy(live.wifiIp, wifiUp ? WiFi.localIP().toString().c_str() : "", sizeof(live.wifiIp));
    strlcpy(live.apIp, s_apOn ? WiFi.softAPIP().toString().c_str() : "", sizeof(live.apIp));
    liveUnlock();

    periodic(link);

    if (link == LINK_NONE || !pending || (int32_t)(now - backoffUntil) < 0) {
      delay(100);
      continue;
    }
    if (!queue.peek(rec)) {
      delay(50);
      continue;
    }

    int code = 0;
    bool ok = sendRecord(rec, link, cfg.serverUrl, code);
    liveLock();
    live.lastHttpCode = code;
    liveUnlock();
    if (ok && code >= 200 && code < 300) {
      queue.pop();
      s_sendFails = 0;
      liveLock();
      live.sent++;
      live.lastUploadMs = millis();
      live.lastError[0] = 0;
      liveUnlock();
      Serial.printf("[NET] sent via %s (%d), queue %u\n", link == LINK_WIFI ? "WiFi" : "cell", code, queue.size());
      // newest point also to the optional live target (e.g. Home Assistant webhook)
      if (cfg.liveUrl[0] && queue.size() == 0) {
        int c2;
        if (!sendRecord(rec, link, cfg.liveUrl, c2) || c2 < 200 || c2 >= 300) {
          Serial.printf("[NET] live target failed (%d)\n", c2);
        }
      }
    } else if (ok && code >= 400 && code < 500 && code != 408 && code != 429) {
      // rejected by the server: retrying will not help, drop it
      queue.pop();
      liveLock();
      live.failed++;
      liveUnlock();
      liveSetError("server rejected record: HTTP %d (device id '%s' registered?)", code, deviceId());
    } else {
      s_sendFails++;
      liveLock();
      live.failed++;
      liveUnlock();
      liveSetError("upload failed via %s (code %d)", link == LINK_WIFI ? "WiFi" : "cell", code);
      uint32_t wait = 1000UL << (s_sendFails > 6 ? 6 : s_sendFails);
      backoffUntil = millis() + (wait > 60000 ? 60000 : wait);
      if (link == LINK_CELL && s_sendFails >= 3) {
        if (!modem.alive()) {
          cellRecover("modem not responding");
        } else if (!modem.pollRegistration()) {
          s_cell = CS_SEARCH;
          s_cellT0 = millis();
        } else if (!modem.checkAttached()) {
          s_cell = CS_ATTACH;
        }
        s_sendFails = 0;
      }
    }
  }
}

void uplinkBegin(FreematicsESP32* sys)
{
  s_sys = sys;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(s_deviceId, sizeof(s_deviceId), "FM%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  s_cell = cfg.cellEnabled ? CS_OFF : CS_DISABLED;
  WiFi.persistent(false);
  xTaskCreatePinnedToCore(uplinkTask, "uplink", 10240, nullptr, 2, nullptr, 0);
}
