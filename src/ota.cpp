#include "ota.h"
#include "log.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "settings.h"
#include "version.h"
#include "uplink.h"
#include "live.h"
#include "modem.h"

static volatile bool s_request = false;
static volatile bool s_busy = false;
static uint32_t s_lastCheck = 0;
static uint32_t s_lastCheckTime = 0;   // unix time of the last check
static uint32_t s_availableBuild = 0;
static char s_result[96] = "noch nicht geprüft";
static int s_progress = -1;

void otaRequestCheck() { s_request = true; }
bool otaBusy() { return s_busy || s_request; }

static void setResult(const char* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_result, sizeof(s_result), fmt, ap);
  va_end(ap);
  Log.printf("[OTA] %s\n", s_result);
}

void otaStatusJson(JsonObject o)
{
  o["auto"] = cfg.autoUpdate;
  o["env"] = fwEnv();
  o["result"] = s_result;
  o["busy"] = (bool)s_busy;
  o["progress"] = s_progress;
  o["available"] = s_availableBuild;
  o["lastCheck"] = s_lastCheckTime;
}

static void beginHttp(HTTPClient& http, WiFiClientSecure& tls, const char* url)
{
  // NOTE: the server certificate is not validated (no CA bundle on the
  // device); the firmware image is verified with the SHA-256 from the manifest
  tls.setInsecure();
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setConnectTimeout(10000);
  http.setUserAgent("freematics-telelogger");
  http.begin(tls, url);
}

static bool hexEqual(const uint8_t* digest, const char* hex)
{
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    char b[3] = {hex[i * 2], hex[i * 2 + 1], 0};
    if ((uint8_t)strtoul(b, nullptr, 16) != digest[i]) return false;
  }
  return true;
}

static bool install(const char* url, int size, const char* sha)
{
  HTTPClient http;
  WiFiClientSecure tls;
  beginHttp(http, tls, url);
  int code = http.GET();
  if (code != 200) {
    setResult("Download fehlgeschlagen (HTTP %d)", code);
    http.end();
    return false;
  }
  int len = http.getSize();
  if (len <= 0) len = size;
  if (len <= 0 || (size > 0 && len != size)) {
    setResult("Download: unerwartete Grösse %d", len);
    http.end();
    return false;
  }
  if (!Update.begin(len)) {
    setResult("Update.begin: %s", Update.errorString());
    http.end();
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[2048];
  int done = 0;
  uint32_t lastData = millis();
  bool ok = true;
  while (done < len) {
    size_t avail = stream->available();
    if (!avail) {
      if (!stream->connected() || millis() - lastData > 20000) { ok = false; break; }
      delay(5);
      continue;
    }
    int n = stream->readBytes(buf, min(avail, sizeof(buf)));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update_ret(&ctx, buf, n);
    if (Update.write(buf, n) != (size_t)n) { ok = false; break; }
    done += n;
    s_progress = (int)((int64_t)done * 100 / len);
  }
  http.end();
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (!ok || done != len) {
    Update.abort();
    setResult("Download abgebrochen bei %d von %d Bytes", done, len);
    return false;
  }
  if (!hexEqual(digest, sha)) {
    Update.abort();
    setResult("Prüfsumme falsch, Update verworfen");
    return false;
  }
  if (!Update.end(true)) {
    setResult("Update.end: %s", Update.errorString());
    return false;
  }
  return true;
}

// cellular: download in byte ranges through the modem's HTTP stack
static bool installCell(Modem* modem, const char* url, int size, const char* sha)
{
  if (size <= 0) {
    setResult("Update-Info ohne Grösse");
    return false;
  }
  int chunk = 8192;
  uint8_t* buf = nullptr;
#ifdef BOARD_HAS_PSRAM
  buf = (uint8_t*)ps_malloc(32768);
  if (buf) chunk = 32768;
#endif
  if (!buf) buf = (uint8_t*)malloc(chunk);
  if (!buf) {
    setResult("kein Speicher für den Download");
    return false;
  }
  if (!Update.begin(size)) {
    setResult("Update.begin: %s", Update.errorString());
    free(buf);
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  int pos = 0;
  bool ok = true;
  uint32_t t0 = millis();
  while (pos < size && ok) {
    int want = size - pos > chunk ? chunk : size - pos;
    int got = -1, status = 0;
    for (int attempt = 0; attempt < 3 && got <= 0; attempt++) {
      got = modem->httpGetBinary(url, pos, want, buf, want, status);
      if (status == 301 || status == 302) {
        setResult("Weiterleitung (HTTP %d): Update-Quelle muss direkt erreichbar sein (raw.githubusercontent.com)", status);
        ok = false;
        break;
      }
    }
    if (!ok) break;
    if (got <= 0) {
      setResult("Download über Mobilfunk abgebrochen bei %d von %d Bytes (HTTP %d)", pos, size, status);
      ok = false;
      break;
    }
    // a server without range support sends the whole file: only accept the start
    if (status == 200 && pos > 0) {
      setResult("Server unterstützt keine Teil-Downloads");
      ok = false;
      break;
    }
    mbedtls_sha256_update_ret(&ctx, buf, got);
    if (Update.write(buf, got) != (size_t)got) {
      setResult("Update.write: %s", Update.errorString());
      ok = false;
      break;
    }
    pos += got;
    s_progress = (int)((int64_t)pos * 100 / size);
  }
  free(buf);
  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (!ok) {
    Update.abort();
    return false;
  }
  if (!hexEqual(digest, sha)) {
    Update.abort();
    setResult("Prüfsumme falsch, Update verworfen");
    return false;
  }
  if (!Update.end(true)) {
    setResult("Update.end: %s", Update.errorString());
    return false;
  }
  Log.printf("[OTA] %d bytes via cellular in %lus\n", size, (unsigned long)(millis() - t0) / 1000);
  return true;
}

static void check(bool manual, Modem* modem)
{
  s_busy = true;
  s_progress = -1;
  s_lastCheck = millis();
  s_lastCheckTime = time(nullptr) > 1700000000 ? time(nullptr) : 0;

  char url[220];
  snprintf(url, sizeof(url), "%sfirmware-%s.json", cfg.updateUrl, fwEnv());
  JsonDocument doc;
  if (modem) {
    static uint8_t mbuf[2048];
    int status = 0;
    int n = modem->httpGetBinary(url, 0, 0, mbuf, sizeof(mbuf) - 1, status);
    if (n <= 0) {
      setResult("keine Update-Info über Mobilfunk (HTTP %d)", status);
      s_busy = false;
      return;
    }
    mbuf[n] = 0;
    if (deserializeJson(doc, (const char*)mbuf)) {
      setResult("Update-Info ungültig");
      s_busy = false;
      return;
    }
  } else {
    HTTPClient http;
    WiFiClientSecure tls;
    beginHttp(http, tls, url);
    int code = http.GET();
    if (code != 200) {
      setResult("keine Update-Info (HTTP %d)", code);
      http.end();
      s_busy = false;
      return;
    }
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) {
      setResult("Update-Info ungültig");
      s_busy = false;
      return;
    }
  }
  uint32_t build = doc["build"] | 0;
  uint32_t epoch = doc["epoch"] | 0;
  const char* binUrl = doc["url"] | "";
  const char* sha = doc["sha256"] | "";
  const char* commit = doc["commit"] | "";
  int size = doc["size"] | 0;
  s_availableBuild = build;

  bool newer = epoch && epoch > fwCommitEpoch() && strncmp(commit, fwCommit(), 7) != 0;
  if (!newer) {
    setResult("aktuell (Build %u, verfügbar: Build %u)", (unsigned)fwBuild(), (unsigned)build);
    s_busy = false;
    return;
  }
  if ((!fwCommitEpoch() || strcmp(fwBranch(), "master")) && !manual) {
    // development build (other branch / no git info): only update on request
    setResult("Build %u verfügbar (Entwicklungs-Build läuft, nur manuell)", (unsigned)build);
    s_busy = false;
    return;
  }
  setResult("installiere Build %u über %s ...", (unsigned)build, modem ? "Mobilfunk" : "WLAN");
  bool ok = modem ? installCell(modem, binUrl, size, sha) : install(binUrl, size, sha);
  if (ok) {
    setResult("Build %u installiert, Neustart", (unsigned)build);
    delay(500);
    systemRestart();
  }
  s_progress = -1;
  s_busy = false;
}

void otaService(uint8_t link, Modem* modem, bool roaming, bool ignition, bool standby)
{
  if (!cfg.updateUrl[0]) return;
  bool wifi = link == LINK_WIFI;
  bool cell = link == LINK_CELL && modem && cfg.otaCellular && !roaming;
  if (s_request) {
    s_request = false;
    if (wifi) check(true, nullptr);
    else if (cell) check(true, modem);
    else if (link == LINK_CELL && roaming) setResult("keine Updates im Roaming");
    else if (link == LINK_CELL) setResult("Updates über Mobilfunk sind ausgeschaltet");
    else setResult("keine Verbindung");
    return;
  }
  if (!cfg.autoUpdate || (!wifi && !cell) || ignition || standby) return;
  uint32_t now = millis();
  // first check 2 min after boot, then every 6 h
  uint32_t due = s_lastCheck ? OTA_CHECK_INTERVAL_MS : 120000UL;
  if (now - (s_lastCheck ? s_lastCheck : 0) < due) return;
  check(false, wifi ? nullptr : modem);
}
