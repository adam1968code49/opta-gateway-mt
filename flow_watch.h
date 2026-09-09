#ifndef FLOW_WATCH_H
#define FLOW_WATCH_H

// =====================================================================
//  PLC THREAD ONLY. After every discharge, compare what the tank level
//  says left the tank with what the pulse meter says went past it.
//
//  2026-09-08 the hose from the condensate pump to the filter blew off
//  three discharges in a row: the pump ran, the level dropped 15-23 cm
//  as always, the meter counted nothing, and nobody knew until the floor
//  was wet. The meter was right -- no water passed it -- but no code put
//  the two facts side by side. This does, and says so in lastError.
//
//  Inputs are this tick's snapshot only (pump bit, level, plcConnected)
//  plus the meter's raw pulse count. Output is the snapshot only: a
//  count, and a latched text laid over lastError while it says "ok".
//  Thresholds and the measurements behind them: config.h, FLOW WATCH.
// =====================================================================

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include "config.h"
#include "plc_tags.h"
#include "shared.h"
#include "flow_meter.h"

enum FlowWatchState : uint8_t { FW_IDLE, FW_PUMPING, FW_SETTLING };

static FlowWatchState s_fwState       = FW_IDLE;
static bool           s_fwEnabled     = false;
static bool           s_fwPrevPump    = false;   // pump bit seen on the previous valid tick
static bool           s_fwPrevValid   = false;   // false until one valid tick: no edge on boot
static unsigned long  s_fwStartMs     = 0;       // pump rising edge
static unsigned long  s_fwPumpOffMs   = 0;       // pump falling edge
static float          s_fwLevelStart  = 0.0f;    // cm at the rising edge
static uint32_t       s_fwPulsesStart = 0;       // meter count at the rising edge
static int32_t        s_fwCount       = 0;       // mismatches since boot
static bool           s_fwLatched     = false;
static unsigned long  s_fwLatchUntil  = 0;
static char           s_fwText[PlcSnapshot::LASTERR_CAP] = "";

//  PLC THREAD, once, before the first tick. The slot macros are checked
//  against the tables so a reorder disables the watch instead of judging
//  the wrong tag.
inline void flowWatchInit() {
#if FLOW_WATCH_ENABLE
  if (strcmp(VALVE_TAGS[VSLOT_P_COND], TAG_P_COND) != 0 ||
      strcmp(SENSOR_TAGS[SSLOT_LEVEL], TAG_LEVEL) != 0) {
    LOGLN("[FLOW] slot map wrong, watch disabled");
    s_fwEnabled = false;
    return;
  }
  s_fwEnabled = true;
#else
  s_fwEnabled = false;
#endif
}

static void flowWatchAbort(const char* why) {
  LOG("[FLOW] watch aborted ("); LOG(why); LOGLN(")");
  s_fwState = FW_IDLE;
}

//  One finished discharge: judge it, log it, latch or release.
static void flowWatchJudge(PlcSnapshot& w, unsigned long now) {
  float dropCm = s_fwLevelStart - w.sensor[SSLOT_LEVEL];
  float litres = (float)(flowPulses() - s_fwPulsesStart) / (FLOW_K_HZ_PER_LPM * 60.0f);
  unsigned long pumpS = (s_fwPumpOffMs - s_fwStartMs) / 1000UL;
  bool mismatch = dropCm >= FLOW_WATCH_LEVEL_DROP_CM && litres < FLOW_WATCH_MIN_L;

  long dropCmI = (long)(dropCm + (dropCm >= 0.0f ? 0.5f : -0.5f));   // no %f: newlib-nano
  long mL      = (long)(litres * 1000.0f + 0.5f);

  LOG("[FLOW] pump "); LOG(pumpS); LOG("s level "); LOG(s_fwLevelStart, 1);
  LOG("->"); LOG(w.sensor[SSLOT_LEVEL], 1); LOG(" meter "); LOG(mL); LOG("mL ");
  LOGLN(mismatch ? "MISMATCH" : "ok");

  if (mismatch) {
    s_fwCount++;
    s_fwLatched    = true;
    s_fwLatchUntil = now + FLOW_WATCH_LATCH_MS;
    snprintf(s_fwText, sizeof s_fwText, "flow mismatch: lvl -%ldcm meter %ldmL x%ld",
             dropCmI, mL, (long)s_fwCount);
  } else if (dropCm >= FLOW_WATCH_LEVEL_DROP_CM) {
    s_fwLatched = false;                      // a real discharge went past the meter: fault cleared
  }
  //  A drop under the threshold judges nothing: the pump moved no water
  //  worth speaking of, so it neither raises nor clears a latch.
}

//  PLC THREAD, every tick, after the sweeps and before sharedPublish(w):
//  this tick's valve[]/sensor[]/plcConnected are final by then, and a
//  disconnected tick arrives with both ok[] arrays cleared.
static void flowWatchTick(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  w.flowMismatchCount = s_fwCount;
  if (!s_fwEnabled) return;
  unsigned long now = millis();

  bool pumpOk = w.plcConnected && w.valveOk[VSLOT_P_COND];
  bool pump   = pumpOk && w.valve[VSLOT_P_COND] > 0.5f;

  switch (s_fwState) {
    case FW_IDLE:
      if (pumpOk && s_fwPrevValid && !s_fwPrevPump && pump && w.ok[SSLOT_LEVEL]) {
        s_fwStartMs     = now;
        s_fwLevelStart  = w.sensor[SSLOT_LEVEL];
        s_fwPulsesStart = flowPulses();
        s_fwState       = FW_PUMPING;
      }
      break;

    case FW_PUMPING:
      if (!pumpOk) { flowWatchAbort("plc"); break; }
      if (!pump) { s_fwPumpOffMs = now; s_fwState = FW_SETTLING; break; }
      if (now - s_fwStartMs > FLOW_WATCH_PUMP_MAX_MS) flowWatchAbort("pump on too long");
      break;

    case FW_SETTLING:
      if (!pumpOk) { flowWatchAbort("plc"); break; }
      if (now - s_fwPumpOffMs >= FLOW_WATCH_SETTLE_MS) {
        s_fwState = FW_IDLE;
        if (!w.ok[SSLOT_LEVEL]) { flowWatchAbort("level unread"); break; }
        flowWatchJudge(w, now);
      }
      break;
  }

  //  Edge memory: only a tick that actually read the pump bit counts.
  if (pumpOk) { s_fwPrevPump = pump; s_fwPrevValid = true; }
  else        { s_fwPrevValid = false; }

  //  Overlay. Real errors (read fail, control refusal, probe) are not
  //  "ok" and win; the verdict shows only in the gaps between them.
  if (s_fwLatched) {
    if ((long)(now - s_fwLatchUntil) >= 0) s_fwLatched = false;
    else if (strcmp(w.lastError, "ok") == 0)
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s", s_fwText);
  }
  w.flowMismatchCount = s_fwCount;
}

#endif // FLOW_WATCH_H
