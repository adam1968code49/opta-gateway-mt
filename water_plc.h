#ifndef WATER_PLC_H
#define WATER_PLC_H

// =====================================================================
//  PLC THREAD ONLY. The Opta's water measurement INTO the PLC -- the old
//  writeProductFlow() moved thread, rules unchanged. Every rule below was
//  taught by a real incident; see the FLOW->PLC, RUNNING TOTALS, PROGRAM
//  DOWNLOAD PROTECTION, HMI RESET HANDSHAKE and HMI DISPLAY CADENCE blocks
//  in config.h for the reasoning.
//
//    product_flowrate / product_watervolume   every pass (5 s)
//    cumulative_watervolume                   STORAGE, per finished discharge
//    display_watervolume, trip_watervolume    HMI, once an hour (wall clock)
//    trip_history_1..5                        once an hour, real boundaries only
//
//  Wall clock comes from time(nullptr): ArduinoIoTCloud's TimeService
//  writes NTP into the mbed RTC (TimeService.cpp: _setRTC -> set_time),
//  which any thread may read. Main's ArduinoCloud object is not touched.
// =====================================================================

#include <time.h>
#include <math.h>
#include "config.h"
#include "plc_tags.h"
#include "shared.h"
#include "flow_meter.h"

// ---- eip access with a heartbeat after every exchange ------------------
static bool beatReadReal (const char* tag, float& out) { bool r = eip.readReal(tag, out);  wdBeatPlc(); return r; }
static bool beatReadBool (const char* tag, bool& out)  { bool r = eip.readBool(tag, out);  wdBeatPlc(); return r; }
static bool beatWriteReal(const char* tag, float v)    { bool r = eip.writeReal(tag, v);   wdBeatPlc(); return r; }
static bool beatWriteBool(const char* tag, bool v)     { bool r = eip.writeBool(tag, v);   wdBeatPlc(); return r; }

//  Meter-style rollover, identical for both totals so they cannot disagree.
static inline float waterVolWrap(float v) {
  while (v >= WATERVOL_WRAP_L) v -= WATERVOL_WRAP_L;
  return v;
}
//  Rollover-aware trip = cumulative - mark.
static inline float tripFromMark(float cum, float mark) {
  float t = cum - mark;
  if (t < 0.0f) t += WATERVOL_WRAP_L;
  return t;
}
static inline bool saneVol(bool ok, float v) {
  return ok && !isnan(v) && v >= 0.0f && v <= WATERVOL_MAX_SANE_L;
}

//  The reference for the download check, and the reset path moves it too.
static float s_lastGoodCum   = 0.0f;
static bool  s_lastGoodValid = false;

//  True once per hour. Wall clock when the RTC holds a plausible epoch,
//  free-running otherwise. firstTick marks the boot refresh, which updates
//  the counters but must not push a history entry.
static bool hmiUpdateDue(unsigned long now, bool& firstTick) {
  static uint32_t      lastHour = 0xFFFFFFFFUL;
  static unsigned long lastMs   = 0;
  static bool          everRun  = false;

  bool due = false;
  time_t t = time(nullptr);
  if (t > (time_t)1600000000L) {              // plausible epoch -> clock is real
    uint32_t h = (uint32_t)((unsigned long)t / HMI_UPDATE_SEC);
    if (h != lastHour) {
      //  A changed hour index is not a passed hour: the clock jumps when NTP
      //  lands or resyncs. Require real elapsed time too; on a jump that
      //  comes too soon adopt the hour without writing.
      if (!everRun || (now - lastMs) >= HMI_MIN_GAP_MS) due = true;
      lastHour = h;
    }
  } else if (!everRun || (now - lastMs) >= HMI_UPDATE_SEC * 1000UL) {
    due = true;                               // no clock: keep the HMI moving
  }
  if (due) { lastMs = now; firstTick = !everRun; everRun = true; }
  return due;
}

//  Read the five history tags, shift down one, newest into _1, write back.
//  Oldest first so each slot is written before its source is overwritten.
static bool tripHistoryPush(PlcSnapshot& w, float reading) {
  float vals[TRIP_HIST_N];
  bool  ok[TRIP_HIST_N];
  beatReadReals(TRIP_HIST_TAGS, vals, ok, TRIP_HIST_N);
  for (int i = 0; i < TRIP_HIST_N; i++) if (!ok[i]) vals[i] = 0.0f;   // partly readable still advances
  bool all = true;
  for (int i = TRIP_HIST_N - 1; i >= 0; i--) {
    float v = (i == 0) ? reading : vals[i - 1];
    if (!beatWriteReal(TRIP_HIST_TAGS[i], v)) {
      all = false;
      LOG("[TRIP] history write FAIL: "); LOG(TRIP_HIST_TAGS[i]);
      LOG(" (cip=0x"); LOG(eip.lastCipStatus(), HEX); LOGLN(")");
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "write FAIL %s", TRIP_HIST_TAGS[i]);
    }
  }
  return all;
}

static bool tripHistoryClear() {
  bool all = true;
  for (int i = 0; i < TRIP_HIST_N; i++) all &= beatWriteReal(TRIP_HIST_TAGS[i], 0.0f);
  return all;
}

//  Cloud flowResetTotal (via the command queue) and the HMI reset flag both
//  land here: clear the gateway's counters and journal, zero both PLC
//  totals, and move the reference down with them or the next pass would
//  "restore" the total we were asked to clear.
static void waterOnReset(PlcSnapshot& w) {
  flowReset(w);
  beatWriteReal(TAG_CUMUL_WATERVOL,   0.0f);
  beatWriteReal(TAG_DISPLAY_WATERVOL, 0.0f);
  s_lastGoodCum   = 0.0f;
  s_lastGoodValid = true;
}

//  Called every tick; rate-limits itself to FLOW_PLC_WRITE_MS (5 s), or
//  FLOW_PLC_RETRY_MS while the core tags have never answered.
static void waterToPlc(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  static unsigned long lastWrite = 0;
  static bool          everOk    = false;
  static bool          s_tripEverOk  = false;
  static unsigned long s_tripLastTry = 0;
  static int8_t        lastState = -1;              // -1 unknown, 0 failing, 1 ok

  unsigned long now = millis();
  unsigned long interval = everOk ? FLOW_PLC_WRITE_MS : FLOW_PLC_RETRY_MS;
  if (lastWrite != 0 && now - lastWrite < interval) return;
  lastWrite = now;
  bool forceHmi = false;

  //  HMI lifetime-total reset: reset first so the zero goes out this pass.
  bool rst = false;
  if (beatReadBool(TAG_CUMUL_VOL_RESET, rst) && rst) {
    waterOnReset(w);
    beatWriteBool(TAG_CUMUL_VOL_RESET, false);      // the acknowledgement
    LOGLN("[FLOW>PLC] lifetime total reset requested -- both totals zeroed");
    snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "cumulative total reset (HMI)");
  }

  //  Trip tags back off while they have never answered.
  const bool tripProbe = s_tripEverOk || s_tripLastTry == 0 ||
                         (now - s_tripLastTry) >= FLOW_PLC_RETRY_MS;
  if (tripProbe) s_tripLastTry = now;

  //  Trip reset: one atomic write of the mark; history goes with it. Not
  //  acknowledged if the lifetime total is unreadable.
  bool trst = false;
  if (tripProbe && beatReadBool(TAG_TRIP_RESET, trst) && trst) {
    float cur = 0.0f;
    bool okCur = beatReadReal(TAG_CUMUL_WATERVOL, cur);
    if (saneVol(okCur, cur)) {
      beatWriteReal(TAG_TRIP_MARK,     cur);
      beatWriteReal(TAG_TRIP_WATERVOL, 0.0f);
      tripHistoryClear();
      beatWriteBool(TAG_TRIP_RESET,    false);
      LOG("[TRIP] reset -- mark set to "); LOGLN(cur);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "trip counter reset");
    } else {
      LOGLN("[TRIP] reset requested but the lifetime total is unreadable");
    }
  }

  bool okRate = beatWriteReal(TAG_PROD_FLOWRATE,    w.flowRate);
  bool okVol  = beatWriteReal(TAG_PROD_WATERVOLUME, w.flowBatch);   // THIS discharge

  //  Running totals. Read the base every pass, never cache it.
  float base   = 0.0f;
  bool  okBase = beatReadReal(TAG_CUMUL_WATERVOL, base);
  bool  baseSane = saneVol(okBase, base);

  //  A base below the last good one means the PLC's copy was replaced
  //  (program download). Restore rather than build on it.
  if (baseSane && s_lastGoodValid && base < s_lastGoodCum - WATERVOL_DROP_EPS) {
    LOG("[WATER] cumulative went BACKWARDS "); LOG(s_lastGoodCum);
    LOG(" -> "); LOG(base); LOGLN(" -- PLC reinitialised (program download?)");
    w.plcTotalRestores++;
    snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "PLC total reinitialised - restored by gateway");
#if WATERVOL_RESTORE_ENABLE
    if (beatWriteReal(TAG_CUMUL_WATERVOL, s_lastGoodCum)) {
      base = s_lastGoodCum;
      LOGLN("[WATER] restored the lifetime total from the gateway's copy");
      forceHmi = true;                        // the HMI tag was reinitialised too
    } else {
      baseSane = false;
    }
#else
    baseSane = false;
#endif
  }

  bool okTotals = false;
  if (baseSane) {
    float pend = flowUncommitted();

    //  A finished discharge goes in. Acknowledge, THEN debit the journal.
    if (flowCommitReady()) {
      float newCum = waterVolWrap(base + pend);
      if (beatWriteReal(TAG_CUMUL_WATERVOL, newCum)) {
        flowCommitDone(pend);
        LOG("[FLOW>PLC] committed "); LOG(pend, 3); LOG(" L -> cumulative "); LOGLN(newCum);
        base = newCum;
        pend = 0.0f;
      } else {
        LOG("[FLOW>PLC] commit REJECTED, "); LOG(pend, 3); LOGLN(" L still owed -- will retry");
        snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "commit REJECTED, %ld mL owed", (long)(pend * 1000.0f));   // no %f: newlib-nano
      }
    }
    s_lastGoodCum   = base;                   // after the commit
    s_lastGoodValid = true;

    //  Live figures -> snapshot, every pass. Computed, not read back.
    float liveCum = waterVolWrap(base + pend);
    w.liveCum = liveCum;

    float mark   = 0.0f;
    bool  okMark = tripProbe && beatReadReal(TAG_TRIP_MARK, mark);
    if (okMark) s_tripEverOk = true;
    bool  markSane = saneVol(okMark, mark);
    float liveTrip = markSane ? tripFromMark(liveCum, mark) : 0.0f;
    w.liveTripValid = markSane;
    if (markSane) w.liveTrip = liveTrip;

    //  HMI figures -> PLC, once an hour (or forced after a restore).
    bool firstTick = false;
    bool hourly    = hmiUpdateDue(now, firstTick);
    if (hourly || forceHmi) {
      w.hmiWriteCount++;
      bool okHmi = beatWriteReal(TAG_DISPLAY_WATERVOL, liveCum);
      if (!okHmi) {
        LOG("[HMI] write FAIL: "); LOGLN(TAG_DISPLAY_WATERVOL);
        snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "write FAIL %s", TAG_DISPLAY_WATERVOL);
      }
      if (markSane) {
        if (!beatWriteReal(TAG_TRIP_WATERVOL, liveTrip)) {
          okHmi = false;
          LOG("[HMI] write FAIL: "); LOG(TAG_TRIP_WATERVOL);
          LOG(" (cip=0x"); LOG(eip.lastCipStatus(), HEX); LOGLN(")");
          snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "write FAIL %s", TAG_TRIP_WATERVOL);
        }
        if (hourly && !firstTick) okHmi &= tripHistoryPush(w, liveTrip);   // real boundaries only
      }
      w.hmiWriteOk = okHmi;
      LOG(hourly ? "[HMI] hourly update odo=" : "[HMI] forced refresh odo="); LOG(liveCum);
      if (markSane) { LOG(" trip="); LOG(liveTrip); LOG(firstTick ? " (boot, no history)" : " (history pushed)"); }
      else          { LOG(" (trip tags absent)"); }
      LOGLN(okHmi ? " ok" : " FAILED");
    }
    okTotals = true;                          // the live path was real
  }

  bool all = okRate && okVol && okTotals;
  int8_t state = all ? 1 : 0;
  if (state != lastState) {                   // log transitions only
    lastState = state;
    if (all) {
      LOGLN("[FLOW>PLC] writes accepted (flowrate / watervolume / display)");
    } else {
      LOG("[FLOW>PLC] write FAIL rate="); LOG(okRate ? "ok" : "no");
      LOG(" vol=");  LOG(okVol ? "ok" : "no");
      LOG(" base="); LOG(baseSane ? "ok" : (okBase ? "INSANE" : "no"));
      LOG(" (cip=0x"); LOG(eip.lastCipStatus(), HEX); LOGLN(")");
    }
  }
  w.waterOwedL = flowUncommitted();
  if (okRate && okVol) everOk = true;         // backoff driven by the core writes only
  w.plcFlowWriteOk = all;
}

#endif // WATER_PLC_H
