/******************************************************************************
 * Freematics ONE+ Telelogger (rewrite)
 *
 *  - GNSS position, OBD-II PIDs and CAN signals (broadcast + UDS requests)
 *  - OsmAnd protocol: works natively with Traccar and Home Assistant
 *  - WiFi and cellular (roaming aware), offline queue in RAM + flash
 *  - configuration and live view via built-in web UI
 *  - fast wake-up on engine start (voltage rise / motion), no reboot
 *
 * Distributed under BSD license (based on the Freematics telelogger by
 * Stanley Huang).
 ******************************************************************************/
#include <FreematicsPlus.h>
#include <math.h>
#include "config.h"
#include "settings.h"
#include "live.h"
#include "record.h"
#include "queue.h"
#include "gnss.h"
#include "vehicle.h"
#include "uplink.h"
#include "webui.h"

static FreematicsESP32 sys;
static MEMS_I2C* mems = nullptr;
static float accBias[3] = {0};

// state
static uint8_t state = PS_BOOT;
static bool ignition = false;
static uint32_t ignitionCandidateSince = 0;
static uint32_t ignitionOffSince = 0;
static uint32_t idleSince = 0;
static uint32_t lastMotion = 0;
static uint32_t lastRecord = 0;
static uint16_t lastRecordCourse = 0;
static uint32_t lastVoltageCheck = 0;
static float voltage = 0;
static int deviceTemp = 0;
static bool gnssHw = false;   // standalone receiver found at boot

static bool clockValid() { return time(nullptr) > 1700000000; }

static void led(bool on)
{
#ifdef PIN_LED
  digitalWrite(PIN_LED, on ? HIGH : LOW);
#endif
}

// ---------------------------------------------------------------------------
// motion sensor
// ---------------------------------------------------------------------------
static void memsInit()
{
  mems = new ICM_42627;
  if (mems->begin()) {
    Serial.println("[MEMS] ICM-42627");
    return;
  }
  delete mems;
  mems = new ICM_20948_I2C;
  if (mems->begin()) {
    Serial.println("[MEMS] ICM-20948");
    return;
  }
  delete mems;
  mems = nullptr;
  Serial.println("[MEMS] none");
}

static void memsCalibrate()
{
  if (!mems) return;
  float sum[3] = {0};
  int n = 0;
  for (uint32_t t = millis(); millis() - t < 500; ) {
    float acc[3];
    if (mems->read(acc)) {
      for (int i = 0; i < 3; i++) sum[i] += acc[i];
      n++;
    }
    delay(10);
  }
  if (n) for (int i = 0; i < 3; i++) accBias[i] = sum[i] / n;
}

// returns the deviation from the resting acceleration in g
static float memsMotion()
{
  if (!mems) return 0;
  float acc[3], gyr[3], mag[3], temp = 0;
  if (!mems->read(acc, gyr, mag, &temp)) return 0;
  if (temp > -40 && temp < 125) deviceTemp = (int)temp;
  float m = 0;
  for (int i = 0; i < 3; i++) {
    float d = acc[i] - accBias[i];
    m += d * d;
  }
  return sqrtf(m);
}

// ---------------------------------------------------------------------------
// records
// ---------------------------------------------------------------------------
static void pushRecord(uint8_t event)
{
  if (!cfg.serverUrl[0]) return;
  static Record r;
  memset(&r, 0, sizeof(r));
  const Fix& f = gnss.last();
  r.valid = gnss.hasFix(5000);
  if (f.ms) {
    r.lat = f.lat;
    r.lon = f.lon;
    r.alt = f.alt;
    r.course = f.course;
    r.sat = f.sat;
    r.hdop = f.hdop;
    r.speed = r.valid ? f.speed : 0;
  }
  r.ts = (r.valid && f.utc) ? f.utc : (clockValid() ? (uint32_t)time(nullptr) : 0);
  r.ignition = ignition;
  r.motion = (lastMotion && millis() - lastMotion < 10000) || r.speed > 5;
  r.power = voltage;
  r.event = event;
  liveLock();
  r.rssi = (int8_t)constrain(live.rssi, -127, 0);
  char vin[18];
  strlcpy(vin, live.vin, sizeof(vin));
  uint8_t dtcCount = live.dtcCount;
  uint16_t dtc[6];
  memcpy(dtc, live.dtc, sizeof(dtc));
  liveUnlock();

  uint32_t maxAge = max<uint32_t>(cfg.intervalDriving * 2000UL, 15000UL);
  vehicle.appendAttrs(r.attrs, sizeof(r.attrs), maxAge);
  size_t n = strlen(r.attrs);
  if (mems) n += snprintf(r.attrs + n, sizeof(r.attrs) - n, "&deviceTemp=%d", deviceTemp);
  if ((event == EV_IGNITION_ON || event == EV_BOOT) && vin[0] && n < sizeof(r.attrs)) {
    n += snprintf(r.attrs + n, sizeof(r.attrs) - n, "&vin=%s", vin);
  }
  if (event == EV_IGNITION_ON && dtcCount && n < sizeof(r.attrs)) {
    static const char sysc[] = {'P', 'C', 'B', 'U'};
    n += snprintf(r.attrs + n, sizeof(r.attrs) - n, "&dtcs=");
    for (int i = 0; i < dtcCount && i < 6 && n < sizeof(r.attrs); i++) {
      n += snprintf(r.attrs + n, sizeof(r.attrs) - n, "%s%c%04X", i ? "%20" : "", sysc[dtc[i] >> 14], dtc[i] & 0x3FFF);
    }
  }
  if (n >= sizeof(r.attrs)) r.attrs[sizeof(r.attrs) - 1] = 0;

  queue.push(r);
  lastRecord = millis();
  lastRecordCourse = r.course;
  led(true);
  delay(5);
  led(false);
}

// ---------------------------------------------------------------------------
// live data for the UI
// ---------------------------------------------------------------------------
static void publishLive()
{
  const Fix& f = gnss.last();
  liveLock();
  live.powerState = state;
  live.ignition = ignition;
  live.motion = lastMotion && millis() - lastMotion < 10000;
  live.voltage = voltage;
  live.deviceTemp = deviceTemp;
  live.gnssFix = gnss.hasFix(5000);
  if (f.ms) {
    live.lat = f.lat;
    live.lon = f.lon;
    live.alt = f.alt;
    live.speed = f.speed;
    live.course = f.course;
    live.sat = f.sat;
    live.hdop = f.hdop;
    live.gnssUtc = f.utc;
    live.gnssFixMs = f.ms;
  }
  live.gnssRejected = gnss.rejected();
  liveUnlock();
}

// ---------------------------------------------------------------------------
// ignition / voltage
// ---------------------------------------------------------------------------
static void checkVoltage()
{
  float v = vehicle.readVoltage();
  if (v > 5 && v < 40) voltage = v;
}

static bool ignitionCondition()
{
  if (voltage >= cfg.runVoltage) return true;
  if (vehicle.obdConnected()) return true;
  return false;
}

static void updateIgnition()
{
  uint32_t now = millis();
  bool cond = ignitionCondition();
  if (cond) {
    ignitionOffSince = 0;
    if (!ignition) {
      if (!ignitionCandidateSince) ignitionCandidateSince = now;
      // short debounce against voltage spikes
      if (now - ignitionCandidateSince >= 1500 || vehicle.obdConnected()) {
        ignition = true;
        ignitionCandidateSince = 0;
        state = PS_DRIVING;
        Serial.printf("[PWR] ignition ON (%.2f V)\n", voltage);
        pushRecord(EV_IGNITION_ON);
      }
    }
  } else {
    ignitionCandidateSince = 0;
    if (ignition) {
      if (!ignitionOffSince) ignitionOffSince = now;
      // voltage dips (start/stop systems) must not end the trip
      if (now - ignitionOffSince >= 15000) {
        ignition = false;
        ignitionOffSince = 0;
        state = PS_IDLE;
        idleSince = now;
        Serial.printf("[PWR] ignition OFF (%.2f V)\n", voltage);
        pushRecord(EV_IGNITION_OFF);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// standby
// ---------------------------------------------------------------------------
static void standby()
{
  Serial.println("[PWR] entering standby");
  state = PS_STANDBY;
  publishLive();
  uplinkSetStandby(true);
  vehicle.sleep();
  if (!cfg.gnssKeepOn && gnss.standalone()) gnss.end();
  memsCalibrate();

  uint32_t lastHeartbeat = millis();
  uint32_t lastVolt = 0;
  uint32_t baselineSince = millis();
  float baseline = voltage > 5 ? voltage : 0;
  bool wake = false;
  const char* reason = "";

  while (!wake) {
    delay(STANDBY_POLL_MS);
    uint32_t now = millis();

    // 1) motion
    if (mems && memsMotion() >= cfg.motionG) {
      wake = true;
      reason = "motion";
      break;
    }

    // 2) voltage: engine running or a clear rise above the parked level
    uint32_t vInterval = sys.devType > 12 ? 1000 : 10000; // ATRV wakes the co-processor
    if (now - lastVolt >= vInterval) {
      lastVolt = now;
      checkVoltage();
      if (voltage > 5) {
        if (!baseline || voltage < baseline) baseline = voltage;
        if (now - baselineSince > 1800000UL) { baseline = voltage; baselineSince = now; }
        if (voltage >= cfg.runVoltage) { wake = true; reason = "voltage"; break; }
        if (cfg.wakeDelta > 0 && voltage - baseline >= cfg.wakeDelta) { wake = true; reason = "voltage rise"; break; }
      }
    }

    // keep GNSS fix fresh (hot start) and send heartbeats
    gnss.update(); // standalone receiver or modem GNSS fed by the uplink task
    if (cfg.heartbeatMin && now - lastHeartbeat >= cfg.heartbeatMin * 60000UL) {
      lastHeartbeat = now;
      Serial.println("[PWR] heartbeat");
      pushRecord(EV_HEARTBEAT);
    }
    static uint32_t lastPub = 0;
    if (now - lastPub > 2000) { lastPub = now; publishLive(); }
  }

  Serial.printf("[PWR] wake up (%s, %.2f V)\n", reason, voltage);
  uplinkSetStandby(false);
  vehicle.wake();
  if (gnssHw && !gnss.standalone()) gnss.begin(&sys);
  gnss.resetFilter();
  state = PS_IDLE;
  idleSince = millis();
  lastMotion = millis();
  // report immediately: the server sees the car is awake within seconds
  pushRecord(EV_NONE);
}

// ---------------------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  liveMutex = xSemaphoreCreateMutex();
  memset(&live, 0, sizeof(live));
  live.bootMs = millis();
  Serial.printf("\nFreematics Telelogger %s\n", FW_VERSION);

#ifdef PIN_LED
  pinMode(PIN_LED, OUTPUT);
  led(true);
#endif

  bool loaded = settingsLoad();
  Serial.printf("[CFG] %s\n", loaded ? "loaded" : "defaults");
  queue.begin();

  if (sys.begin(true, true)) {
    Serial.printf("[SYS] co-processor type %u\n", sys.devType);
  } else {
    Serial.println("[SYS] no co-processor (OBD/CAN unavailable)");
  }
  liveLock();
  live.devType = sys.devType;
  liveUnlock();

  vehicle.begin(&sys);
  memsInit();
  memsCalibrate();

  bool gnssOk = gnssHw = gnss.begin(&sys);
  if (!gnssOk && cfg.gnssUseModem) uplinkUseModemGnss(true);
  liveLock();
  live.memsOk = mems != nullptr;
  live.gnssPresent = gnssOk || cfg.gnssUseModem;
  live.gnssFromModem = !gnssOk;
  liveUnlock();

  uplinkBegin(&sys);
  webBegin();
  Serial.printf("[SYS] device id %s\n", deviceId());

  checkVoltage();
  state = PS_IDLE;
  idleSince = millis();
  lastMotion = millis();
  led(false);
  pushRecord(EV_BOOT);
}

void loop()
{
  uint32_t now = millis();

  bool newFix = gnss.update();
  if (memsMotion() >= cfg.motionG) lastMotion = now;
  const Fix& f = gnss.last();
  if (newFix && f.speed > 5) lastMotion = now;

  if (now - lastVoltageCheck >= VOLTAGE_CHECK_MS) {
    lastVoltageCheck = now;
    checkVoltage();
  }
  updateIgnition();
  vehicle.update(ignition);

  // record scheduling
  bool moving = gnss.hasFix(5000) && f.speed > 10;
  uint32_t interval = (ignition || moving) ? cfg.intervalDriving * 1000UL : cfg.intervalIdle * 1000UL;
  bool due = now - lastRecord >= interval;
  if (!due && (ignition || moving) && cfg.headingChange && newFix && f.speed > 15 && now - lastRecord >= 1000) {
    int d = abs((int)f.course - (int)lastRecordCourse) % 360;
    if (d > 180) d = 360 - d;
    due = d >= cfg.headingChange;
  }
  if (due) pushRecord(EV_NONE);

  static uint32_t lastPub = 0;
  if (now - lastPub >= 500) {
    lastPub = now;
    publishLive();
  }

  // standby when parked long enough and nothing moves
  // never while somebody uses the web UI, and not within the first minutes after
  // power-up so the device can be configured in a parked car
  bool configWindow = now - live.bootMs < CONFIG_WINDOW_MS || uplinkApClients() > 0;
  if (!ignition && !moving && !configWindow && now - idleSince >= cfg.standbyDelay * 1000UL && now - lastMotion >= 30000) {
    standby();
    return;
  }
  if (ignition || moving) idleSince = now;
  delay(MAIN_TICK_MS);
}
