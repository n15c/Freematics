// Log output: everything printed through Log goes to the serial console and
// into a ring buffer that the web UI can read (/api/log). The buffer lives in
// RTC memory, so it survives software restarts, crashes and OTA updates (not
// a power loss) and shows what happened before a restart.
#pragma once
#include <Arduino.h>

class LogPrint : public Print {
public:
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t size) override;
  using Print::write;
};

extern LogPrint Log;

void logBegin();
// absolute number of bytes written so far (position for incremental reads)
uint32_t logTotal();
// copy log data starting at absolute position from (clamped to what is still
// buffered); returns bytes copied, next = position after the copied data
size_t logRead(uint32_t from, char* out, size_t max, uint32_t& next);
