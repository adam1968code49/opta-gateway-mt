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

//  A board built from secrets.h.example pushes nothing rather than
//  retrying a 401 forever.
inline bool influxConfigured() { return strncmp(INFLUX_TOKEN, "PUT_YOUR", 8) != 0; }

inline bool influxPushReady(unsigned long now) {
  if (!influxConfigured())                          return false;
  if (WiFi.status() != WL_CONNECTED)                return false;
  if (!ArduinoCloud.connected())                    return false;
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
        String head = s_pushSsl.readStringUntil('\n');
        int sp = head.indexOf(' ');
        if (sp > 0) code = head.substring(sp + 1, sp + 4).toInt();
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
  if (!influxConfigured()) { s_selfTestDone = true; pushStat = "not configured (placeholder token)"; return; }
  if (!influxPushReady(now)) return;
  time_t epoch = time(nullptr);
  if (epoch < (time_t)1600000000L) return;         // RTC not set yet: no timestamp, no row

  struct mallinfo before = mallinfo();
  char line[192];
  int n = snprintf(line, sizeof line, "opta_selftest,thing_name=%s heapFree=%lui,uptimeS=%lui,fw=\"%s\" %lu\n",
                   INFLUX_THING_NAME, (unsigned long)before.fordblks, (unsigned long)(now / 1000UL),
                   FW_VERSION, (unsigned long)epoch);
  if (n <= 0 || (size_t)n >= sizeof line) { s_selfTestDone = true; pushStat = "selftest line too long"; return; }
  int code = influxPush(line, (size_t)n);
  struct mallinfo after = mallinfo();
  s_selfTestDone = true;

  char st[112];
  snprintf(st, sizeof st, "%s code=%d ms=%d heap %lu>%lu>%lu n=%lu/%lu",
           (code >= 200 && code < 300) ? "ok" : "FAIL", code, s_pushMs,
           (unsigned long)before.fordblks, (unsigned long)s_pushHeapPeakFree, (unsigned long)after.fordblks,
           (unsigned long)s_pushOks, (unsigned long)s_pushFails);
  pushStat = String(st);
  LOG("[PUSH] selftest: "); LOGLN(st);
}

#endif // INFLUX_PUSH_H
