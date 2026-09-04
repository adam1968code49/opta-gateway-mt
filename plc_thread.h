#ifndef PLC_THREAD_H
#define PLC_THREAD_H

// =====================================================================
//  THE PLC THREAD. Owns eip. Nobody else calls it.
//
//  Every 2 s: reconnect if needed (knocking first with plcProbe so a
//  powered-off PLC costs 0.3 s, not 28), sweep the 28 sensor tags into
//  the working snapshot, drain the command queue, publish. Blocking in
//  here -- the eip connect, CIP I/O -- is now this thread's problem alone.
//  Main keeps publishing to the cloud while this thread waits on a socket.
// =====================================================================

#include <mbed.h>
#include "rtos/Thread.h"
#include "config.h"
#include "EtherNetIP.h"
#include "plc_probe.h"
#include "plc_tags.h"
#include "shared.h"
#include "stack_watch.h"
#include "wd_feeder.h"

#define PLC_THREAD_STACK   16384

extern EtherNetIPClient eip;       // defined in the .ino, as before
extern IPAddress        plcIp;

static StackWatch s_plcStack;
inline uint32_t plcStackMinFree() { return s_plcStack.minFree(); }

// ---------------------------------------------------------------------
//  Read every sensor tag into w.sensor[] / w.ok[]. Unchanged algorithm:
//  22 REALs in batched MSP reads, 6 DINTs singly with scaling, scattered
//  into canonical slots.
// ---------------------------------------------------------------------
static void pollSensorsInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  static float rvals[N_REAL];
  static bool  rok[N_REAL];

  for (size_t k = 0; k < N_SENSORS; k++) w.ok[k] = false;

  eip.readRealsMultiple(REAL_TAGS, rvals, rok, N_REAL);
  for (size_t k = 0; k < N_REAL; k++) {
    w.sensor[REAL_SLOT[k]] = rvals[k];
    w.ok    [REAL_SLOT[k]] = rok[k];
  }

  for (size_t k = 0; k < N_DINT; k++) {
    int32_t raw = 0;
    if (eip.readDint(DINT_TAGS[k], raw)) {
      w.sensor[DINT_SLOT[k]] = (float)raw * DINT_SCALE[k] + DINT_OFFSET[k];
      w.ok    [DINT_SLOT[k]] = true;
    }
  }

  size_t good = 0;
  for (size_t k = 0; k < N_SENSORS; k++) if (w.ok[k]) good++;
  w.fails = (int32_t)N_SENSORS - (int32_t)good;
  w.lastCipStatus = eip.lastCipStatus();

  //  Name the failing tags only when the pattern changes, not every tick.
  static uint32_t lastFailMask = 0;
  uint32_t failMask = 0;
  for (size_t k = 0; k < N_SENSORS; k++) if (!w.ok[k]) failMask |= (1UL << k);
  if (failMask != lastFailMask) {
    lastFailMask = failMask;
    for (size_t k = 0; k < N_SENSORS; k++)
      if (!w.ok[k]) { LOG("[EIP] read FAIL: "); LOGLN(SENSOR_TAGS[k]); }
  }

  if (w.fails == 0) {
    w.plcConnected = true;
    snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "ok");
  } else {
    LOG("[EIP] "); LOG(w.fails); LOG(" tag reads failed  cip=0x");
    LOG(eip.lastCipStatus(), HEX); LOG(" extLen="); LOGLN(eip.lastExtStatusLen());
    snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "read fail x%ld (cip=0x%X)",
             (long)w.fails, (unsigned)eip.lastCipStatus());
    if (w.fails >= (int32_t)N_SENSORS) { eip.end(); w.plcConnected = false; }
  }
}

// ---------------------------------------------------------------------
//  Batch 0 drains the queue and logs. Batch 1 replaces the LOG with the
//  eip.write* per CmdTag. Draining now keeps the queue from filling if
//  anything posts to it early.
// ---------------------------------------------------------------------
static void drainCommands() {
  Cmd c;
  while (sharedCmdTake(c)) {
    LOG("[PLC] cmd tag="); LOG(c.tag); LOG(" value="); LOGLN(c.value);
  }
}

static void plcThreadBody() {
  g_plcThreadId = osThreadGetId();
  s_plcStack.begin();          // shallowest point of this stack
  PlcSnapshot w = {};
  unsigned long reconnectWait = RECONNECT_BACKOFF_MS;
  unsigned long lastReconnect = 0;
  LOGLN("[PLC] thread started");

  for (;;) {
    unsigned long tick0 = millis();
    wdBeatPlc();

    if (!eip.connected()) {
      w.plcConnected = false;
      if (tick0 - lastReconnect >= reconnectWait) {
        lastReconnect = tick0;
        wdWherePlc(WD_AT_PLCPROBE);
        unsigned long eipT0 = millis();
        bool eipOk = false;
        if (plcProbe(plcIp, PLC_ENIP_PORT)) {
          wdWherePlc(WD_AT_EIPCONN);
          eipOk = eip.begin();
        } else {
          snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "plc not listening (probe)");
        }
        w.eipMs = (int32_t)(millis() - eipT0);
        if (eipOk) {
          w.plcConnected = true;
          snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "ok");
          reconnectWait = RECONNECT_BACKOFF_MS;
          LOGLN("[EIP] session OK");
        } else {
          reconnectWait *= 2;
          if (reconnectWait > RECONNECT_BACKOFF_MAX_MS) reconnectWait = RECONNECT_BACKOFF_MAX_MS;
          LOG("[EIP] reconnect failed, next in "); LOG(reconnectWait / 1000); LOGLN(" s");
        }
      }
    } else {
      wdWherePlc(WD_AT_SENSORS);
      pollSensorsInto(w);
      wdWherePlc(WD_AT_CIPWRITE);
      drainCommands();
    }

    s_plcStack.sample();
    w.plcStackFree = s_plcStack.minFree();
    w.cmdDropped   = g_cmdDropped;

    wdWherePlc(WD_AT_PLCPUBLISH);
    sharedPublish(w);
    wdWherePlc(WD_AT_NONE);

    //  Sleep out the remainder of the tick. A pass that overran (a slow
    //  connect) simply starts the next one immediately.
    unsigned long spent = millis() - tick0;
    if (spent < SAMPLE_INTERVAL_MS)
      rtos::ThisThread::sleep_for(std::chrono::milliseconds(SAMPLE_INTERVAL_MS - spent));
  }
}

inline void plcThreadBegin() {
  static rtos::Thread t(osPriorityNormal, PLC_THREAD_STACK, nullptr, "plc");
  t.start(mbed::callback(plcThreadBody));
  LOG("[PLC] thread launched, stack "); LOG(PLC_THREAD_STACK); LOGLN(" B");
}

#endif // PLC_THREAD_H
