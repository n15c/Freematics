#include "log.h"
#include <esp_attr.h>
#include <time.h>

LogPrint Log;

#define LOG_SIZE 6144
#define LOG_MAGIC (0x4C4F4700u | (LOG_SIZE >> 10))

struct LogRing {
  uint32_t magic;
  uint32_t total;   // bytes ever written (position)
  bool lineStart;
  char buf[LOG_SIZE];
};

static RTC_NOINIT_ATTR LogRing ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_ready = false;

static void ringPut(const char* p, size_t n)
{
  for (size_t i = 0; i < n; i++) {
    ring.buf[ring.total % LOG_SIZE] = p[i];
    ring.total++;
  }
}

void logBegin()
{
  if (ring.magic != LOG_MAGIC || ring.total > 0x7FFFFFFF) {
    ring.magic = LOG_MAGIC;
    ring.total = 0;
    ring.lineStart = true;
  }
  s_ready = true;
  Log.printf("%s===== restart, reset reason %d =====\n", ring.lineStart ? "" : "\n", (int)esp_reset_reason());
}

size_t LogPrint::write(const uint8_t* buf, size_t size)
{
  Serial.write(buf, size);
  if (!s_ready) return size;
  // time prefix for every line: UTC time once known, else uptime
  char ts[20];
  time_t now = time(nullptr);
  int tl;
  if (now > 1700000000) {
    struct tm t;
    gmtime_r(&now, &t);
    tl = snprintf(ts, sizeof(ts), "%02d:%02d:%02d ", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    uint32_t ms = millis();
    tl = snprintf(ts, sizeof(ts), "+%lu.%lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000) / 100);
  }
  portENTER_CRITICAL(&s_mux);
  const char* p = (const char*)buf;
  for (size_t i = 0; i < size; i++) {
    if (p[i] == '\r') continue;
    if (ring.lineStart) {
      ringPut(ts, tl);
      ring.lineStart = false;
    }
    ringPut(p + i, 1);
    if (p[i] == '\n') ring.lineStart = true;
  }
  portEXIT_CRITICAL(&s_mux);
  return size;
}

uint32_t logTotal()
{
  return ring.total;
}

size_t logRead(uint32_t from, char* out, size_t max, uint32_t& next)
{
  portENTER_CRITICAL(&s_mux);
  uint32_t total = ring.total;
  uint32_t oldest = total > LOG_SIZE ? total - LOG_SIZE : 0;
  if (from < oldest || from > total) from = oldest;
  size_t n = 0;
  while (from + n < total && n < max) {
    out[n] = ring.buf[(from + n) % LOG_SIZE];
    n++;
  }
  portEXIT_CRITICAL(&s_mux);
  next = from + n;
  return n;
}
