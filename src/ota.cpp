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

static void check(bool manual)
{
  s_busy = true;
  s_progress = -1;
  s_lastCheck = millis();
  s_lastCheckTime = time(nullptr) > 1700000000 ? time(nullptr) : 0;

  char url[220];
  snprintf(url, sizeof(url), "%sfirmware-%s.json", cfg.updateUrl, fwEnv());
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
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) {
    setResult("Update-Info ungültig");
    s_busy = false;
    return;
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
  if (!fwCommitEpoch() && !manual) {
    // development build without git information: only update on request
    setResult("Build %u verfügbar (eigener Build, nur manuell)", (unsigned)build);
    s_busy = false;
    return;
  }
  setResult("installiere Build %u ...", (unsigned)build);
  if (install(binUrl, size, sha)) {
    setResult("Build %u installiert, Neustart", (unsigned)build);
    delay(500);
    systemRestart();
  }
  s_progress = -1;
  s_busy = false;
}

void otaService(bool wifiUp, bool ignition, bool standby)
{
  if (!cfg.updateUrl[0]) return;
  if (s_request) {
    s_request = false;
    if (!wifiUp) {
      setResult("Updates nur über WLAN (keine WLAN-Verbindung)");
      return;
    }
    check(true);
    return;
  }
  if (!cfg.autoUpdate || !wifiUp || ignition || standby) return;
  uint32_t now = millis();
  // first check 2 min after boot, then every 6 h
  uint32_t due = s_lastCheck ? OTA_CHECK_INTERVAL_MS : 120000UL;
  if (now - (s_lastCheck ? s_lastCheck : 0) < due) return;
  check(false);
}
