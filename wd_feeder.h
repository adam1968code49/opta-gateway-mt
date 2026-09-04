#ifndef WD_FEEDER_H
#define WD_FEEDER_H

// =====================================================================
//  Feeds the watchdog ArduinoIoTCloud arms, watching BOTH threads.
//
//  ArduinoIoTCloud enables the hardware watchdog by default and refreshes
//  it only inside ArduinoCloud.update(), on a 32.76 s window. On the old
//  single-threaded firmware any blocking call after update() ate that
//  window; measured: eip.begin() 27.7 s, cloud TLS reconnect 110 s.
//
//  Two threads now. Each stamps its own beat. The stall is the OLDER of
//  the two beats, so a wedge in either thread is what the feeder sees.
//  Under WD_STALL_GIVEUP_MS it kicks; over it, it stops, and the hardware
//  resets the board -- a hang that never returns is worse than a restart.
//  The maximum stall and the section code at that moment are kept for
//  publication by main.
// =====================================================================

#include <mbed.h>
#include "drivers/Watchdog.h"

#define WD_FEED_PERIOD_MS   2000
#define WD_STALL_GIVEUP_MS  60000

//  Where each thread is. Codes 1,2,4,12 occur only on main; 5-11,13 only
//  on the PLC thread; so the code names the thread as well as the section.
#define WD_AT_NONE       0
#define WD_AT_WEB        1    // handleConfigClient()          main
#define WD_AT_CLOUD      2    // ArduinoCloud.update()         main
#define WD_AT_USB        3    // usbLogService()               (batch 6)
#define WD_AT_PUSH       4    // influxSelfTest()              main
#define WD_AT_EIPCONN    5    // eip.begin()                   plc
#define WD_AT_SENSORS    6    // pollSensors()                 plc
#define WD_AT_VALVES     7    // pollValves()                  plc (batch 1)
#define WD_AT_PLCSTATE   8    // pollPlcState()                plc (batch 1)
#define WD_AT_HEATPUMP   9    // pollHeatPump()                plc (batch 5)
#define WD_AT_CIPWRITE  10    // command drain / water writes  plc
#define WD_AT_PLCPROBE  11    // plcProbe()                    plc
#define WD_AT_CLOUDPROBE 12   // cloudProbeOnce()              main
#define WD_AT_PLCPUBLISH 13   // sharedPublish()               plc

static volatile uint8_t  s_where       = WD_AT_NONE;
static volatile uint8_t  s_stallWhere  = WD_AT_NONE;
static volatile uint32_t s_mainBeatMs  = 0;
static volatile uint32_t s_plcBeatMs   = 0;
static volatile uint32_t s_stallMaxMs  = 0;
static volatile uint32_t s_wdRefusals  = 0;
static volatile bool     s_wdSeen      = false;

inline void     wdWhere(uint8_t w)  { s_where = w; }
inline void     wdBeatMain()        { s_mainBeatMs = millis(); }
inline void     wdBeatPlc()         { s_plcBeatMs  = millis(); }
inline uint32_t wdStallMax()        { return s_stallMaxMs; }
inline uint8_t  wdStallWhere()      { return s_stallWhere; }
inline uint32_t wdRefusals()        { return s_wdRefusals; }
inline bool     wdSeen()            { return s_wdSeen; }

static void wdFeederLoop() {
  uint32_t t0 = millis();
  s_mainBeatMs = t0;
  s_plcBeatMs  = t0;          // so the first pass is not a false stall
  for (;;) {
    rtos::ThisThread::sleep_for(std::chrono::milliseconds(WD_FEED_PERIOD_MS));
    uint32_t now = millis();
    uint32_t sinceMain = now - s_mainBeatMs;
    uint32_t sincePlc  = now - s_plcBeatMs;
    uint32_t since = (sinceMain > sincePlc) ? sinceMain : sincePlc;
    if (since > s_stallMaxMs) {
      s_stallMaxMs = since;
      s_stallWhere = s_where;
    }
    //  Checked every pass, not at startup: the watchdog is armed by the
    //  cloud state machine inside update(), so at the end of setup() it
    //  is not running yet.
    if (!mbed::Watchdog::get_instance().is_running()) continue;
    s_wdSeen = true;
    if (since < WD_STALL_GIVEUP_MS) {
      mbed::Watchdog::get_instance().kick();
    } else {
      s_wdRefusals++;
    }
  }
}

inline void wdFeederBegin() {
  static rtos::Thread t(osPriorityHigh, 2048, nullptr, "wdfeed");
  t.start(mbed::callback(wdFeederLoop));
  LOG("[WD] feeder started, kick every "); LOG(WD_FEED_PERIOD_MS);
  LOG(" ms, give up after "); LOG(WD_STALL_GIVEUP_MS); LOGLN(" ms, watching main+plc");
}

#endif // WD_FEEDER_H
