#include "log.h"
#include "gnss.h"
#include "config.h"
#include "util.h"
#include <sys/time.h>
#include <math.h>

Gnss gnss;

// Fixes implying more than this speed relative to the previous fix are suspicious.
static const double MAX_PLAUSIBLE_MPS = 90.0;   // 324 km/h
// After this long without an accepted fix, any fix is accepted (tunnel, car train, ferry, parking).
static const uint32_t ANCHOR_TIMEOUT_MS = 60000;
// Number of consecutive self-consistent fixes needed to override the anchor.
static const uint8_t REANCHOR_FIXES = 3;

bool Gnss::begin(FreematicsESP32* sys)
{
  m_sys = sys;
  if (!m_mutex) m_mutex = xSemaphoreCreateMutex();
  m_standalone = false;
  if (sys->gpsBeginExt()) {
    Log.println("[GNSS] external receiver");
    m_standalone = true;
  } else if (sys->gpsBegin()) {
    Log.println("[GNSS] internal receiver");
    m_standalone = true;
  } else {
    Log.println("[GNSS] no standalone receiver");
  }
  m_lastData = millis();
  return m_standalone;
}

void Gnss::end()
{
  if (m_standalone && m_sys) m_sys->gpsEnd(true);
  m_standalone = false;
}

void Gnss::feedModem(const GPS_DATA& gd)
{
  if (!m_mutex) return;
  xSemaphoreTake(m_mutex, portMAX_DELAY);
  m_modem = gd;
  m_modemNew = true;
  xSemaphoreGive(m_mutex);
}

bool Gnss::plausible(double lat, double lon, uint32_t ms, double refLat, double refLon, uint32_t refMs)
{
  double dt = (ms - refMs) / 1000.0;
  if (dt < 1) dt = 1;
  double d = distanceMeters(lat, lon, refLat, refLon);
  return d < 200 || d / dt <= MAX_PLAUSIBLE_MPS;
}

bool Gnss::accept(const GPS_DATA& gd, uint32_t now)
{
  if (gd.lat == 0 && gd.lng == 0) return false;
  if (fabs(gd.lat) > 90 || fabs(gd.lng) > 180) return false;
  // hdop is in units of 0.1
  if (gd.hdop && gd.hdop > 100 && gd.sat < 5) return false;

  double lat = gd.lat, lon = gd.lng;
  bool ok = true;
  if (m_anchorValid && now - m_fix.ms < ANCHOR_TIMEOUT_MS) {
    ok = plausible(lat, lon, now, m_fix.lat, m_fix.lon, m_fix.ms);
    if (!ok) {
      m_rejected++;
      // is this consistent with the previous rejected fix? then the anchor may be the outlier
      if (m_candCount && plausible(lat, lon, now, m_candLat, m_candLon, m_candMs)) {
        m_candCount++;
      } else {
        m_candCount = 1;
      }
      m_candLat = lat;
      m_candLon = lon;
      m_candMs = now;
      if (m_candCount >= REANCHOR_FIXES) {
        Log.println("[GNSS] re-anchoring position");
        ok = true;
      }
    }
  }
  if (!ok) return false;
  m_candCount = 0;
  m_anchorValid = true;

  m_fix.lat = lat;
  m_fix.lon = lon;
  m_fix.alt = gd.alt;
  m_fix.speed = gd.speed * 1.852f;
  m_fix.course = gd.heading;
  m_fix.sat = gd.sat;
  m_fix.hdop = gd.hdop / 10.0f;
  m_fix.ms = now;
  uint32_t utc = gnssToUnix(gd.date, gd.time);
  m_fix.utc = utc;
  if (utc) {
    // keep the system clock in sync (used for record timestamps)
    time_t sys = time(nullptr);
    if (labs((long)(sys - utc)) > 2) {
      struct timeval tv = { (time_t)utc, 0 };
      settimeofday(&tv, nullptr);
    }
  }
  return true;
}

bool Gnss::update()
{
  uint32_t now = millis();
  if (m_standalone) {
    GPS_DATA* gd = nullptr;
    bool good = m_sys->gpsGetData(&gd);
    if (gd && gd->sentences != m_lastSentences) {
      // receiver is alive (even without a fix, e.g. in a garage)
      m_lastSentences = gd->sentences;
      m_lastData = now;
    }
    if (good && gd) {
      m_lastData = now;
      if (gd->time != m_lastGpsTime) {
        m_lastGpsTime = gd->time;
        return accept(*gd, now);
      }
    } else if (now - m_lastData > GNSS_REINIT_MS) {
      Log.println("[GNSS] no data, restarting receiver");
      m_sys->gpsEnd(true);
      delay(50);
      begin(m_sys);
    }
    return false;
  }
  // modem GNSS
  if (!m_mutex) return false;
  GPS_DATA gd;
  bool fresh = false;
  xSemaphoreTake(m_mutex, portMAX_DELAY);
  if (m_modemNew) {
    gd = m_modem;
    m_modemNew = false;
    fresh = true;
  }
  xSemaphoreGive(m_mutex);
  return fresh && accept(gd, now);
}
