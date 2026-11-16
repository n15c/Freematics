// Pure helper functions (no hardware access) - unit tested on the host, see test/.
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

static int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// date: ddmmyy, time: hhmmsscc
uint32_t gnssToUnix(uint32_t date, uint32_t time)
{
  unsigned day = date / 10000, month = (date / 100) % 100, yy = date % 100;
  unsigned year = yy < 80 ? 2000 + yy : 1900 + yy; // receivers report 1980 before a fix
  if (!day || day > 31 || !month || month > 12 || year < 2020) return 0;
  unsigned hh = time / 1000000, mm = (time / 10000) % 100, ss = (time / 100) % 100;
  return (uint32_t)(daysFromCivil(year, month, day) * 86400 + hh * 3600 + mm * 60 + ss);
}

double distanceMeters(double lat1, double lon1, double lat2, double lon2)
{
  const double R = 6371000.0;
  double p1 = lat1 * M_PI / 180, p2 = lat2 * M_PI / 180;
  double dp = p2 - p1, dl = (lon2 - lon1) * M_PI / 180;
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2 * R * atan2(sqrt(a), sqrt(1 - a));
}

int hexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool isHexStr(const char* s, int len)
{
  for (int i = 0; i < len; i++) if (hexVal(s[i]) < 0) return false;
  return len > 0;
}

uint32_t parseHex(const char* s, int len)
{
  uint32_t v = 0;
  for (int i = 0; i < len; i++) v = (v << 4) | hexVal(s[i]);
  return v;
}

// format a number compactly and URL-safe (no exponent, no '+')
int fmtNum(char* buf, size_t size, float v)
{
  if (isnan(v) || isinf(v)) return snprintf(buf, size, "0");
  if (fabsf(v - roundf(v)) < 0.0005f && fabsf(v) < 2e9f) return snprintf(buf, size, "%ld", (long)lroundf(v));
  int n = snprintf(buf, size, "%.3f", v);
  // trim trailing zeros
  while (n > 0 && buf[n - 1] == '0') buf[--n] = 0;
  if (n > 0 && buf[n - 1] == '.') buf[--n] = 0;
  return n;
}

void appendKV(char* buf, size_t size, const char* key, float v)
{
  size_t len = strlen(buf);
  if (len + 4 >= size) return;
  char num[24];
  fmtNum(num, sizeof(num), v);
  char k[24];
  int j = 0;
  for (int i = 0; key[i] && j < (int)sizeof(k) - 1; i++) {
    char c = key[i];
    if (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') k[j++] = c;
  }
  k[j] = 0;
  if (!j) return;
  snprintf(buf + len, size - len, "&%s=%s", k, num);
}

bool extractBits(const uint8_t* data, int len, uint16_t startBit, uint8_t bitLen, bool bigEndian, bool isSigned, int64_t& out)
{
  if (bitLen < 1 || bitLen > 32) return false;
  uint64_t v = 0;
  if (bigEndian) {
    // bit 0 = MSB of byte 0, value read MSB first (UDS style: byte n -> startBit n*8)
    if ((int)(startBit + bitLen + 7) / 8 > len) return false;
    for (int i = 0; i < bitLen; i++) {
      int b = startBit + i;
      int bit = (data[b / 8] >> (7 - (b % 8))) & 1;
      v = (v << 1) | bit;
    }
  } else {
    // Intel / DBC little endian: startBit = LSB position (byte n bit b -> n*8+b)
    if ((int)(startBit + bitLen + 7) / 8 > len) return false;
    for (int i = 0; i < bitLen; i++) {
      int b = startBit + i;
      uint64_t bit = (data[b / 8] >> (b % 8)) & 1;
      v |= bit << i;
    }
  }
  if (isSigned && bitLen < 64 && (v & ((uint64_t)1 << (bitLen - 1)))) {
    out = (int64_t)v - ((int64_t)1 << bitLen);
  } else {
    out = (int64_t)v;
  }
  return true;
}


bool parseRequestResponse(char* buf, const char* request, uint8_t* out, int outSize, int& outLen)
{
  int reqLen = strlen(request);
  if (reqLen < 2 || (reqLen & 1) || !isHexStr(request, reqLen)) return false;
  // collect all hex bytes of the response (ISO-TP lines look like "0: 62 F4 0D ...")
  uint8_t bytes[96];
  int nb = 0;
  for (char* line = buf; line && *line && nb < (int)sizeof(bytes); ) {
    char* end = strpbrk(line, "\r\n");
    if (end) *end = 0;
    char* p = strchr(line, ':');
    p = p ? p + 1 : line;
    // skip a pure length line like "014"
    int tokens = 0;
    for (char* t = p; *t; ) {
      while (*t == ' ') t++;
      int l = 0;
      while (t[l] && t[l] != ' ') l++;
      if (l == 2 && isHexStr(t, 2) && nb < (int)sizeof(bytes)) {
        bytes[nb++] = parseHex(t, 2);
        tokens++;
      } else if (l > 2 && !(l % 2) && isHexStr(t, l) && !strchr(line, ':') && tokens == 0 && l >= 4) {
        // response without spaces
        for (int i = 0; i < l && nb < (int)sizeof(bytes); i += 2) bytes[nb++] = parseHex(t + i, 2);
        tokens++;
      }
      t += l;
    }
    line = end ? end + 1 : nullptr;
  }

  // find positive response: SID+0x40 followed by the rest of the request (PID / DID)
  uint8_t sid = parseHex(request, 2);
  int echoLen = reqLen / 2 - 1;
  for (int i = 0; i + 1 + echoLen <= nb; i++) {
    if (bytes[i] != (uint8_t)(sid + 0x40)) continue;
    bool match = true;
    for (int k = 0; k < echoLen && match; k++) {
      match = bytes[i + 1 + k] == parseHex(request + 2 + k * 2, 2);
    }
    if (!match) continue;
    const uint8_t* data = bytes + i + 1 + echoLen;
    int dlen = nb - (i + 1 + echoLen);
    if (dlen > outSize) dlen = outSize;
    memcpy(out, data, dlen);
    outLen = dlen;
    return true;
  }
  return false;
}

bool parseSniffLine(char* line, bool ext29, uint32_t& id, uint8_t* data, uint8_t& len)
{
  len = 0;
  while (*line == ' ') line++;
  if (!*line || !strncmp(line, "OK", 2) || *line == '>' || !strncmp(line, "AT", 2)) return false;
  if (*line == '$') {
    // "$<id>,<b0>,<b1>,..."
    char* comma = strchr(line, ',');
    if (!comma) return false;
    if (!isHexStr(line + 1, comma - line - 1)) return false;
    id = parseHex(line + 1, comma - line - 1);
    for (char* p = comma; *p == ',' && len < 8; ) {
      if (!isHexStr(p + 1, 2)) break;
      data[len++] = parseHex(p + 1, 2);
      p += 3;
    }
    return true;
  }
  // tokenise
  char* tok[16];
  int nt = 0;
  char* save = nullptr;
  for (char* p = strtok_r(line, " ", &save); p && nt < 16; p = strtok_r(nullptr, " ", &save)) tok[nt++] = p;
  if (!nt) return false;
  int first = 0;
  int l0 = strlen(tok[0]);
  if (nt == 1) {
    // no spaces: "7E8034110..." or "18DAF110034110..."
    int idLen = ext29 ? 8 : 3;
    if (l0 <= idLen || !isHexStr(tok[0], l0)) return false;
    id = parseHex(tok[0], idLen);
    for (int i = idLen; i + 1 < l0 && len < 8; i += 2) data[len++] = parseHex(tok[0] + i, 2);
    return true;
  }
  if ((l0 == 3 || l0 == 8) && isHexStr(tok[0], l0)) {
    id = parseHex(tok[0], l0);
    first = 1;
  } else if (ext29 && nt >= 5 && l0 == 2) {
    // "18 DA F1 10 xx ..."
    id = 0;
    for (int i = 0; i < 4; i++) {
      if (!isHexStr(tok[i], 2)) return false;
      id = (id << 8) | parseHex(tok[i], 2);
    }
    first = 4;
  } else {
    return false;
  }
  for (int i = first; i < nt && len < 8; i++) {
    if (strlen(tok[i]) != 2 || !isHexStr(tok[i], 2)) break;
    data[len++] = parseHex(tok[i], 2);
  }
  return true;
}

