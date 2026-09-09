#ifndef FLOW_METER_H
#define FLOW_METER_H

// =====================================================================
//  Pulse flow meter on Opta I1. The ISR only counts. Everything else --
//  rate, running total, batch detection, the commit journal -- runs on
//  the PLC thread inside flowTick(), so the litres that must be added to
//  the PLC's lifetime total are debited in the same thread that performs
//  the write, and only after it succeeded. flowInit() is the one call
//  made from main: setup() arms the interrupt before anything slow.
//
//  Calibration, batch thresholds and the reason the batch counter is
//  zeroed at the START of the next discharge are in config.h (FLOW block).
// =====================================================================

#include <Arduino.h>
#include "config.h"
#include "shared.h"

static volatile uint32_t s_flowPulses    = 0;   // ISR-owned; 32-bit read is atomic on STM32
static uint32_t          s_flowLastCount = 0;   // PLC thread: snapshot at the previous tick
static unsigned long     s_flowLastMs    = 0;

static void flowIsr() { s_flowPulses++; }

//  MAIN THREAD, once, early in setup().
inline void flowInit() {
  pinMode(FLOW_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(FLOW_PIN), flowIsr, FALLING);
  s_flowLastMs = millis();
}

//  Raw ISR count, same single aligned 32-bit load flowTick() uses. For
//  flow_watch.h, which meters one discharge by pulse delta and must not
//  disturb flowTick()'s own s_flowLastCount bookkeeping.
inline uint32_t flowPulses() { return s_flowPulses; }

// ---- commit journal: litres measured but not yet in the PLC's total -----
static float s_uncommittedL  = 0.0f;
static bool  s_pendingCommit = false;

inline float flowUncommitted()  { return s_uncommittedL; }
inline bool  flowCommitReady()  { return s_pendingCommit && s_uncommittedL > 0.0f; }
inline void  flowCommitDone(float committedL) {
  s_uncommittedL -= committedL;
  if (s_uncommittedL < 0.0f) s_uncommittedL = 0.0f;   // float slop only
  s_pendingCommit = false;
}
inline void flowReset(PlcSnapshot& w) {
  w.flowTotal     = 0.0f;
  w.flowBatch     = 0.0f;
  w.waterOwedL    = 0.0f;
  s_uncommittedL  = 0.0f;
  s_pendingCommit = false;
}

//  PLC THREAD, every tick.
static void flowTick(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  unsigned long now = millis();
  unsigned long dt  = now - s_flowLastMs;
  if (dt < 500) return;                       // too short an interval to rate

  uint32_t count = s_flowPulses;              // single aligned 32-bit load
  uint32_t delta = count - s_flowLastCount;   // wrap-safe unsigned math
  s_flowLastCount = count;
  s_flowLastMs    = now;

  float hz     = (float)delta * 1000.0f / (float)dt;
  float litres = (float)delta / (FLOW_K_HZ_PER_LPM * 60.0f);
  w.flowRate   = hz / FLOW_K_HZ_PER_LPM;
  w.flowTotal += litres;

  //  Batch: zeroed when the NEXT discharge starts, so the display keeps
  //  answering "the batch just finished" through the idle gap.
  static bool          inBatch    = false;
  static unsigned long lastFlowMs = 0;
  if (w.flowRate > FLOW_BATCH_ON_LPM) {
    if (!inBatch) { w.flowBatch = 0.0f; inBatch = true; }
    lastFlowMs = now;
  } else if (inBatch && (now - lastFlowMs) >= FLOW_BATCH_IDLE_MS) {
    inBatch         = false;
    s_pendingCommit = true;                   // hand this discharge to the PLC commit
  }
  if (inBatch) {
    w.flowBatch    += litres;
    s_uncommittedL += litres;
  }
  w.waterOwedL = s_uncommittedL;
}

#endif // FLOW_METER_H
