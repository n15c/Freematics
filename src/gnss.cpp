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
  m_ended = false;
  m_lastBeginTry = millis();
  if (sys->gpsBeginExt()) {
    Log.println("[GNSS] external receiver");
    m_standalone = true;
  } else if (sys->gpsBegin()) {
    Log.println("[GNSS] internal receiver");
    m_standalone = true;
  } else {
    Log.println("[GNSS] no standalone receiver");
  }
  if (m_standalone) m_hw = true;
  m_lastData = millis();
  m_lastAliveCheck = 0;
  m_deadChecks = 0;
  m_timeSeen = false;
  m_lastAliveTime = 0;
  m_startMs = millis();
  m_firstFixLogged = false;
  return m_standalone;
}

void Gnss::restart(const char* why)
{
  m_restarts++;
  Log.printf("[GNSS] %s, restarting receiver (#%u)\n", why, m_restarts);
  m_sys->gpsEnd(true);
  delay(50);
  begin(m_sys);
}

void Gnss::logStatus()
{
  if (!m_standalone && !m_fix.ms) return;
  uint32_t now = millis();
  if (m_fix.ms && now - m_fix.ms < 5000) {
    Log.printf("[GNSS] fix: %u sats, HDOP %.1f, %.0f km/h, rejected %u, restarts %u\n",
               m_fix.sat, m_fix.hdop, m_fix.speed, (unsigned)m_rejected, m_restarts);
  } else {
    Log.printf("[GNSS] NO FIX for %lus (receiver %s, last data %lus ago), restarts %u\n",
               (unsigned long)(now - (m_fix.ms > m_startMs ? m_fix.ms : m_startMs)) / 1000,
               m_standalone ? (viaCoprocessor() ? "co-processor" : "UART") : "modem",
               (unsigned long)(now - m_lastData) / 1000, m_restarts);
  }
}

void Gnss::end()
{
  if (m_standalone && m_sys) m_sys->gpsEnd(true);
  m_standalone = false;
  m_ended = true;
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
  // a receiver that failed to restart: keep trying (not when switched off on purpose)
  if (!m_standalone && m_hw && !m_ended && now - m_lastBeginTry > 30000) {
    Log.println("[GNSS] retrying receiver start");
    begin(m_sys);
  }
  if (m_standalone) {
    GPS_DATA* gd = nullptr;
    bool good = m_sys->gpsGetData(&gd);
    bool link = viaCoprocessor();
    // Restart only a receiver that delivers nothing at all (switched off, e.g.
    // by a co-processor reset). A receiver that is searching must keep
    // running: restarting it throws away the satellites found so far.
    // Counted in checks, not time: the main loop can be blocked for seconds
    // (OBD protocol search, VIN), which must not look like a dead receiver.
    //
    // UART receiver: the sentence counter must advance.
    // Co-processor receiver: ATGRR cannot be used for this (ATGPS consumes the
    // same buffer, so ATGRR is nearly always empty). Instead the receiver's
    // UTC time in the ATGPS answer must advance - but only once it has been
    // seen advancing: a receiver without any satellite yet (cold start,
    // garage) has no time and must not be restarted.
    if (now - m_lastAliveCheck >= 3000) {
      m_lastAliveCheck = now;
      bool alive;
      if (link) {
        uint32_t t = gd ? gd->time : 0;
        alive = t && t != m_lastAliveTime;
        if (alive && m_lastAliveTime) m_timeSeen = true;
        if (t) m_lastAliveTime = t;
        if (!m_timeSeen) alive = true;
      } else {
        alive = gd && gd->sentences != m_lastSentences;
        if (gd) m_lastSentences = gd->sentences;
      }
      if (alive) {
        m_lastData = now;
        m_deadChecks = 0;
      } else {
        m_deadChecks++;
      }
    }
    if (link) {
      // Diagnostics only: in a long tunnel the time may also stand still, and
      // a restart there would throw away the satellites. Log it so a receiver
      // switched off by the co-processor (e.g. after an OBD init) shows up.
      if (m_deadChecks == GNSS_DEAD_CHECKS_LINK) {
        Log.printf("[GNSS] co-processor receiver: time unchanged for %us (receiver stopped?)\n",
                   GNSS_DEAD_CHECKS_LINK * 3);
      }
    } else if (m_deadChecks >= GNSS_DEAD_CHECKS) {
      m_deadChecks = 0;
      restart("receiver silent");
      return false;
    }
    if (good && gd && gd->time != m_lastGpsTime) {
      m_lastGpsTime = gd->time;
      bool ok = accept(*gd, now);
      if (ok && !m_firstFixLogged) {
        m_firstFixLogged = true;
        Log.printf("[GNSS] first fix after %lus (%u sats, HDOP %.1f)\n",
                   (unsigned long)(now - m_startMs) / 1000, m_fix.sat, m_fix.hdop);
      }
      return ok;
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
