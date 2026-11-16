#include "live.h"
#include <stdarg.h>

LiveData live;
SemaphoreHandle_t liveMutex = nullptr; // created in setup()

void liveSetValue(const char* key, float value)
{
  liveLock();
  int i;
  for (i = 0; i < live.valueCount; i++) {
    if (!strcmp(live.values[i].key, key)) break;
  }
  if (i == live.valueCount) {
    if (live.valueCount >= LIVE_MAX_VALUES) {
      liveUnlock();
      return;
    }
    strlcpy(live.values[i].key, key, sizeof(live.values[i].key));
    live.valueCount++;
  }
  live.values[i].value = value;
  live.values[i].ts = millis();
  liveUnlock();
}

void liveSetError(const char* fmt, ...)
{
  char buf[sizeof(live.lastError)];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  liveLock();
  strlcpy(live.lastError, buf, sizeof(live.lastError));
  liveUnlock();
  Serial.printf("[ERR] %s\n", buf);
}
