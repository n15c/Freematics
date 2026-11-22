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
#include "log.h"
#include "config.h"
#include "settings.h"
#include "live.h"
#include "record.h"
#include "queue.h"
#include "gnss.h"
#include "vehicle.h"
#include "uplink.h"
#include "webui.h"
#include "version.h"
#include "ota.h"
#include <esp_ota_ops.h>
#include <esp_sleep.h>

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
static bool gnssHw = false;      // standalone receiver found at boot
static bool powerOnBoot = false; // plugged in (not a software restart)

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
    Log.println("[MEMS] ICM-42627");
    return;
  }
  delete mems;
  mems = new ICM_20948_I2C;
  if (mems->begin()) {
    Log.println("[MEMS] ICM-20948");
    return;
  }
  delete mems;
  mems = nullptr;
  Log.println("[MEMS] none");
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
static void pushRecord(uint8_t event, const char* extra = nullptr)
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
  // speed from the vehicle (more accurate, also without GNSS fix); the GNSS
  // speed stays available as attribute gpsSpeed for comparison
  float obdKmh;
  bool speedObd = cfg.speedFromObd && vehicle.obdSpeed(obdKmh, 3000);
  if (speedObd) r.speed = obdKmh;
  r.ts = (r.valid && f.utc) ? f.utc : (clockValid() ? (uint32_t)time(nullptr) : 0);
  r.ignition = ignition;
  r.motion = (lastMotion && millis() - lastMotion < 10000) || r.speed > 5;
  r.power = voltage;
  r.event = event;
  if (extra) strlcpy(r.attrs, extra, sizeof(r.attrs));
  if (speedObd && r.valid) {
    size_t l = strlen(r.attrs);
    snprintf(r.attrs + l, sizeof(r.attrs) - l, "&gpsSpeed=%.1f", f.speed);
  }
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
  // engine running although the voltage is low (smart alternator)
  if (vehicle.engineRunning()) return true;
  // no voltage measurement available at all: fall back to "ECU answers"
  if (voltage <= 0 && vehicle.obdConnected()) return true;
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
      if (now - ignitionCandidateSince >= 1500 || vehicle.engineRunning()) {
        ignition = true;
        ignitionCandidateSince = 0;
        state = PS_DRIVING;
        Log.printf("[PWR] ignition ON (%.2f V)\n", voltage);
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
        Log.printf("[PWR] ignition OFF (%.2f V)\n", voltage);
        pushRecord(EV_IGNITION_OFF);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// standby
// ---------------------------------------------------------------------------
// reason why the device stays awake, nullptr = standby now
static const char* standbyBlocker(uint32_t now, bool moving)
{
  static char buf[64];
  if (ignition) return "ignition on";
  if (moving) return "vehicle moving (GNSS)";
  if (now - lastMotion < 30000) return "motion detected (MEMS)";
  if (webBusy() || otaBusy()) return "firmware update";
  if (webRecentlyUsed(WEB_ACTIVE_MS)) return "web UI in use";
  if (powerOnBoot && now - live.bootMs < CONFIG_WINDOW_MS) {
    snprintf(buf, sizeof(buf), "configuration window after power-up (%lus left)",
             (unsigned long)(CONFIG_WINDOW_MS - (now - live.bootMs)) / 1000);
    return buf;
  }
  uint32_t idle = now - idleSince;
  if (idle < cfg.standbyDelay * 1000UL) {
    snprintf(buf, sizeof(buf), "standby delay (%lus left)", (unsigned long)(cfg.standbyDelay * 1000UL - idle) / 1000);
    return buf;
  }
  return nullptr;
}
static void standby()
{
  Log.println("[PWR] entering standby");
  state = PS_STANDBY;
  publishLive();
  uplinkSetStandby(true);
  // Keeping the receiver powered means a hot start (fix within seconds) when
  // the car is started again. A receiver behind the co-processor needs the
  // co-processor awake: it then only stops OBD traffic instead of sleeping.
  bool keepGnss = gnss.standalone() && cfg.gnssKeepOn && cfg.gnssKeepMin > 0;
  bool coprocAwake = keepGnss && gnss.viaCoprocessor();
  if (gnss.standalone() && !keepGnss) gnss.end();
  if (coprocAwake) vehicle.idle(); else vehicle.sleep();
  memsCalibrate();

  uint32_t standbyStart = millis();
  uint32_t lastHeartbeat = millis();
  uint32_t lastVolt = 0;
  uint32_t lowSince = 0;
  bool saver = false;
  uint8_t voltHits = 0;
  float baseline = voltage > 5 ? voltage : 0;
  bool wake = false;
  const char* reason = "";

  while (!wake) {
    // light sleep (ESP32 ~1 mA instead of ~30 mA) when nothing else runs;
    // not with the standalone GNSS on, its software UART needs the CPU
    // (the co-processor receiver buffers itself, light sleep is fine there)
    if (uplinkIdle() && (!gnss.standalone() || gnss.viaCoprocessor())) {
      Serial.flush();
      esp_sleep_enable_timer_wakeup(STANDBY_POLL_MS * 1000ULL);
      esp_light_sleep_start();
    } else {
      delay(STANDBY_POLL_MS);
    }
    uint32_t now = millis();

    // 1) motion: two consecutive samples (400 ms) above the threshold, a
    // single spike (door, passing truck, sensor noise) does not wake up
    static uint8_t motionHits = 0;
    if (mems && memsMotion() >= cfg.motionG) {
      if (++motionHits >= 2) {
        motionHits = 0;
        wake = true;
        reason = "motion";
        break;
      }
    } else {
      motionHits = 0;
    }

    // 2) voltage: engine running or a clear rise above the parked level
    uint32_t vInterval = sys.devType > 12 ? 1000 : 10000; // ATRV wakes the co-processor
    if (now - lastVolt >= vInterval) {
      lastVolt = now;
      checkVoltage();
      if (voltage > 5) {
        // baseline follows the parked voltage slowly (a minimum would lock onto
        // a single low outlier and every normal reading would look like a rise)
        if (!baseline) baseline = voltage;
        bool high = voltage >= cfg.runVoltage;
        bool rise = cfg.wakeDelta > 0 && voltage - baseline >= cfg.wakeDelta;
        if (high || rise) {
          // must hold for 3 readings in a row
          if (++voltHits >= 3) {
            wake = true;
            reason = high ? "voltage" : "voltage rise";
            break;
          }
        } else {
          voltHits = 0;
          baseline += (voltage - baseline) * 0.05f;
        }
        // battery protection
        if (!saver && cfg.lowVoltage > 0 && voltage < cfg.lowVoltage) {
          if (!lowSince) lowSince = now;
          if (now - lowSince >= LOW_VOLTAGE_DELAY_MS) {
            saver = true;
            Log.printf("[PWR] battery low (%.2f V): protection on\n", voltage);
            uplinkSetBatterySaver(true);
            if (gnss.standalone()) gnss.end();
            if (coprocAwake) {
              vehicle.sleep();
              coprocAwake = false;
            }
          }
        } else if (voltage >= cfg.lowVoltage) {
          lowSince = 0;
          if (saver && voltage >= cfg.lowVoltage + 0.3f) {
            saver = false;
            Log.printf("[PWR] battery recovered (%.2f V)\n", voltage);
            uplinkSetBatterySaver(false);
          }
        }
      }
    }
    // GNSS hot standby only for short stops
    if (gnss.standalone() && now - standbyStart > cfg.gnssKeepMin * 60000UL) {
      Log.println("[GNSS] off (long standby)");
      gnss.end();
      if (coprocAwake) {
        vehicle.sleep();
        coprocAwake = false;
      }
    }

    // keep GNSS fix fresh (hot start) and send heartbeats
    // standalone receiver or modem GNSS fed by the uplink task; the
    // co-processor receiver is polled less often (each poll is a command)
    static uint32_t lastGnssPoll = 0;
    if (!gnss.viaCoprocessor() || now - lastGnssPoll >= 2000) {
      lastGnssPoll = now;
      gnss.update();
    }
    if (!saver && cfg.heartbeatMin && now - lastHeartbeat >= cfg.heartbeatMin * 60000UL) {
      lastHeartbeat = now;
      Log.println("[PWR] heartbeat");
      pushRecord(EV_HEARTBEAT);
    }
    static uint32_t lastPub = 0;
    if (now - lastPub > 2000) { lastPub = now; publishLive(); }
  }

  Log.printf("[PWR] wake up (%s, %.2f V)\n", reason, voltage);
  uplinkSetBatterySaver(false);
  uplinkSetStandby(false);
  vehicle.wake();
  // restart the receiver only if it was switched off: a running receiver
  // keeps satellites and ephemeris (hot start), even without a fix right now
  if (gnssHw && !gnss.standalone()) gnss.begin(&sys);
  gnss.resetFilter();
  state = PS_IDLE;
  idleSince = millis();
  lastMotion = millis();
  // report immediately: the server sees the car is awake within seconds
  char extra[48];
  snprintf(extra, sizeof(extra), "&wakeReason=%s&wakeVoltage=%.2f", reason, voltage);
  for (char* c = extra; *c; c++) if (*c == ' ') *c = '_';
  pushRecord(EV_WAKE, extra);
}

// ---------------------------------------------------------------------------
void setup()
{
  // give the co-processor time to boot when everything powers up together
  delay(500);
  Serial.begin(115200);
  logBegin();
  liveMutex = xSemaphoreCreateMutex();
  memset(&live, 0, sizeof(live));
  live.bootMs = millis();
  const esp_partition_t* part = esp_ota_get_running_partition();
  Log.printf("\nFreematics Telelogger %s\n", fwVersionString());
  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  if (part) esp_ota_get_state_partition(part, &otaState);
  const esp_partition_t* bootPart = esp_ota_get_boot_partition();
  Log.printf("[SYS] running from %s (state %d, boot partition %s), reset reason %d\n", part ? part->label : "?",
             (int)otaState, bootPart ? bootPart->label : "?", (int)esp_reset_reason());
  powerOnBoot = esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_BROWNOUT;

#ifdef PIN_LED
  pinMode(PIN_LED, OUTPUT);
  led(true);
#endif

  bool loaded = settingsLoad();
  Log.printf("[CFG] %s\n", loaded ? "loaded" : "defaults");
  queue.begin();

  // The co-processor keeps running when only the ESP32 restarts (USB flashing,
  // OTA, settings) and may still be in low power mode from a previous standby.
  // If it does not answer, reset it and try again.
  bool coproc = sys.begin(true, true);
  for (int attempt = 1; !coproc && attempt <= 2; attempt++) {
    Log.printf("[SYS] co-processor not answering, reset #%d\n", attempt);
    pinMode(PIN_LINK_RESET, OUTPUT);
    digitalWrite(PIN_LINK_RESET, LOW);
    delay(100);
    digitalWrite(PIN_LINK_RESET, HIGH);
    delay(1500);
    coproc = sys.begin(true, true);
  }
  if (coproc) {
    Log.printf("[SYS] co-processor type %u\n", sys.devType);
  } else {
    Log.println("[SYS] no co-processor (OBD/CAN/voltage unavailable)");
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
  Log.printf("[SYS] device id %s\n", deviceId());

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

  static uint32_t lastGnssLog = 0;
  if ((ignition || moving) && now - lastGnssLog >= 60000) {
    lastGnssLog = now;
    gnss.logStatus();
  }

  static uint32_t lastPub = 0;
  if (now - lastPub >= 500) {
    lastPub = now;
    publishLive();
  }

  // standby when parked long enough and nothing moves
  // never while somebody uses the web UI or a firmware update runs, and not within the first minutes after
  // power-up so the device can be configured in a parked car
  const char* blocker = standbyBlocker(now, moving);
  static uint32_t lastBlockerLog = 0;
  if (blocker && !ignition && now - lastBlockerLog >= 30000) {
    lastBlockerLog = now;
    Log.printf("[PWR] no standby: %s\n", blocker);
  }
  liveLock();
  strlcpy(live.standbyBlocker, blocker ? blocker : "", sizeof(live.standbyBlocker));
  liveUnlock();
  if (!blocker) {
    standby();
    return;
  }
  if (ignition || moving) idleSince = now;
  delay(MAIN_TICK_MS);
}
