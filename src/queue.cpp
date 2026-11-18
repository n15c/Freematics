#include "log.h"
#include "queue.h"
#include <LittleFS.h>
#include <stddef.h>

RecordQueue queue;

static const size_t FIXED_LEN = offsetof(Record, attrs);

class Lock {
public:
  Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }
private:
  SemaphoreHandle_t m_;
};

bool RecordQueue::begin()
{
  m_mutex = xSemaphoreCreateMutex();
  m_cap = QUEUE_RAM_RECORDS;
#ifdef BOARD_HAS_PSRAM
  m_ring = (Record*)ps_malloc(sizeof(Record) * m_cap);
#endif
  if (!m_ring) {
    if (m_cap > 40) m_cap = 40;
    m_ring = (Record*)malloc(sizeof(Record) * m_cap);
  }
  m_fsOk = LittleFS.begin(true);
  if (m_fsOk) {
    File f = LittleFS.open(QUEUE_FILE, "r");
    if (f) {
      m_fileSize = f.size();
      f.close();
      File idx = LittleFS.open(QUEUE_INDEX_FILE, "r");
      if (idx) {
        idx.read((uint8_t*)&m_fileRead, sizeof(m_fileRead));
        idx.close();
      }
      if (m_fileRead >= m_fileSize) {
        LittleFS.remove(QUEUE_FILE);
        LittleFS.remove(QUEUE_INDEX_FILE);
        m_fileRead = m_fileSize = 0;
      }
    }
  }
  Log.printf("[QUEUE] RAM slots:%u, file:%u bytes pending\n", m_cap, m_fileSize - m_fileRead);
  return m_ring != nullptr;
}

static bool writeEntry(File& f, const Record& r)
{
  uint16_t alen = strnlen(r.attrs, RECORD_ATTR_LEN - 1);
  uint16_t total = FIXED_LEN + alen;
  return f.write((const uint8_t*)&total, 2) == 2 &&
         f.write((const uint8_t*)&r, FIXED_LEN) == FIXED_LEN &&
         f.write((const uint8_t*)r.attrs, alen) == alen;
}

bool RecordQueue::spillOldestToFile()
{
  if (!m_fsOk || !m_count) return false;
  uint32_t tail = (m_head + m_cap - m_count) % m_cap;
  bool ok = false;
  if (m_fileSize < QUEUE_FILE_MAX_BYTES) {
    File f = LittleFS.open(QUEUE_FILE, "a");
    if (f) {
      ok = writeEntry(f, m_ring[tail]);
      m_fileSize = f.size();
      f.close();
    }
  }
  if (!ok) m_dropped++;
  if (m_peekPending && !m_peekFromFile) {
    // the record handed out by peek() now lives at the start of the file
    // (peek only reads from RAM while the file is empty)
    if (ok) m_peekFromFile = true; else m_peekPending = false;
  }
  m_count--;
  return ok;
}

void RecordQueue::push(const Record& r)
{
  if (!m_ring) return;
  Lock l(m_mutex);
  if (m_count == m_cap) {
    if (!m_fsOk) {
      // drop oldest
      m_count--;
      m_dropped++;
      if (m_peekPending && !m_peekFromFile) m_peekPending = false;
    } else {
      spillOldestToFile();
    }
  }
  m_ring[m_head] = r;
  m_head = (m_head + 1) % m_cap;
  m_count++;
}

bool RecordQueue::filePeek(Record& r)
{
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return false;
  bool ok = false;
  uint16_t total = 0;
  if (f.seek(m_fileRead) && f.read((uint8_t*)&total, 2) == 2 &&
      total >= FIXED_LEN && total < FIXED_LEN + RECORD_ATTR_LEN) {
    uint16_t alen = total - FIXED_LEN;
    ok = f.read((uint8_t*)&r, FIXED_LEN) == FIXED_LEN &&
         f.read((uint8_t*)r.attrs, alen) == alen;
    r.attrs[alen] = 0;
    m_peekLen = total + 2;
  }
  f.close();
  if (!ok) {
    // corrupt file: discard it
    LittleFS.remove(QUEUE_FILE);
    LittleFS.remove(QUEUE_INDEX_FILE);
    m_fileRead = m_fileSize = 0;
  }
  return ok;
}

void RecordQueue::filePop()
{
  m_fileRead += m_peekLen;
  if (m_fileRead >= m_fileSize) {
    LittleFS.remove(QUEUE_FILE);
    LittleFS.remove(QUEUE_INDEX_FILE);
    m_fileRead = m_fileSize = 0;
    m_popsSinceSync = 0;
  } else if (++m_popsSinceSync >= 10) {
    syncIndex();
  }
}

void RecordQueue::syncIndex()
{
  m_popsSinceSync = 0;
  if (!m_fileSize) return;
  File idx = LittleFS.open(QUEUE_INDEX_FILE, "w");
  if (idx) {
    idx.write((const uint8_t*)&m_fileRead, sizeof(m_fileRead));
    idx.close();
  }
}

bool RecordQueue::peek(Record& r)
{
  if (!m_ring) return false;
  Lock l(m_mutex);
  m_peekPending = false;
  if (m_fsOk && m_fileRead < m_fileSize) {
    if (filePeek(r)) {
      m_peekFromFile = true;
      m_peekPending = true;
      return true;
    }
  }
  if (!m_count) return false;
  uint32_t tail = (m_head + m_cap - m_count) % m_cap;
  r = m_ring[tail];
  m_peekFromFile = false;
  m_peekPending = true;
  return true;
}

void RecordQueue::pop()
{
  Lock l(m_mutex);
  if (!m_peekPending) return;
  m_peekPending = false;
  if (m_peekFromFile) {
    filePop();
  } else if (m_count) {
    m_count--;
  }
}

uint32_t RecordQueue::ramCount()
{
  return m_count;
}

uint32_t RecordQueue::fileCount()
{
  // estimate: average entry size ~ fixed part + 120 bytes of attributes
  uint32_t pending = m_fileSize - m_fileRead;
  return pending ? (pending / (FIXED_LEN + 122) + 1) : 0;
}

uint32_t RecordQueue::size()
{
  return ramCount() + fileCount();
}

void RecordQueue::persist()
{
  if (!m_ring || !m_fsOk) return;
  Lock l(m_mutex);
  m_peekPending = false;
  while (m_count) spillOldestToFile();
  syncIndex();
}

void RecordQueue::clear()
{
  Lock l(m_mutex);
  m_count = 0;
  m_peekPending = false;
  if (m_fsOk) {
    LittleFS.remove(QUEUE_FILE);
    LittleFS.remove(QUEUE_INDEX_FILE);
  }
  m_fileRead = m_fileSize = 0;
}
