// FIFO of records waiting for upload: RAM ring buffer that spills the oldest
// records to a LittleFS file when it runs full (e.g. no coverage abroad).
#pragma once
#include "record.h"

class RecordQueue {
public:
  bool begin();
  void push(const Record& r);
  // copy the oldest record without removing it
  bool peek(Record& r);
  // remove the record returned by the last peek()
  void pop();
  uint32_t size();
  uint32_t ramCount();
  uint32_t fileCount();
  uint32_t dropped() { return m_dropped; }
  // move everything in RAM to flash (before restart)
  void persist();
  void clear();
private:
  bool spillOldestToFile();
  bool filePeek(Record& r);
  void filePop();
  void syncIndex();
  Record* m_ring = nullptr;
  uint32_t m_cap = 0;
  uint32_t m_head = 0;   // next write
  uint32_t m_count = 0;
  uint32_t m_fileRead = 0;  // read offset in queue file
  uint32_t m_fileSize = 0;
  uint32_t m_dropped = 0;
  bool m_fsOk = false;
  bool m_peekFromFile = false;
  bool m_peekPending = false;
  uint16_t m_peekLen = 0;
  uint8_t m_popsSinceSync = 0;
  SemaphoreHandle_t m_mutex = nullptr;
};

extern RecordQueue queue;
