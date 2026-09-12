#ifndef CLOUD_SIDE_H
#define CLOUD_SIDE_H

// =====================================================================
//  CLOUD THREAD ONLY (batch 6; was main). The one place Cloud* properties are assigned from
//  PLC data. Takes the published snapshot and, when its seq has advanced,
//  copies each slot into its Cloud* -- skipping slots the PLC thread did
//  not read this tick, so a stale property keeps its last real value
//  instead of becoming 0. The String properties fed from PLC data
//  (lastError, plcFailTag, plcStateText) are built here from char[], on
//  main, at assignment time. That is the whole tearing story.
// =====================================================================

#include "thingProperties.h"
#include "shared.h"

static PlcSnapshot s_local = {};

//  What lastError currently holds, as far as this file knows. The de-dup in
//  cloudSideAssign() compares the snapshot text against THIS, so any other
//  writer of lastError (the cloud thread's "plc thread stalled" warning)
//  must record what it wrote here -- otherwise the next "ok" from the PLC
//  looks unchanged and the warning sticks on the dashboard forever
//  (2026-09-11 10:32 boot: "plc thread stalled 64s" stayed up for hours).
static char s_prevLastError[PlcSnapshot::LASTERR_CAP] = "";
inline void cloudSideNoteLastError(const char* text) { snprintf(s_prevLastError, sizeof s_prevLastError, "%s", text); }

//  Assign sensor slot i only if it was read this tick.
#define TAKE(i, var)    do { if (s_local.ok[i])      var = s_local.sensor[i]; } while (0)
//  Valve sweep: BOOL tags arrive as 0.0/1.0 -- compare against 0.5.
#define TAKE_VB(i, var) do { if (s_local.valveOk[i]) var = (s_local.valve[i] > 0.5f); } while (0)
#define TAKE_VF(i, var) do { if (s_local.valveOk[i]) var = s_local.valve[i]; } while (0)
//  Heat-pump sweep (slot order == HP_REAL_TAGS / HP_BOOL_TAGS).
#define TAKE_HR(i, var) do { if (s_local.hpRealOk[i]) var = s_local.hpReal[i]; } while (0)
#define TAKE_HI(i, var) do { if (s_local.hpRealOk[i]) var = (int)s_local.hpReal[i]; } while (0)
#define TAKE_HB(i, var) do { if (s_local.hpBoolOk[i]) var = (s_local.hpBool[i] > 0.5f); } while (0)

static void cloudSideAssign() {
  SHARED_ASSERT_ON_CLOUD();

  // ---- sensors: slot order == SENSOR_TAGS ---------------------------------
  TAKE( 0, t1HotTank);     TAKE( 1, t2ColdTank);
  TAKE( 2, t3TopChIn);     TAKE( 3, t4TopChOut);
  TAKE( 4, t5BotChIn);     TAKE( 5, t6BotChOut);
  TAKE( 6, t7HotHxIn);     TAKE( 7, t8HotHxOut);
  TAKE( 8, t9BoosterIn);   TAKE( 9, t10BoosterOut);
  TAKE(10, t11CondenserOut); TAKE(11, t12HeatPumpIn);
  TAKE(12, vtVaisalaTemp);
  TAKE(13, p1TopChamber);  TAKE(14, p2BotChamber);
  TAKE(15, p3ChamberRedun); TAKE(16, p4Booster);
  TAKE(17, p5Collector);   TAKE(18, p6VaisalaWvp);
  TAKE(19, hp1HotInlet);   TAKE(20, hp1ColdInlet);
  TAKE(21, hp2HotInlet);   TAKE(22, hp2ColdInlet);
  TAKE(23, atmTemp);       TAKE(24, atmRh);
  TAKE(25, atmTempDs);     TAKE(26, atmRhDs);
  TAKE(27, tankLevel);

  // ---- valve sweep: slot order == VALVE_TAGS -------------------------------
  TAKE_VB( 0, valveS1);    TAKE_VB( 1, valveS5);   TAKE_VB( 2, valveS6);
  TAKE_VB( 3, valveS7);    TAKE_VB( 4, valveS10);
  TAKE_VB( 5, valveV1A);   TAKE_VB( 6, valveV1B);
  TAKE_VB( 7, valveV2A1);  TAKE_VB( 8, valveV2A2); TAKE_VB( 9, valveV2B);
  TAKE_VB(10, pumpScroll); TAKE_VB(11, pumpCond);
  TAKE_VB(12, pumpHotWater); TAKE_VB(13, pumpColdWater);
  TAKE_VB(14, boosterVfdRun);
  TAKE_VF(15, posS2);  TAKE_VF(16, posS3);  TAKE_VF(17, posS4);
  TAKE_VF(18, posS8);  TAKE_VF(19, posS9);
  TAKE_VF(20, posV10); TAKE_VF(21, posV11);
  TAKE_VF(22, hmiWaterTotal); TAKE_VF(23, cumulativeWaterVolume);
  TAKE_VB(VSLOT_PRESS_ERROR, pressError); TAKE_VB(VSLOT_TEMP_ERROR, tempError); TAKE_VB(VSLOT_GEN_ERROR, genError);
  TAKE_VB(27, stStage1);     TAKE_VB(28, stStage2);     TAKE_VB(29, stAuxHeat);
  TAKE_VB(30, stIndoorCirc); TAKE_VB(31, stIndoorFlow); TAKE_VB(32, stOutdoorFlow);
  TAKE_VB(33, stLockout);    TAKE_VB(34, stPhaseFault); TAKE_VB(35, stBACnetControl);
  plcReadFails = (int)s_local.valveFails;
  //  CloudString holds a String: assigning it every 2 s is a heap round trip
  //  even when nothing changed. Assign only on a real change of text.
  static char prevFailTag[PlcSnapshot::FAILTAG_CAP] = "";
  if (strcmp(prevFailTag, s_local.failTag) != 0) {
    snprintf(prevFailTag, sizeof prevFailTag, "%s", s_local.failTag);
    plcFailTag = String(s_local.failTag);
  }

  // ---- state words and timers: only when the 6 s sweep actually ran -------
  static uint32_t lastStateSeq = 0;
  if (s_local.stateSeq != lastStateSeq) {
    lastStateSeq = s_local.stateSeq;
    plcActionWord     = (int)s_local.actionWord;
    plcStateWord      = (int)s_local.stateWord;
    static char prevStateText[PlcSnapshot::STATETEXT_CAP] = "";
    if (strcmp(prevStateText, s_local.stateText) != 0) {
      snprintf(prevStateText, sizeof prevStateText, "%s", s_local.stateText);
      plcStateText = String(s_local.stateText);
    }
    adsorpElapsedS    = (int)s_local.adsorpElapsedS;
    desorpElapsedT6S  = (int)s_local.desorpElapsedT6S;
    desorpElapsedT11S = (int)s_local.desorpElapsedT11S;
    //  Preset read-back: the dashboard shows what the PLC runs. A local
    //  assignment to a READWRITE property publishes without re-entering
    //  its callback.
    if (s_local.adsorpPreMin > 0) adsorpTimeMs = (int)s_local.adsorpPreMin;
    if (s_local.desorpPreMin > 0) desorpTimeMs = (int)s_local.desorpPreMin;
  }

  // ---- heat pump: only when the 6 s sweep actually ran --------------------
  static uint32_t lastHpSeq = 0;
  if (s_local.hpSeq != lastHpSeq) {
    lastHpSeq = s_local.hpSeq;
    TAKE_HR( 0, hpLoopTemp);     TAKE_HR( 1, hpStage1ActivT); TAKE_HR( 2, hpStage2ActivT);
    TAKE_HR( 3, hpDischTemp);    TAKE_HR( 4, hpSuctionPress); TAKE_HR( 5, hpDischPress);
    TAKE_HR( 6, hpEvapTemp);     TAKE_HR( 7, hpCondTemp);     TAKE_HR( 8, hpSuctionLineT);
    TAKE_HR( 9, hpSuperheat);    TAKE_HR(10, hpEevPosition);  TAKE_HR(11, hpCompCurrent);
    TAKE_HR(12, hpOutdoorIn);    TAKE_HR(13, hpOutdoorOut);   TAKE_HR(14, hpIndoorIn);
    TAKE_HI(15, hpOperationMode); TAKE_HI(16, hpLimitsWord);  TAKE_HI(17, hpPermAlarms1);
    TAKE_HI(18, hpPermAlarms2);  TAKE_HI(19, hpBoardFaults);  TAKE_HI(20, hpSensorFaults);
    TAKE_HR(21, hpHtgSp1);       TAKE_HR(22, hpHtgSp2);       TAKE_HR(23, hpHtgDelta1);  TAKE_HR(24, hpHtgDelta2);
    TAKE_HR(25, hpClgSp1);       TAKE_HR(26, hpClgSp2);       TAKE_HR(27, hpClgDelta1);  TAKE_HR(28, hpClgDelta2);
    TAKE_HB( 0, hpStage1Call);   TAKE_HB( 1, hpStage2Call);   TAKE_HB( 2, hpStage1Sat);  TAKE_HB( 3, hpStage2Sat);
    TAKE_HB( 4, hpY1Cmd);        TAKE_HB( 5, hpY2Cmd);        TAKE_HB( 6, hpReverseCmd); TAKE_HB( 7, hpCoolRequest);
    TAKE_HB( 8, hpTempValid);    TAKE_HB( 9, hpEnableSt);     TAKE_HB(10, hpManualOvrSt);
    hpDataStale = s_local.hpDataStale;
    hpDataAgeS  = (int)s_local.hpDataAgeS;
  }

  // ---- water accounting (batch 2) -----------------------------------------
  flowRate           = s_local.flowRate;
  flowTotal          = s_local.flowTotal;
  flowBatch          = s_local.flowBatch;
  waterOwedL         = s_local.waterOwedL;
  displayWaterVolume = s_local.liveCum;
  if (s_local.liveTripValid) tripWaterVolume = s_local.liveTrip;
  plcFlowWriteOk     = s_local.plcFlowWriteOk;
  hmiWriteOk         = s_local.hmiWriteOk;
  hmiWriteCount      = (int)s_local.hmiWriteCount;
  plcTotalRestores   = (int)s_local.plcTotalRestores;

  // ---- link ---------------------------------------------------------------
  plcConnected = s_local.plcConnected;
  eipMs        = (int)s_local.eipMs;
  if (strcmp(s_prevLastError, s_local.lastError) != 0) {
    snprintf(s_prevLastError, sizeof s_prevLastError, "%s", s_local.lastError);
    lastError = String(s_local.lastError);     // the one String this file still builds, and only on change
  }
}
#undef TAKE
#undef TAKE_VB
#undef TAKE_VF
#undef TAKE_HR
#undef TAKE_HI
#undef TAKE_HB

//  Call every main pass. Cheap when nothing changed: one mutex-guarded
//  struct copy and a compare.
inline bool cloudSideConsume(uint32_t& lastSeq) {
  if (!sharedTake(s_local, lastSeq)) return false;
  cloudSideAssign();
  return true;
}

inline uint32_t cloudSideSnapshotAgeMs() { return millis() - s_local.stampMs; }
inline uint32_t cloudSidePlcStackFree()  { return s_local.plcStackFree; }
inline uint32_t cloudSideCmdDropped()    { return s_local.cmdDropped; }
inline int32_t  cloudSideFlowMismatchCount() { return s_local.flowMismatchCount; }
inline bool     cloudSidePlcConnected()  { return s_local.plcConnected; }
inline bool     cloudSideHasSnapshot()   { return s_local.seq != 0; }   // false until the PLC thread published once
inline const PlcSnapshot& cloudSideSnapshot() { return s_local; }       // cloud thread's own copy (batch 11 replay)

#endif // CLOUD_SIDE_H
