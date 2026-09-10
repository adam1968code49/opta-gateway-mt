#ifndef INFLUX_PUSH_H
#define INFLUX_PUSH_H

// =====================================================================
//  CLOUD THREAD ONLY. Push line protocol straight into InfluxDB Cloud
//  from the board (HTTPS POST /api/v2/write). Ported from the single-
//  thread firmware's influx_push.h (2026-09-02), which was never proven:
//  its self-test sat behind a placeholder token for its whole life.
//
//  Batch 11 phase 1 = TRANSPORT + SELF-TEST ONLY. Before any buffering or
//  gap replay is built on top, one handshake has to be seen to succeed on
//  this board: heap at the peak (connected, before close), duration, HTTP
//  code. influxSelfTest() does that once, a minute after the cloud is up,
//  into its own measurement (opta_selftest) so a failed experiment can
//  never land in a series anything reads. Result -> pushStat.
//
//  TLS: BearSSL, the same stack the cloud library uses on Opta
//  (AIoTC_Config.h -> BOARD_USE_BEARSSL), with ArduinoBearSSL's own
//  default trust anchors -- Amazon Root CA 1 is among them, and that is
//  InfluxDB Cloud's chain (AWS). NOT mbed's WiFiSSLClient: its /wlan/
//  bundle carries no Amazon root (review, 2026-09-10), and mbedTLS peaks
//  at ~100 KB of heap for one handshake. BearSSL keeps its buffers inside
//  the client object, so that object is file-scope, not on the 24 KB
//  cloud-thread stack. ArduinoBearSSL.onGetTime is already installed by
//  the cloud library (TLSClientMqtt.cpp); certificate dates validate.
//
//  Gate: ArduinoCloud.connected(), not WiFi.status(). An associated radio
//  with no route is exactly the state in which connect() blocks for 18 s
//  (the old eip.begin() lesson). If MQTT is passing, the path is real.
// =====================================================================

#include <Arduino.h>
#include <malloc.h>
#include <time.h>
#include <WiFi.h>
#include <ArduinoBearSSL.h>
#include <BearSSLTrustAnchors.h>   // TAs / TAs_NUM: ArduinoBearSSL's default list (13 roots incl. Amazon Root CA 1)
#include "config.h"
#include "shared.h"
#include "wd_feeder.h"
#include "thingProperties.h"
#include "cloud_probe.h"     // cloudProbeLastOkMs(): "the internet path is real" without needing the MQTT session
#include "secrets.h"        // INFLUX_HOST / INFLUX_BUCKET / INFLUX_TOKEN -- gitignored, never printed

#ifndef INFLUX_PORT
#define INFLUX_PORT 443
#endif
#ifndef INFLUX_THING_NAME
#define INFLUX_THING_NAME "IP_2_thing"    // the tag live points carry; a different board must override this
#endif
#define INFLUX_MAX_BODY          10240    // ~25 rows of line protocol: one TCP window, one pass
#define INFLUX_RESP_TIMEOUT_MS   4000     // status line only; a 204 has no body
#define INFLUX_BACKOFF_MS        30000UL  // first wait after a failure, doubling
#define INFLUX_BACKOFF_MAX_MS    900000UL // 15 min: replay is never urgent
#define INFLUX_SUCCESS_GAP_MS    2000UL   // after a 2xx the next POST may follow this soon (review C1: 30 s here made a 60 min drain)
#define INFLUX_SELFTEST_AFTER_MS 60000UL  // one minute of cloud-up before the experiment

static WiFiClient    s_pushTcp;
static BearSSLClient s_pushSsl(s_pushTcp, TAs, TAs_NUM);   // file-scope: its TLS buffers must not live on the thread stack

static unsigned long s_pushWait  = INFLUX_BACKOFF_MS;
static unsigned long s_pushLast  = 0;
static uint32_t      s_pushOks   = 0;
static uint32_t      s_pushFails = 0;
static int           s_pushMs    = 0;
static int           s_pushCode  = 0;     // last HTTP status, or -1 refused / -2 connect / -3 no reply / 0 never
static unsigned long s_pushHeapPeakFree = 0;   // heapFree while connected (the real cost of the handshake)

inline uint32_t influxPushOks()   { return s_pushOks; }
inline uint32_t influxPushFails() { return s_pushFails; }
inline int      influxPushMs()    { return s_pushMs; }
inline int      influxPushCode()  { return s_pushCode; }

//  pushStat is a CloudString (two Strings inside). Build the String only
//  when the text actually changed, never on a rewrite of the same status.
static char s_pushStatPrev[128] = "";
static void pushStatSet(const char* st) {
  if (strncmp(s_pushStatPrev, st, sizeof s_pushStatPrev) == 0) return;
  snprintf(s_pushStatPrev, sizeof s_pushStatPrev, "%s", st);
  pushStat = String(st);
}

//  A board built from secrets.h.example pushes nothing rather than
//  retrying a 401 forever.
inline bool influxConfigured() { return strncmp(INFLUX_TOKEN, "PUT_YOUR", 8) != 0; }

//  "The path to the internet is real right now": either MQTT is passing, or
//  -- while the Arduino Cloud is down -- the probe's TCP knock landed within
//  the last two probe periods. Either one is proof connect() will not sit in
//  the 18 s "associated but no route" hole. (batch 11.1: without the second
//  clause the board waited for the Arduino Cloud before filling a gap the
//  Arduino Cloud itself was causing.)
inline bool influxNetReal(unsigned long now) {
  if (ArduinoCloud.connected()) return true;
  unsigned long ok = cloudProbeLastOkMs();
  return ok != 0 && now - ok < 2 * CLOUD_PROBE_PERIOD_MS;
}

inline bool influxPushReady(unsigned long now) {
  if (!influxConfigured())                          return false;
  if (g_otaStarted)                                 return false;   // never a second TLS session beside an OTA download
  if (WiFi.status() != WL_CONNECTED)                return false;
  if (!influxNetReal(now))                          return false;
  if (s_pushLast && now - s_pushLast < s_pushWait)  return false;
  return true;
}

//  POST one batch. Returns the HTTP status, or a negative local code.
//  Caller has checked influxPushReady(). Blocks for the handshake and one
//  round trip; where-code 4 names it if it ever runs long.
static int influxPush(const char* body, size_t len) {
  SHARED_ASSERT_ON_CLOUD();
  if (len == 0 || len > INFLUX_MAX_BODY) { s_pushCode = -1; return -1; }
  unsigned long t0 = millis();
  s_pushLast = t0;
  wdWhereCloud(WD_AT_PUSH);

  int code = -2;
  if (s_pushSsl.connect(INFLUX_HOST, INFLUX_PORT)) {
    struct mallinfo mid = mallinfo();
    s_pushHeapPeakFree = mid.fordblks;           // measured with the session up, before anything is freed
    //  precision=s: the rows carry whole seconds. ns would invent digits.
    s_pushSsl.print(F("POST /api/v2/write?bucket=")); s_pushSsl.print(INFLUX_BUCKET);
    s_pushSsl.print(F("&precision=s HTTP/1.1\r\nHost: ")); s_pushSsl.print(INFLUX_HOST);
    s_pushSsl.print(F("\r\nAuthorization: Token ")); s_pushSsl.print(INFLUX_TOKEN);
    s_pushSsl.print(F("\r\nContent-Type: text/plain; charset=utf-8\r\nAccept: application/json"
                      "\r\nConnection: close\r\nContent-Length: ")); s_pushSsl.print((unsigned long)len);
    s_pushSsl.print(F("\r\n\r\n"));
    s_pushSsl.write((const uint8_t*)body, len);
    s_pushSsl.flush();
    //  Status line only: "HTTP/1.1 204 No Content".
    code = -3;
    unsigned long tw = millis();
    while (millis() - tw < INFLUX_RESP_TIMEOUT_MS) {
      if (s_pushSsl.available()) {
        //  "HTTP/1.1 204 No Content" -- status line into a fixed buffer (no
        //  String), the three digits after the first space are the code.
        char head[48]; size_t hn = 0;
        unsigned long th = millis();
        while (millis() - th < 1000) {
          int ch = s_pushSsl.available() ? s_pushSsl.read() : -1;
          if (ch < 0) { if (!s_pushSsl.connected()) break; delay(2); continue; }
          if (ch == '\n') break;
          if (hn < sizeof head - 1) head[hn++] = (char)ch;
        }
        head[hn] = 0;
        const char* sp = strchr(head, ' ');
        if (sp && sp[1] >= '0' && sp[1] <= '9') code = atoi(sp + 1);
        break;
      }
      if (!s_pushSsl.connected()) break;
      delay(10);
    }
  }
  s_pushSsl.stop();
  s_pushMs   = (int)(millis() - t0);
  s_pushCode = code;
  if (code >= 200 && code < 300) { s_pushOks++; s_pushWait = INFLUX_SUCCESS_GAP_MS; }
  else {
    s_pushFails++;
    //  Auth/bucket problems (401/403/404) cannot be retried into success:
    //  go straight to the 15 min ceiling instead of hammering.
    if (code == 401 || code == 403 || code == 404) s_pushWait = INFLUX_BACKOFF_MAX_MS;
    else {
      if (s_pushWait < INFLUX_BACKOFF_MS) s_pushWait = INFLUX_BACKOFF_MS; else s_pushWait *= 2;
      if (s_pushWait > INFLUX_BACKOFF_MAX_MS) s_pushWait = INFLUX_BACKOFF_MAX_MS;
    }
  }
  LOG("[PUSH] "); LOG((unsigned long)len); LOG(" B, code "); LOG(code); LOG(", "); LOG(s_pushMs); LOGLN(" ms");
  return code;
}

//  One-shot, a minute after the cloud is up. Writes to opta_selftest, never
//  to arduino_iot. Published as pushStat so it can be read on the dashboard
//  without a serial port.
static unsigned long s_selfTestArmedAt = 0;
static bool          s_selfTestDone    = false;

static void influxSelfTest(unsigned long now, bool cloudUp) {
  SHARED_ASSERT_ON_CLOUD();
  if (s_selfTestDone) return;
  if (!cloudUp) { s_selfTestArmedAt = 0; return; }
  if (s_selfTestArmedAt == 0) { s_selfTestArmedAt = now; return; }
  if (now - s_selfTestArmedAt < INFLUX_SELFTEST_AFTER_MS) return;
  if (!influxConfigured()) { s_selfTestDone = true; pushStatSet("not configured (placeholder token)"); return; }
  if (!influxPushReady(now)) return;
  time_t epoch = time(nullptr);
  if (epoch < (time_t)1600000000L) return;         // RTC not set yet: no timestamp, no row

  struct mallinfo before = mallinfo();
  char line[192];
  int n = snprintf(line, sizeof line, "opta_selftest,thing_name=%s heapFree=%lui,uptimeS=%lui,fw=\"%s\" %lu\n",
                   INFLUX_THING_NAME, (unsigned long)before.fordblks, (unsigned long)(now / 1000UL),
                   FW_VERSION, (unsigned long)epoch);
  if (n <= 0 || (size_t)n >= sizeof line) { s_selfTestDone = true; pushStatSet("selftest line too long"); return; }
  int code = influxPush(line, (size_t)n);
  struct mallinfo after = mallinfo();
  s_selfTestDone = true;

  char st[112];
  snprintf(st, sizeof st, "%s code=%d ms=%d heap %lu>%lu>%lu n=%lu/%lu",
           (code >= 200 && code < 300) ? "ok" : "FAIL", code, s_pushMs,
           (unsigned long)before.fordblks, (unsigned long)s_pushHeapPeakFree, (unsigned long)after.fordblks,
           (unsigned long)s_pushOks, (unsigned long)s_pushFails);
  pushStatSet(st);
  LOG("[PUSH] selftest: "); LOGLN(st);
}

#endif // INFLUX_PUSH_H
