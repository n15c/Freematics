#include "osmand.h"

static const char* eventName(uint8_t ev)
{
  switch (ev) {
  case EV_IGNITION_ON: return "ignitionOn";
  case EV_IGNITION_OFF: return "ignitionOff";
  case EV_HEARTBEAT: return "heartbeat";
  case EV_BOOT: return "boot";
  default: return nullptr;
  }
}

static void urlEncode(const char* in, char* out, size_t size)
{
  static const char* hex = "0123456789ABCDEF";
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 4 < size; i++) {
    unsigned char c = in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[j++] = c;
    } else {
      out[j++] = '%';
      out[j++] = hex[c >> 4];
      out[j++] = hex[c & 0xf];
    }
  }
  out[j] = 0;
}

int osmandQuery(const Record& r, const char* deviceId, char* buf, size_t size)
{
  char id[64];
  urlEncode(deviceId, id, sizeof(id));
  int n = snprintf(buf, size, "id=%s", id);
  if (r.ts) n += snprintf(buf + n, size - n, "&timestamp=%lu", (unsigned long)r.ts);
  if (r.lat != 0 || r.lon != 0) {
    n += snprintf(buf + n, size - n, "&lat=%.6f&lon=%.6f&speed=%.1f&bearing=%u&altitude=%.0f",
                  r.lat, r.lon, r.speed / 1.852f, (unsigned)r.course, r.alt);
    if (r.hdop > 0) n += snprintf(buf + n, size - n, "&accuracy=%.0f&hdop=%.1f", r.hdop * 5.0f, r.hdop);
    if (r.sat) n += snprintf(buf + n, size - n, "&sat=%u", (unsigned)r.sat);
  }
  n += snprintf(buf + n, size - n, "&valid=%s&ignition=%s&motion=%s",
                r.valid ? "true" : "false", r.ignition ? "true" : "false", r.motion ? "true" : "false");
  if (r.power > 0) n += snprintf(buf + n, size - n, "&power=%.2f", r.power);
  if (r.rssi) n += snprintf(buf + n, size - n, "&rssi=%d", (int)r.rssi);
  const char* ev = eventName(r.event);
  if (ev) n += snprintf(buf + n, size - n, "&event=%s", ev);
  if (r.attrs[0]) n += snprintf(buf + n, size - n, "%s", r.attrs);
  return n < (int)size ? n : 0;
}

int osmandUrl(const char* base, const char* query, bool post, char* buf, size_t size)
{
  const char* host = strstr(base, "://");
  host = host ? host + 3 : base;
  bool hasPath = strchr(host, '/') != nullptr;
  bool hasQuery = strchr(host, '?') != nullptr;
  int n;
  if (post) {
    n = snprintf(buf, size, "%s%s", base, hasPath ? "" : "/");
  } else if (hasQuery) {
    n = snprintf(buf, size, "%s&%s", base, query);
  } else {
    n = snprintf(buf, size, "%s%s?%s", base, hasPath ? "" : "/", query);
  }
  return n < (int)size ? n : 0;
}
