#ifndef WD_FEEDER_H
#define WD_FEEDER_H

// =====================================================================
//  Feeds the watchdog ArduinoIoTCloud arms, watching THREE threads.
//
//  ArduinoIoTCloud enables the hardware watchdog by default and refreshes
//  it only inside ArduinoCloud.update(), on a 32.76 s window. Three
//  threads each stamp their own beat and each has its own budget: main
//  and plc 60 s (their I/O is bounded), cloud 300 s (a TLS stall on a bad
//  uplink is a gap in telemetry, not a reason to reboot the gateway; only
//  a cloud thread that never comes back is). Over budget the feeder stops
//  kicking, writes a KVStore marker so the next bootReason names the
//  thread and section, and resets the board itself (the cloud library
//  also kicks the watchdog from inside update(), so merely stopping our
//  kicks would not be enough while the cloud thread is healthy).
//
//  Two stall figures: the lifetime maximum (serial), and a 10-minute
//  sliding window maximum (published), so a 6 s boot connect does not pin
//  loopStallMs forever and hide a 20 s stall a day later.
// =====================================================================

#include <mbed.h>
#include "drivers/Watchdog.h"
#include "boot_reason.h"

#define WD_FEED_PERIOD_MS    2000
#define WD_STALL_GIVEUP_MS   60000     // main and plc
#define WD_CLOUD_GIVEUP_MS   300000    // cloud
#define WD_STALL_WINDOW_MS   600000    // published maximum looks back this far
#define WD_FEEDER_STACK      6144      // bootMarkIntentional goes through KVStore/QSPI

//  Where each thread is. 1 main; 2,4,12,14,15,16 cloud; 5-11,13 plc.
#define WD_AT_NONE       0
#define WD_AT_WEB        1    // handleConfigClient()          main
#define WD_AT_CLOUD      2    // ArduinoCloud.update(), connected   cloud
#define WD_AT_USB        3    // usbLogService()               (later)
#define WD_AT_PUSH       4    // influxSelfTest()              cloud (later)
#define WD_AT_EIPCONN    5    // eip.begin()                   plc
#define WD_AT_SENSORS    6    // pollSensors()                 plc
#define WD_AT_VALVES     7    // pollValves()                  plc
#define WD_AT_PLCSTATE   8    // pollPlcState()                plc
#define WD_AT_HEATPUMP   9    // pollHeatPump()                plc
#define WD_AT_CIPWRITE  10    // command drain / water writes  plc
#define WD_AT_PLCPROBE  11    // plcProbe()                    plc
#define WD_AT_CLOUDPROBE 12   // cloudProbeOnce()              cloud
#define WD_AT_PLCPUBLISH 13   // sharedPublish()               plc
#define WD_AT_WIFI       14   // update() with WiFi down: re-association   cloud
#define WD_AT_CLOUDCONN  15   // update() WiFi up, cloud down: DNS/TLS/NTP  cloud
#define WD_AT_DIAG       16   // heartbeat print + 30 s diagnostics (WiFi.RSSI)  cloud

static volatile uint8_t  s_whereMain   = WD_AT_NONE;   // written by main only
static volatile uint8_t  s_wherePlc    = WD_AT_NONE;   // written by plc only
static volatile uint8_t  s_whereCloud  = WD_AT_NONE;   // written by cloud only
static volatile uint32_t s_mainBeatMs  = 0;
static volatile uint32_t s_plcBeatMs   = 0;
static volatile uint32_t s_cloudBeatMs = 0;
static volatile uint32_t s_stallMaxMs  = 0;            // lifetime
static volatile uint8_t  s_stallWhere  = WD_AT_NONE;
static volatile uint32_t s_winMaxMs    = 0;            // current 10 min window
static volatile uint8_t  s_winWhere    = WD_AT_NONE;
static volatile uint32_t s_prevWinMaxMs = 0;           // previous window
static volatile uint8_t  s_prevWinWhere = WD_AT_NONE;
static volatile uint32_t s_wdRefusals  = 0;
static volatile bool     s_wdSeen      = false;
static volatile bool     s_markerDone  = false;

inline void     wdWhereMain(uint8_t w)  { s_whereMain  = w; }
inline void     wdWherePlc(uint8_t w)   { s_wherePlc   = w; }
inline void     wdWhereCloud(uint8_t w) { s_whereCloud = w; }
inline void     wdBeatMain()   { s_mainBeatMs  = millis(); }
inline void     wdBeatPlc()    { s_plcBeatMs   = millis(); }
inline void     wdBeatCloud()  { s_cloudBeatMs = millis(); }
inline uint32_t wdStallMax()   { return s_stallMaxMs; }
inline uint8_t  wdStallWhere() { return s_stallWhere; }
inline uint32_t wdRefusals()   { return s_wdRefusals; }
inline bool     wdSeen()       { return s_wdSeen; }
//  Published figures: the larger of the current and the previous window,
//  so a stall is visible for at least 10 minutes and at most 20.
inline uint32_t wdStallWindowMax()   { return (s_winMaxMs >= s_prevWinMaxMs) ? s_winMaxMs : s_prevWinMaxMs; }
inline uint8_t  wdStallWindowWhere() { return (s_winMaxMs >= s_prevWinMaxMs) ? s_winWhere : s_prevWinWhere; }

static void wdFeederLoop() {
  uint32_t t0 = millis();
  s_mainBeatMs = s_plcBeatMs = s_cloudBeatMs = t0;   // so the first pass is not a false stall
  uint32_t winStart = t0;
  for (;;) {
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(WD_FEED_PERIOD_MS));
    uint32_t now = millis();
    uint32_t sinceMain  = now - s_mainBeatMs;
    uint32_t sincePlc   = now - s_plcBeatMs;
    uint32_t sinceCloud = now - s_cloudBeatMs;

    //  The worst of the three, blamed on the thread whose beat is oldest.
    uint32_t since = sinceMain;  uint8_t where = s_whereMain;
    if (sincePlc   > since) { since = sincePlc;   where = s_wherePlc;   }
    if (sinceCloud > since) { since = sinceCloud; where = s_whereCloud; }

    if (since > s_stallMaxMs) { s_stallMaxMs = since; s_stallWhere = where; }

    if (now - winStart >= WD_STALL_WINDOW_MS) {
      s_prevWinMaxMs = s_winMaxMs;  s_prevWinWhere = s_winWhere;
      s_winMaxMs = 0;               s_winWhere = WD_AT_NONE;
      winStart = now;
    }
    if (since > s_winMaxMs) { s_winMaxMs = since; s_winWhere = where; }

    //  Checked every pass, not at startup: the watchdog is armed by the
    //  cloud state machine inside update(), so at the end of setup() it
    //  is not running yet.
    if (!mbed::Watchdog::get_instance().is_running()) continue;
    s_wdSeen = true;

    bool giveUp = sinceMain  > WD_STALL_GIVEUP_MS
               || sincePlc   > WD_STALL_GIVEUP_MS
               || sinceCloud > WD_CLOUD_GIVEUP_MS;
    if (!giveUp) {
      mbed::Watchdog::get_instance().kick();
      continue;
    }
    //  Give up. Say why, once, where the next boot will find it; the
    //  hardware resets the board within 32.76 s of the last kick.
    //  Blame the thread that is furthest past ITS OWN budget -- not the
    //  largest absolute stall: a cloud thread 250 s into a legal 300 s
    //  reconnect must not take the name of a main thread wedged at 61 s.
    if (!s_markerDone) {
      s_markerDone = true;
      uint32_t rMain  = sinceMain  / (WD_STALL_GIVEUP_MS / 1000);   // per-mille-ish ratio, integer
      uint32_t rPlc   = sincePlc   / (WD_STALL_GIVEUP_MS / 1000);
      uint32_t rCloud = sinceCloud / (WD_CLOUD_GIVEUP_MS / 1000);
      uint32_t bSince = sinceMain;  uint8_t bWhere = s_whereMain;  const char* bWho = "main";  uint32_t r = rMain;
      if (rPlc   > r) { bSince = sincePlc;   bWhere = s_wherePlc;   bWho = "plc";   r = rPlc;   }
      if (rCloud > r) { bSince = sinceCloud; bWhere = s_whereCloud; bWho = "cloud"; }
      char tag[48];
      snprintf(tag, sizeof tag, "wd giveup @%u %s %lus", (unsigned)bWhere, bWho, (unsigned long)(bSince / 1000));
      bootMarkIntentional(tag);      // marker first: LOG takes g_logMutex, which the wedged thread may hold
      LOG("[WD] "); LOGLN(tag);
      //  Reset ourselves. ArduinoIoTCloud kicks the hardware watchdog on
      //  its own inside update() (ArduinoIoTCloudTCP.cpp watchdog_reset),
      //  so with a healthy cloud thread "stop kicking and wait" would wait
      //  forever. The marker is in QSPI; nothing else is worth saving.
      delay(50);
      NVIC_SystemReset();
    }
    s_wdRefusals++;
  }
}

inline void wdFeederBegin() {
  static rtos::Thread t(osPriorityHigh, WD_FEEDER_STACK, nullptr, "wdfeed");
  t.start(mbed::callback(wdFeederLoop));
  LOG("[WD] feeder started, kick every "); LOG(WD_FEED_PERIOD_MS);
  LOG(" ms; give up main/plc "); LOG(WD_STALL_GIVEUP_MS / 1000);
  LOG(" s, cloud "); LOG(WD_CLOUD_GIVEUP_MS / 1000); LOGLN(" s");
}

#endif // WD_FEEDER_H
