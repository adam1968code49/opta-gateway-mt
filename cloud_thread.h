#ifndef CLOUD_THREAD_H
#define CLOUD_THREAD_H

// =====================================================================
//  THE CLOUD THREAD. Owns every Cloud* property and ArduinoCloud.update().
//
//  Everything that used to be the cloud half of loop() lives here: take
//  the PLC snapshot, assign Cloud*, run the probe-gated update() (the
//  seven control callbacks fire inside it, on this thread), the serial
//  heartbeat, the 30 s diagnostics. A TLS stall on a bad uplink now blocks
//  this thread only; main keeps the web page and the panel lights alive,
//  the PLC thread keeps the machine's data flowing, and the feeder gives
//  this thread 300 s before it gives up.
// =====================================================================

#include <mbed.h>
#include "rtos/Thread.h"
#include <malloc.h>
#include <WiFi.h>
#include "config.h"
#include "thingProperties.h"
#include "shared.h"
#include "stack_watch.h"
#include "wd_feeder.h"
#include "cloud_probe.h"
#include "cloud_side.h"
#include "cloud_ctrl.h"

#define CLOUD_THREAD_STACK   24576   // TLS + OTA (second TLS, HTTP, LZSS, FATFS) run here; 16 KB was measured only without OTA
#define CLOUD_PASS_SLEEP_MS  20
#define CLOUD_OFFLINE_RESET_MS      900000    // WiFi up but no cloud for this long -> a fresh boot re-resolves everything...
#define CLOUD_OFFLINE_RESET_HARD_MS 1200000   // ...waiting for the water ledger to be flat, but no longer than this

#if WIFI_FORCE_SECURITY
static void wifiRescue(unsigned long now);   // defined later in the .ino
#endif

static StackWatch s_cloudStack;
static volatile uint32_t s_mainStackMin = 0;   // main reports its own watermark for the [HB] line
inline void cloudThreadReportMainStack(uint32_t v) { s_mainStackMin = v; }

static void cloudThreadBody() {
  g_cloudThreadId = osThreadGetId();
  s_cloudStack.begin();          // shallowest point of this stack
  uint32_t      lastSeq  = 0;
  unsigned long lastWarn = 0, lastHb = 0, lastDiag = 0;
  unsigned long cloudDownSince = 0;      // first pass that saw WiFi up + cloud down; 0 = not in that state
  LOGLN("[CLOUD] thread started");

  for (;;) {
    unsigned long pass0 = millis();
    wdBeatCloud();
    wdWhereCloud(WD_AT_SNAP);           // snapshot copy, 141 Cloud* assignments, status reads

    // ---- take the PLC snapshot, assign Cloud* once per PLC tick -------------
    cloudSideConsume(lastSeq);
    bool plcStalled = cloudSideHasSnapshot() && cloudSideSnapshotAgeMs() > 3 * SAMPLE_INTERVAL_MS;
    if (plcStalled && millis() - lastWarn > 10000) {
      lastWarn = millis();
      char b[PlcSnapshot::LASTERR_CAP];
      snprintf(b, sizeof b, "plc thread stalled %lus", (unsigned long)(cloudSideSnapshotAgeMs() / 1000));
      lastError = String(b);
      LOGLN(b);
    }

    // ---- cloud, gated only in the (WiFi up, cloud down) state ---------------
    unsigned long cloudT0 = millis();
    bool wifiUp  = WiFi.status() == WL_CONNECTED;
    bool cloudUp = ArduinoCloud.connected();
    wdWhereCloud(WD_AT_CLOUDPROBE);
    bool runCloud = cloudUpdateAllowed(wifiUp, cloudUp, cloudT0);
    //  Say which road update() is about to take, so a stall names it.
    wdWhereCloud(!wifiUp ? WD_AT_WIFI : (!cloudUp ? WD_AT_CLOUDCONN : WD_AT_CLOUD));
    if (runCloud) ArduinoCloud.update();
    cloudMs = (int)(millis() - cloudT0);
    //  Last resort against "alive but never reconnects": a quarter hour of
    //  WiFi up and cloud down ends in a marked reset. Not when WiFi itself
    //  is down -- a reboot would not bring an access point back.
    if (cloudUp || !wifiUp)             cloudDownSince = 0;
    else if (cloudDownSince == 0)       cloudDownSince = cloudT0;
    else if (cloudT0 - cloudDownSince >= CLOUD_OFFLINE_RESET_MS
             && ((float)waterOwedL == 0.0f || cloudT0 - cloudDownSince >= CLOUD_OFFLINE_RESET_HARD_MS)) {
      //  Wait for the water ledger to be flat (an in-flight batch would be
      //  lost for good), but not forever. No LOG here: the marker is the
      //  record, and a blocked serial port must not delay the reset.
      bootMarkIntentional("cloud offline 15min");
      delay(50);
      NVIC_SystemReset();
    }
#if WIFI_FORCE_SECURITY
    wdWhereCloud(WD_AT_WIFI);          // its WiFi.begin() is the same road as 14
    wifiRescue(millis());
#endif
    wdWhereCloud(WD_AT_DIAG);          // heartbeat print and the 30 s block (WiFi.RSSI is a driver call)

    unsigned long now = millis();

    // ---- panel lights, handed to main as one byte ----------------------------
    bool fault = !(bool)plcConnected || plcStalled
              || (bool)pressError || (bool)tempError || (bool)genError;
    g_ledBits = ((bool)plcConnected            ? LED_BIT_PLC   : 0)
              | (ArduinoCloud.connected()      ? LED_BIT_CLOUD : 0)
              | (fault                         ? LED_BIT_FAULT : 0);

    // ---- heartbeat, 3 s, serial only ----------------------------------------
#if ENABLE_SERIAL_DEBUG
    if (now - lastHb >= 3000) {
      lastHb = now;
      LOG("[HB] wifi=");  LOG(wifiUp ? "up" : "down");
      LOG(" cloud=");     LOG(ArduinoCloud.connected() ? "up" : "down");
      LOG(" plc=");       LOG((bool)plcConnected ? "1" : "0");
      LOG(" seqAge=");    LOG(cloudSideSnapshotAgeMs());
      LOG(" cloudMs=");   LOG((int)cloudMs);
      LOG(" eipMs=");     LOG((int)eipMs);
      LOG(" stall=");     LOG(wdStallMax()); LOG("@"); LOG(wdStallWhere());
      LOG(" win=");       LOG(wdStallWindowMax()); LOG("@"); LOG(wdStallWindowWhere());
      LOG(" probe=");     LOG(cloudProbeOks()); LOG("/"); LOG(cloudProbeFails()); LOG(" failopen="); LOG(cloudFailOpens()); LOG(" rr="); LOG(cloudReresolves());
      LOG(" mainStk=");   LOG(s_mainStackMin);
      LOG(" cloudStk=");  LOG(s_cloudStack.minFree());
      LOG(" plcStk=");    LOG(cloudSidePlcStackFree());
      LOG(" t1=");        LOG((float)t1HotTank);
      LOG(" lvl=");       LOG((float)tankLevel);
      LOGLN("");
    }
#endif

    // ---- diagnostics, 30 s --------------------------------------------------
    if (now - lastDiag >= DIAG_PUBLISH_MS) {
      lastDiag = now;
      struct mallinfo mi = mallinfo();
      heapUsed = (int)mi.uordblks;
      heapFree = (int)mi.fordblks;
      uptimeS  = (int)(now / 1000UL);
      s_cloudStack.sample();
      stackFree   = (int)s_cloudStack.minFree();      // batch 6: the cloud thread's stack -- TLS runs here
      loopStallMs = (int)wdStallWindowMax();          // 10-minute window, not lifetime
      stallWhere  = (int)wdStallWindowWhere();
      wifiRssi    = (int)WiFi.RSSI();
    }

    loopMs = (int)(millis() - pass0);   // the pass stays at code 16 through the sleep; 0 now means 'thread not running'
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(CLOUD_PASS_SLEEP_MS));
  }
}

inline void cloudThreadBegin() {
  static rtos::Thread t(osPriorityNormal, CLOUD_THREAD_STACK, nullptr, "cloud");
  t.start(mbed::callback(cloudThreadBody));
  LOG("[CLOUD] thread launched, stack "); LOG(CLOUD_THREAD_STACK); LOGLN(" B");
}

#endif // CLOUD_THREAD_H
