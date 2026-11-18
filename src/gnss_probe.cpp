#include "gnss_probe.h"
#include "gnss.h"
#include "log.h"

static volatile bool s_request = false;

void gnssProbeRequest() { s_request = true; }

static int ubxFrame(uint8_t cls, uint8_t id, const uint8_t* payload, uint16_t len, uint8_t* out)
{
  out[0] = 0xB5; out[1] = 0x62; out[2] = cls; out[3] = id;
  out[4] = len & 0xFF; out[5] = len >> 8;
  if (len) memcpy(out + 6, payload, len);
  uint8_t a = 0, b = 0;
  for (int i = 2; i < 6 + len; i++) { a += out[i]; b += a; }
  out[6 + len] = a; out[7 + len] = b;
  return 8 + len;
}

// send raw bytes to the receiver
static void sendRaw(FreematicsESP32* sys, const uint8_t* data, int len)
{
  if (sys->gpsUsesLink()) {
    // co-processor: "ATGDS" + hex bytes, in small chunks
    char cmd[120], resp[64];
    for (int off = 0; off < len; off += 32) {
      int n = snprintf(cmd, sizeof(cmd), "ATGDS");
      for (int i = off; i < len && i < off + 32; i++) n += snprintf(cmd + n, sizeof(cmd) - n, "%02X ", data[i]);
      cmd[n - 1] = '\r';
      cmd[n] = 0;
      int r = sys->link->sendCommand(cmd, resp, sizeof(resp), 200);
      if (off == 0) Log.printf("[PROBE] ATGDS answer (%d bytes): %.40s\n", r, r > 0 ? resp : "");
    }
  } else {
    sys->gpsSendCommand(data, len);
  }
}

// collect receiver output for ms milliseconds
static int collect(FreematicsESP32* sys, uint8_t* buf, int size, uint32_t ms)
{
  int total = 0;
  if (sys->gpsUsesLink()) {
    static char tmp[512];
    uint32_t t = millis();
    int polls = 0, first = -1;
    while (millis() - t < ms && total < size) {
      int n = sys->link->sendCommand("ATGRR\r", tmp, sizeof(tmp), 200);
      polls++;
      if (n > 0) {
        if (first < 0) first = n;
        int c = n < size - total ? n : size - total;
        memcpy(buf + total, tmp, c);
        total += c;
      }
      delay(20);
    }
    Log.printf("[PROBE] ATGRR: %d polls, first answer %d bytes\n", polls, first);
  } else {
    sys->gpsCaptureStart(buf, size);
    delay(ms);
    total = sys->gpsCaptureStop();
  }
  return total;
}

struct UbxStats {
  int frames;
  int badChecksum;
  int monVer;
  int dbd;
  int ack;
  int nack;
  int dbdBytes;
};

static void scan(const uint8_t* b, int n, UbxStats& st, bool logVersion)
{
  for (int i = 0; i + 8 <= n; i++) {
    if (b[i] != 0xB5 || b[i + 1] != 0x62) continue;
    int len = b[i + 4] | (b[i + 5] << 8);
    if (i + 8 + len > n) break;
    uint8_t a = 0, c = 0;
    for (int k = i + 2; k < i + 6 + len; k++) { a += b[k]; c += a; }
    if (a != b[i + 6 + len] || c != b[i + 7 + len]) { st.badChecksum++; continue; }
    st.frames++;
    uint8_t cls = b[i + 2], id = b[i + 3];
    const uint8_t* p = b + i + 6;
    if (cls == 0x0A && id == 0x04) {
      st.monVer++;
      if (logVersion && len >= 40) {
        Log.printf("[PROBE] MON-VER sw:'%.30s' hw:'%.10s'\n", (const char*)p, (const char*)p + 30);
        for (int e = 40; e + 30 <= len; e += 30) Log.printf("[PROBE]   ext:'%.30s'\n", (const char*)p + e);
      }
    } else if (cls == 0x13 && id == 0x80) {
      st.dbd++;
      st.dbdBytes += len + 8;
    } else if (cls == 0x05) {
      if (id == 0x01) st.ack++; else st.nack++;
    }
    i += 7 + len;
  }
}

static void dump(const uint8_t* b, int n)
{
  // first bytes as hex and as text, to see what the interface returns
  char line[3 * 32 + 1];
  int show = n < 96 ? n : 96;
  for (int off = 0; off < show; off += 32) {
    int k = 0;
    for (int i = off; i < show && i < off + 32; i++) k += snprintf(line + k, sizeof(line) - k, "%02X ", b[i]);
    Log.printf("[PROBE]   %s\n", line);
  }
  char txt[81];
  int t = 0;
  for (int i = 0; i < n && t < 80; i++) txt[t++] = (b[i] >= 32 && b[i] < 127) ? b[i] : '.';
  txt[t] = 0;
  Log.printf("[PROBE]   text: %s\n", txt);
}

void gnssProbeService(FreematicsESP32* sys)
{
  if (!s_request) return;
  s_request = false;
  if (!gnss.standalone()) {
    Log.println("[PROBE] no standalone GNSS receiver running");
    return;
  }
  bool link = sys->gpsUsesLink();
  Log.printf("[PROBE] receiver via %s, devType %u\n", link ? "co-processor (ATGDS/ATGRR)" : "UART", sys->devType);
  static uint8_t buf[6144];
  uint8_t frame[16];
  UbxStats st;

  // 1) baseline: what does the interface deliver without a request?
  int n = collect(sys, buf, 512, 1200);
  Log.printf("[PROBE] idle output: %d bytes\n", n);
  dump(buf, n);

  // 2) UBX-MON-VER poll: a short, unambiguous UBX answer
  memset(&st, 0, sizeof(st));
  int fl = ubxFrame(0x0A, 0x04, nullptr, 0, frame);
  sendRaw(sys, frame, fl);
  n = collect(sys, buf, sizeof(buf), 2000);
  scan(buf, n, st, true);
  Log.printf("[PROBE] MON-VER: %d bytes, %d UBX frames (%d bad), MON-VER %s\n",
             n, st.frames, st.badChecksum, st.monVer ? "RECEIVED" : "not received");
  if (!st.monVer) dump(buf, n);

  // 3) UBX-MGA-DBD poll: navigation database dump (what we would save)
  memset(&st, 0, sizeof(st));
  fl = ubxFrame(0x13, 0x80, nullptr, 0, frame);
  sendRaw(sys, frame, fl);
  n = collect(sys, buf, sizeof(buf), 4000);
  scan(buf, n, st, false);
  Log.printf("[PROBE] MGA-DBD: %d bytes, %d DBD frames (%d bytes)%s, %d bad checksums%s\n",
             n, st.dbd, st.dbdBytes, n >= (int)sizeof(buf) ? " BUFFER FULL" : "", st.badChecksum,
             st.dbd ? " -> database readable" : "");

  Log.printf("[PROBE] result: UBX answers %s, database %s\n",
             st.frames || st.monVer ? "readable" : "NOT readable", st.dbd ? "readable" : "not readable");
}
