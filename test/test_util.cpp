// Host unit tests: sh test/run_tests.sh
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "util.h"
#include "osmand.h"

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b))) { failures++; printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

static void testTime()
{
  CHECK(gnssToUnix(161126, 21300000) == 1794864600u);   // 2026-11-16 21:30:00
  CHECK(gnssToUnix(290224, 12000000) == 1709208000u);   // leap day 2024-02-29 12:00
  CHECK(gnssToUnix(0, 12000000) == 0);                  // no date yet
  CHECK(gnssToUnix(10180, 0) == 0);                     // 1980 from receiver default
}

static void testDistance()
{
  // Zurich HB -> Bern HB ~ 95 km
  double d = distanceMeters(47.3779, 8.5403, 46.9490, 7.4391);
  CHECK(d > 94000 && d < 96000);
  // Gotthard road tunnel portals (Goeschenen -> Airolo) ~ 16.4 km: must not lock the position
  double t = distanceMeters(46.6644, 8.5878, 46.5181, 8.6086);
  CHECK(t > 15000 && t < 17500);
}

static void testBits()
{
  int64_t v;
  const uint8_t be[] = {0x12, 0x34, 0xFF};
  CHECK(extractBits(be, 3, 0, 16, true, false, v) && v == 0x1234);
  CHECK(extractBits(be, 3, 4, 8, true, false, v) && v == 0x23);
  CHECK(extractBits(be, 3, 16, 8, true, true, v) && v == -1);
  CHECK(!extractBits(be, 3, 16, 16, true, false, v));            // out of range
  const uint8_t le[] = {0x34, 0x12, 0xA5};
  CHECK(extractBits(le, 3, 0, 16, false, false, v) && v == 0x1234);
  CHECK(extractBits(le, 3, 20, 4, false, false, v) && v == 0xA);
  CHECK(extractBits(le, 3, 16, 1, false, false, v) && v == 1);
  const uint8_t s16[] = {0x18, 0xFC};                             // -1000 little endian
  CHECK(extractBits(s16, 2, 0, 16, false, true, v) && v == -1000);
}

static void testResponse()
{
  uint8_t d[32];
  int n = 0;
  char r1[] = "62 F4 0D 4B \r\r";
  CHECK(parseRequestResponse(r1, "22F40D", d, sizeof(d), n) && n == 1 && d[0] == 0x4B);
  char r2[] = "41 0C 1A F8 \r";
  CHECK(parseRequestResponse(r2, "010C", d, sizeof(d), n) && n == 2 && d[0] == 0x1A && d[1] == 0xF8);
  char r3[] = "00B\r0: 62 F4 5B 01 02 03\r1: 04 05 06 07 08 00 00\r";
  CHECK(parseRequestResponse(r3, "22F45B", d, sizeof(d), n) && n >= 8 && d[0] == 0x01 && d[7] == 0x08);
  char r4[] = "7F 22 31 \r";                                         // negative response
  CHECK(!parseRequestResponse(r4, "22F40D", d, sizeof(d), n));
  char r5[] = "7E8 03 41 0D 32 \r";                                  // headers on
  CHECK(parseRequestResponse(r5, "010D", d, sizeof(d), n) && d[0] == 0x32);
  char r6[] = "620101AABB\r";                                        // no spaces
  CHECK(parseRequestResponse(r6, "220101", d, sizeof(d), n) && n == 2 && d[0] == 0xAA);
  char r7[] = "NO DATA\r";
  CHECK(!parseRequestResponse(r7, "220101", d, sizeof(d), n));
}

static void testSniff()
{
  uint32_t id;
  uint8_t d[8], len;
  char l1[] = "7E8 03 41 0D 00";
  CHECK(parseSniffLine(l1, false, id, d, len) && id == 0x7E8 && len == 4 && d[1] == 0x41);
  char l2[] = "18DAF110 03 41 0D 00";
  CHECK(parseSniffLine(l2, true, id, d, len) && id == 0x18DAF110 && len == 4);
  char l3[] = "18 DA F1 10 03 41 0D 00";
  CHECK(parseSniffLine(l3, true, id, d, len) && id == 0x18DAF110 && len == 4 && d[3] == 0x00);
  char l4[] = "3E9123456789ABCDEF0";
  CHECK(parseSniffLine(l4, false, id, d, len) && id == 0x3E9 && len == 8 && d[0] == 0x12);
  char l5[] = "$3E9,01,02,FF";
  CHECK(parseSniffLine(l5, false, id, d, len) && id == 0x3E9 && len == 3 && d[2] == 0xFF);
  char l6[] = "OK";
  CHECK(!parseSniffLine(l6, false, id, d, len));
}

static void testFormat()
{
  char b[32];
  fmtNum(b, sizeof(b), 12.5f); CHECK_STR(b, "12.5");
  fmtNum(b, sizeof(b), 3.0f); CHECK_STR(b, "3");
  fmtNum(b, sizeof(b), -0.25f); CHECK_STR(b, "-0.25");
  fmtNum(b, sizeof(b), 1e7f); CHECK_STR(b, "10000000");
  fmtNum(b, sizeof(b), NAN); CHECK_STR(b, "0");
  char a[64] = "";
  appendKV(a, sizeof(a), "rpm", 850);
  appendKV(a, sizeof(a), "bad key&=", 1.5f);
  CHECK_STR(a, "&rpm=850&badkey=1.5");
}

static void testOsmand()
{
  Record r;
  memset(&r, 0, sizeof(r));
  r.ts = 1794864600;
  r.lat = 45.4642f; r.lon = 9.19f; r.alt = 120; r.speed = 92.6f; r.course = 181; r.sat = 9; r.hdop = 1.2f;
  r.valid = true; r.ignition = true; r.motion = true; r.power = 14.1f; r.rssi = -71; r.event = EV_IGNITION_ON;
  strcpy(r.attrs, "&rpm=2100&coolantTemp=88");
  char q[512];
  CHECK(osmandQuery(r, "FM 1", q, sizeof(q)) > 0);
  CHECK(strstr(q, "id=FM%201&timestamp=1794864600&lat=45.464199&lon=9.190000") == q);
  CHECK(strstr(q, "&speed=50.0&") != nullptr);                     // km/h -> knots
  CHECK(strstr(q, "&valid=true&ignition=true&motion=true&power=14.10&rssi=-71&event=ignitionOn&rpm=2100&coolantTemp=88") != nullptr);
  char u[600];
  osmandUrl("http://traccar:5055", "id=1", false, u, sizeof(u)); CHECK_STR(u, "http://traccar:5055/?id=1");
  osmandUrl("http://traccar:5055/", "id=1", false, u, sizeof(u)); CHECK_STR(u, "http://traccar:5055/?id=1");
  osmandUrl("https://ha.local/api/webhook/abc", "id=1", false, u, sizeof(u)); CHECK_STR(u, "https://ha.local/api/webhook/abc?id=1");
  osmandUrl("http://h/?key=x", "id=1", false, u, sizeof(u)); CHECK_STR(u, "http://h/?key=x&id=1");
  osmandUrl("http://traccar:5055", "id=1", true, u, sizeof(u)); CHECK_STR(u, "http://traccar:5055/");
  // no position known yet: no coordinates, invalid
  Record e;
  memset(&e, 0, sizeof(e));
  CHECK(osmandQuery(e, "x", q, sizeof(q)) > 0);
  CHECK(strstr(q, "lat=") == nullptr && strstr(q, "valid=false") != nullptr);
  // too small buffer is reported
  char small[16];
  CHECK(osmandQuery(r, "FM1", small, sizeof(small)) == 0);
}

int main()
{
  testTime();
  testDistance();
  testBits();
  testResponse();
  testSniff();
  testFormat();
  testOsmand();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
