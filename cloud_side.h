#ifndef CLOUD_SIDE_H
#define CLOUD_SIDE_H

// =====================================================================
//  MAIN THREAD ONLY. The one place Cloud* properties are assigned from
//  PLC data. Takes the published snapshot and, when its seq has advanced,
//  copies each slot into its Cloud* -- skipping slots the PLC thread did
//  not read this tick, so a stale property keeps its last real value
//  instead of becoming 0. The seven String properties are built here from
//  char[], on main, at assignment time. That is the whole tearing story.
// =====================================================================

#include "thingProperties.h"
#include "shared.h"

static PlcSnapshot s_local = {};

//  Assign sensor slot i to a Cloud* only if it was read this tick.
#define TAKE(i, var) do { if (s_local.ok[i]) var = s_local.sensor[i]; } while (0)

static void cloudSideAssign() {
  //  Slot order == SENSOR_TAGS order in plc_tags.h. Same 28 names as the
  //  ASSIGN block in the old pollSensors().
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

  plcConnected = s_local.plcConnected;
  eipMs        = (int)s_local.eipMs;
  lastError    = String(s_local.lastError);     // String built on main, from char[]
}
#undef TAKE

//  Call every main pass. Cheap when nothing changed: one mutex-guarded
//  struct copy and a compare.
inline bool cloudSideConsume(uint32_t& lastSeq) {
  if (!sharedTake(s_local, lastSeq)) return false;
  cloudSideAssign();
  return true;
}

inline uint32_t cloudSideSnapshotAgeMs() {
  return millis() - s_local.stampMs;
}

inline uint32_t cloudSidePlcStackFree() { return s_local.plcStackFree; }
inline uint32_t cloudSideCmdDropped()   { return s_local.cmdDropped; }

#endif // CLOUD_SIDE_H
