#ifndef PLC_THREAD_H
#define PLC_THREAD_H

// =====================================================================
//  THE PLC THREAD. Owns eip. Nobody else calls it.
//
//  Every 2 s: reconnect if needed (knocking first with plcProbe so a
//  powered-off PLC costs 0.3 s, not 28), sweep sensors and valves into the
//  working snapshot, every third tick sweep the state words and timers,
//  drain the command queue into eip.write*, publish. Blocking in here --
//  the eip connect, CIP I/O -- is this thread's problem alone. Main keeps
//  publishing to the cloud while this thread waits on a socket.
//
//  Every eip.read* goes through beatReadReals / beatReadDint, which beat
//  the watchdog after each bounded exchange: a tick of eight MSP chunks
//  and a dozen DINT reads against a half-dead PLC is "alive", not "stuck".
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
#include "flow_meter.h"
#include "flow_watch.h"

#define PLC_THREAD_STACK   16384
#define STATE_EVERY_TICKS  3          // 6 s at SAMPLE_INTERVAL_MS 2000

extern EtherNetIPClient eip;       // defined in the .ino, as before
extern IPAddress        plcIp;

static StackWatch s_plcStack;
inline uint32_t plcStackMinFree() { return s_plcStack.minFree(); }

// ---------------------------------------------------------------------
//  The only two places this file touches eip.read*. Chunked MSP with a
//  beat after every chunk; a failed exchange marks its chunk unread.
// ---------------------------------------------------------------------
static void beatReadReals(const char* const* tags, float* out, bool* ok, size_t n) {
  size_t done = 0;
  while (done < n) {
    size_t chunk = n - done;
    if (chunk > EIP_MAX_MSP_TAGS) chunk = EIP_MAX_MSP_TAGS;
    if (!eip.readRealsMSP(tags + done, out + done, ok + done, chunk))
      for (size_t k = 0; k < chunk; k++) ok[done + k] = false;
    wdBeatPlc();
    done += chunk;
  }
}

static bool beatReadDint(const char* tag, int32_t& out) {
  bool r = eip.readDint(tag, out);
  wdBeatPlc();
  return r;
}

#include "water_plc.h"

// ---------------------------------------------------------------------
//  Name failing tags only when the pattern changes, compared as an
//  ARRAY (a bitmask broke silently at 36 entries in the old firmware).
// ---------------------------------------------------------------------
template <size_t N>
static void logFailChanges(const char* who, const bool (&ok)[N], const char* const* tags,
                           bool (&prevOk)[N], bool& prevValid) {
  bool changed = !prevValid;
  for (size_t k = 0; k < N; k++) if (ok[k] != prevOk[k]) changed = true;
  if (!changed) return;
  for (size_t k = 0; k < N; k++) prevOk[k] = ok[k];
  prevValid = true;
  for (size_t k = 0; k < N; k++)
    if (!ok[k]) { LOG(who); LOG(" read FAIL: "); LOGLN(tags[k]); }
}

// ---------------------------------------------------------------------
//  Sensors: 22 REALs batched, 6 DINTs singly with scaling, scattered
//  into canonical slots. Unchanged algorithm from batch 0.
// ---------------------------------------------------------------------
static void pollSensorsInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  static float rvals[N_REAL];
  static bool  rok[N_REAL];

  for (size_t k = 0; k < N_SENSORS; k++) w.ok[k] = false;

  beatReadReals(REAL_TAGS, rvals, rok, N_REAL);
  for (size_t k = 0; k < N_REAL; k++) {
    w.sensor[REAL_SLOT[k]] = rvals[k];
    w.ok    [REAL_SLOT[k]] = rok[k];
  }

  for (size_t k = 0; k < N_DINT; k++) {
    int32_t raw = 0;
    if (beatReadDint(DINT_TAGS[k], raw)) {
      w.sensor[DINT_SLOT[k]] = (float)raw * DINT_SCALE[k] + DINT_OFFSET[k];
      w.ok    [DINT_SLOT[k]] = true;
    }
  }

  size_t good = 0;
  for (size_t k = 0; k < N_SENSORS; k++) if (w.ok[k]) good++;
  w.fails = (int32_t)N_SENSORS - (int32_t)good;
  //  Name the first unread sensor tag. The PLC program is under active
  //  change (2026-09); a renamed or retyped tag used to show up on the
  //  dashboard only as "read fail x2", with no way to tell which.
  const char* firstBadSensor = nullptr;
  for (size_t k = 0; k < N_SENSORS; k++) if (!w.ok[k]) { firstBadSensor = SENSOR_TAGS[k]; break; }
  //  All 28 unread is a session/download problem, not one tag: naming
  //  Temp_1 there would be exactly the false precision this is meant to end.
  snprintf(w.failTag, PlcSnapshot::FAILTAG_CAP, "%s",
           w.fails >= (int32_t)N_SENSORS ? "all sensors" : (firstBadSensor ? firstBadSensor : "ok"));
  w.lastCipStatus = eip.lastCipStatus();

  static bool prevOk[N_SENSORS];
  static bool prevValid = false;
  logFailChanges("[EIP]", w.ok, SENSOR_TAGS, prevOk, prevValid);

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
//  Valves, pumps, positions, fault flags, heat-pump St_* bits: 36 tags,
//  three MSP chunks, straight into w.valve[] / w.valveOk[].
// ---------------------------------------------------------------------
static unsigned long s_valveSweepStartMs = 0;   // batch 12: clearVerifyTick judges only sweeps that began after the due time
static void pollValvesInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  wdWherePlc(WD_AT_VALVES);
  s_valveSweepStartMs = millis();
  for (size_t k = 0; k < N_VALVE; k++) w.valveOk[k] = false;

  beatReadReals(VALVE_TAGS, w.valve, w.valveOk, N_VALVE);

  static bool prevOk[N_VALVE];
  static bool prevValid = false;
  logFailChanges("[VLV]", w.valveOk, VALVE_TAGS, prevOk, prevValid);

  int32_t nfail = 0;
  const char* firstBad = nullptr;
  for (size_t k = 0; k < N_VALVE; k++)
    if (!w.valveOk[k]) { nfail++; if (!firstBad) firstBad = VALVE_TAGS[k]; }
  w.valveFails = nfail;
  if (strcmp(w.failTag, "ok") == 0)             // a sensor failure named this tick keeps the slot
    snprintf(w.failTag, PlcSnapshot::FAILTAG_CAP, "%s", firstBad ? firstBad : "ok");
}

// ---------------------------------------------------------------------
//  State words and cycle timers, every STATE_EVERY_TICKS ticks. Same
//  decode and same one-line text as the old pollPlcState(): only SET bits
//  appear, '?' prefix means at least one tag did not answer.
// ---------------------------------------------------------------------
static void pollPlcStateInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  wdWherePlc(WD_AT_PLCSTATE);
  static float vals[N_STATE];
  static bool  ok[N_STATE];
  beatReadReals(STATE_TAGS, vals, ok, N_STATE);

  uint32_t act = 0, st = 0;
  int32_t fails = 0;
  for (size_t k = 0; k < N_STATE; k++) {
    if (!ok[k]) { fails++; continue; }
    if (vals[k] < 0.5f) continue;          // BOOL arrives as 0.0/1.0
    if (k < N_ACTION) act |= (1UL << k);
    else              st  |= (1UL << (k - N_ACTION));
  }
  w.actionWord = act;
  w.stateWord  = st;
  w.stateFails = fails;

  static const char* const ACT_N[N_ACTION] = {
    "A1","A2","A3","A4","A5","A6","A7","A8","A9","A10","A11","A12","A13","A14","A15" };
  static const char* const ST_N[N_STATE - N_ACTION] = {
    "START","STOP","RESET","PURGE","State1","State2","Fan1","Fan2",
    "TopA","TopB","BotA","BotB" };

  char*  txt = w.stateText;
  size_t cap = PlcSnapshot::STATETEXT_CAP;
  size_t used = 0;
  txt[0] = 0;
  if (fails) used += snprintf(txt + used, cap - used, "? ");
  bool any = false;
  for (size_t k = 0; k < N_ACTION && used < cap - 12; k++)
    if (act & (1UL << k)) {
      used += snprintf(txt + used, cap - used, "%s%s", any ? "," : "", ACT_N[k]);
      any = true;
    }
  if (!any) used += snprintf(txt + used, cap - used, "no action");
  used += snprintf(txt + used, cap - used, " |");
  for (size_t k = 0; k < N_STATE - N_ACTION && used < cap - 10; k++)
    if (st & (1UL << k))
      used += snprintf(txt + used, cap - used, " %s", ST_N[k]);

  static bool prevOk[N_STATE];
  static bool prevValid = false;
  logFailChanges("[SEQ]", ok, STATE_TAGS, prevOk, prevValid);
  if (fails) { LOG("[SEQ] "); LOG(fails); LOG(" of "); LOG((int)N_STATE); LOGLN(" state tags unread"); }

  //  Cycle timers: elapsed is the new information; the presets are re-read
  //  so the dashboard shows what the PLC is actually running.
  int32_t v = 0;
  if (beatReadDint(TAG_ADSORP_ACC, v))     w.adsorpElapsedS    = v / 1000;
  if (beatReadDint(TAG_DESORP_ACC_T6, v))  w.desorpElapsedT6S  = v / 1000;
  if (beatReadDint(TAG_DESORP_ACC_T11, v)) w.desorpElapsedT11S = v / 1000;
  w.adsorpPreMin = (beatReadDint(TAG_ADSORP_TIME, v)   && v > 0) ? (int32_t)(v / MS_PER_MIN) : 0;
  w.desorpPreMin = (beatReadDint(TAG_DESORP_PRE_T6, v) && v > 0) ? (int32_t)(v / MS_PER_MIN) : 0;
  w.desorpPreMinT11 = (beatReadDint(TAG_DESORP_PRE_T11, v) && v > 0) ? (int32_t)(v / MS_PER_MIN) : 0;
  if (w.desorpPreMin > 0 && w.desorpPreMinT11 > 0 && w.desorpPreMin != w.desorpPreMinT11) {
    static int32_t warnedT6 = -1, warnedT11 = -1;
    if (warnedT6 != w.desorpPreMin || warnedT11 != w.desorpPreMinT11) {   // once per distinct pair
      warnedT6 = w.desorpPreMin; warnedT11 = w.desorpPreMinT11;
      LOG("[SEQ] desorp presets differ: T6="); LOG(w.desorpPreMin); LOG(" min, T11="); LOG(w.desorpPreMinT11); LOGLN(" min");
    }
  }

  w.stateSeq++;
}

// ---------------------------------------------------------------------
//  Heat pump through the PLC: 29 REALs (3 MSP chunks) and 11 BOOLs (one
//  MSP chunk -- the old firmware did 11 single reads). Never touches
//  w.lastError: a quiet heat pump must not mask an AWG sensor fault.
//  Stale detection: the twelve HP_In analog slots bit-identical for
//  HP_STALE_TIMEOUT_MS means the PLC<->unit link is frozen.
// ---------------------------------------------------------------------
static void pollHeatPumpInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  wdWherePlc(WD_AT_HEATPUMP);
  for (size_t k = 0; k < N_HP_REAL; k++) w.hpRealOk[k] = false;
  for (size_t k = 0; k < N_HP_BOOL; k++) w.hpBoolOk[k] = false;

  beatReadReals(HP_REAL_TAGS, w.hpReal, w.hpRealOk, N_HP_REAL);
  beatReadReals(HP_BOOL_TAGS, w.hpBool, w.hpBoolOk, N_HP_BOOL);

  int32_t fails = 0;
  for (size_t k = 0; k < N_HP_REAL; k++) if (!w.hpRealOk[k]) fails++;
  for (size_t k = 0; k < N_HP_BOOL; k++) if (!w.hpBoolOk[k]) fails++;
  w.hpFails = fails;

  static bool prevOkR[N_HP_REAL]; static bool prevValidR = false;
  static bool prevOkB[N_HP_BOOL]; static bool prevValidB = false;
  logFailChanges("[HP]", w.hpRealOk, HP_REAL_TAGS, prevOkR, prevValidR);
  logFailChanges("[HP]", w.hpBoolOk, HP_BOOL_TAGS, prevOkB, prevValidB);

  //  Exact float comparison is the point: live sensor readings never
  //  repeat bit-for-bit, so bit-equality across every watched channel is
  //  what a frozen input image looks like.
  static float lastAnalog[HP_STALE_LAST - HP_STALE_FIRST + 1];
  static unsigned long lastChangeMs = 0;
  static bool primed = false;
  bool anyChanged = false;
  for (size_t k = HP_STALE_FIRST; k <= HP_STALE_LAST; k++) {
    if (!w.hpRealOk[k]) continue;
    if (!primed || w.hpReal[k] != lastAnalog[k - HP_STALE_FIRST]) anyChanged = true;
    lastAnalog[k - HP_STALE_FIRST] = w.hpReal[k];
  }
  unsigned long nowMs = millis();
  if (!primed) { primed = true; lastChangeMs = nowMs; }
  else if (anyChanged) lastChangeMs = nowMs;
  unsigned long ageMs = nowMs - lastChangeMs;
  w.hpDataAgeS = (int32_t)(ageMs / 1000UL);
  bool nowStale = ageMs >= HP_STALE_TIMEOUT_MS;
  if (nowStale != w.hpDataStale) {
    LOG("[HP] data "); LOG(nowStale ? "STALE -- HP_In frozen for " : "live again after ");
    LOG((int)(ageMs / 1000UL)); LOGLN(" s");
  }
  w.hpDataStale = nowStale;
  w.hpSeq++;
}

// ---------------------------------------------------------------------
//  batch 12: clear-fault write + read-back. The PLC program decides whether
//  Press_Error & co. are latched (a 0 sticks) or rewritten every scan (a 0
//  is gone before the next read). Rather than assume, write 0, wait two
//  sample periods so a full valve sweep lands after the write, and report
//  what came back. PLC thread only; fixed arrays, no allocation.
// ---------------------------------------------------------------------
#define CLEAR_VERIFY_MS       4000UL   // two sample periods: a whole valve sweep starts after the write
#define CLEAR_VERIFY_STALE_MS 15000UL  // due time this long gone (PLC was away) -> "unverified", not a verdict
#define CLEAR_HOLD_MS         60000UL  // how long the last message stays visible in the "ok" gaps
#define CLEAR_N 3
static const char* const  CLEAR_TAG[CLEAR_N]  = { TAG_PRESS_ERROR, TAG_TEMP_ERROR, TAG_GEN_ERROR };
static const uint8_t      CLEAR_SLOT[CLEAR_N] = { VSLOT_PRESS_ERROR, VSLOT_TEMP_ERROR, VSLOT_GEN_ERROR };
static bool               s_clearPending[CLEAR_N] = { false, false, false };
static unsigned long      s_clearDueMs[CLEAR_N]   = { 0, 0, 0 };
//  Latched text (review I1): pollSensorsInto rewrites lastError to "ok"
//  every tick, so a one-tick message lives in exactly one snapshot and a
//  cloud pass that happens to be blocked (WiFi begin, TLS, OTA) never sees
//  it. Same overlay rule as flow_watch: shown only where lastError is "ok",
//  so real errors still win.
static char               s_clearText[PlcSnapshot::LASTERR_CAP] = "";
static unsigned long      s_clearTextUntil = 0;
static bool               s_clearTextOn    = false;

static void clearSay(PlcSnapshot& w, const char* fmt, const char* tag) {
  snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, fmt, tag);
  snprintf(s_clearText, sizeof s_clearText, "%s", w.lastError);
  s_clearTextUntil = millis() + CLEAR_HOLD_MS;
  s_clearTextOn    = true;
  LOG("[CTRL] "); LOGLN(w.lastError);
}

//  Shared by the three CMD_CLEAR_* cases: write, report, arm the read-back.
static bool clearFault(PlcSnapshot& w, uint8_t i) {
  SHARED_ASSERT_ON_PLC();
  bool ok = eip.writeBool(CLEAR_TAG[i], false);
  clearSay(w, ok ? "%s cleared" : "write %s failed", CLEAR_TAG[i]);
  if (ok) { s_clearPending[i] = true; s_clearDueMs[i] = millis() + CLEAR_VERIFY_MS; }
  return ok;
}

//  PLC gone (review I2): a verdict read after a reconnect or a PLC download
//  would describe a bit that changed for unrelated reasons. Drop the arming;
//  the "cleared" text stays on screen so the operator knows to look again.
static void clearDisarm() {
  for (uint8_t i = 0; i < CLEAR_N; i++) s_clearPending[i] = false;
}

//  After pollValvesInto(): report the read-back for every armed clear whose
//  sweep STARTED after the due time (review M4: a sweep that started 100 ms
//  after the write but dragged past 4 s on retries read the bit before the
//  PLC's next scan could re-assert it). Runs only with the PLC connected.
static void clearVerifyTick(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  unsigned long now = millis();
  for (uint8_t i = 0; i < CLEAR_N; i++) {
    if (!s_clearPending[i] || (long)(s_valveSweepStartMs - s_clearDueMs[i]) < 0) continue;
    s_clearPending[i] = false;
    const uint8_t slot = CLEAR_SLOT[i];
    if ((long)(now - s_clearDueMs[i]) > (long)CLEAR_VERIFY_STALE_MS || !w.valveOk[slot])
      clearSay(w, "%s clear unverified", CLEAR_TAG[i]);
    else if (w.valve[slot] > 0.5f)
      clearSay(w, "%s re-asserted by PLC", CLEAR_TAG[i]);
    else
      clearSay(w, "%s clear confirmed", CLEAR_TAG[i]);
  }
  //  Overlay the latched text into the "ok" gaps for CLEAR_HOLD_MS.
  if (s_clearTextOn) {
    if ((long)(now - s_clearTextUntil) >= 0) s_clearTextOn = false;
    else if (strcmp(w.lastError, "ok") == 0)
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s", s_clearText);
  }
}

// ---------------------------------------------------------------------
//  batch 13: manual drain. S4 (Air_S4_Output) is a 0..100 REAL, the Lefoo
//  pump (Cond_Pump) a BOOL. The pump only starts with S4 read back open,
//  and stops itself at collector level 18 or after 180 s, whichever first.
//  Both writes are read back 4 s later like the fault clears; the text goes
//  through clearSay() so it stays visible in the "ok" gaps for 60 s.
// ---------------------------------------------------------------------
#define MAN_PUMP_STOP_LEVEL 18.0f
#define MAN_PUMP_MAX_MS     180000UL
#define MAN_VERIFY_MS       4000UL
//  OWNERSHIP (review C1). The two dashboard switches mirror the PLC's real
//  outputs, so during an automatic discharge they read ON with nobody having
//  touched them. Only an output the gateway itself switched on may be
//  switched off from the dashboard; anything else is the sequencer's and
//  stays alone. Ownership ends when the operator turns it off, when the PLC
//  is seen to have taken it back, or when the read-back verdict says so.
static bool          s_manOwnS4      = false;
static bool          s_manOwnS5      = false;   // batch 13.1: Air_S5, tank -> pump
static bool          s_manPumpActive = false;   // == the gateway owns a running pump (timer armed)
static unsigned long s_manPumpOnMs   = 0;
#define MAN_N 3                                 // 0 = S4, 1 = pump, 2 = S5
static bool          s_manChk[MAN_N]    = { false, false, false };
static bool          s_manExpect[MAN_N] = { false, false, false };
static unsigned long s_manDueMs[MAN_N]  = { 0, 0, 0 };
static const uint8_t MAN_SLOT[MAN_N]    = { VSLOT_POS_S4, VSLOT_P_COND, VSLOT_V_S5 };
static const char* const MAN_WHO[MAN_N] = { "S4", TAG_P_COND, "S5" };
static inline bool manReadOn(const PlcSnapshot& w, uint8_t i) {   // slot 0 is a 0..100 REAL, the others BOOL
  return (i == 0) ? (w.valve[MAN_SLOT[i]] >= 50.0f) : (w.valve[MAN_SLOT[i]] > 0.5f);
}

static void manArm(uint8_t i, bool expect) {
  s_manChk[i] = true; s_manExpect[i] = expect; s_manDueMs[i] = millis() + MAN_VERIFY_MS;
}

//  arm=false for the auto-stop: a sequencer pump start inside the next 4 s
//  must not be reported as "re-asserted" (review I6).
static bool manPumpWrite(PlcSnapshot& w, bool on, const char* okText, bool arm) {
  bool ok = eip.writeBool(TAG_P_COND, on);
  if (ok) clearSay(w, "%s", okText);
  else    clearSay(w, "write %s failed", TAG_P_COND);
  if (ok) { if (arm) manArm(1, on); s_manPumpActive = on; if (on) s_manPumpOnMs = millis(); }
  wdBeatPlc();
  return ok;
}

//  ON gates re-checked here on the PLC thread's own state word (review I4):
//  the cloud thread's copy can be ~8 s stale.
static bool manS4(PlcSnapshot& w, bool on) {
  if (on && w.actionWord != 0) { clearSay(w, "%s", "manual: cycle running"); return false; }
  if (!on && !s_manOwnS4)      { clearSay(w, "%s", "S4: not a manual output"); return false; }
  bool pumpOn = s_manPumpActive || (w.valveOk[VSLOT_P_COND] && w.valve[VSLOT_P_COND] > 0.5f);
  bool stoppedPump = false;
  if (!on && pumpOn) {
    stoppedPump = manPumpWrite(w, false, "pump stopped", true);
    if (!stoppedPump) return false;           // review I2: never seal the collector on a running pump
  }
  bool ok = eip.writeReal(TAG_POS_S4, on ? 100.0f : 0.0f);
  if (!ok)              clearSay(w, "write %s failed", TAG_POS_S4);
  else if (stoppedPump) clearSay(w, "%s", "S4 closed, pump stopped");
  else                  clearSay(w, "%s", on ? "S4 opened" : "S4 closed");
  if (ok) { manArm(0, on); s_manOwnS4 = on; }
  return ok;
}

static bool manPump(PlcSnapshot& w, bool on) {
  if (on && w.actionWord != 0)  { clearSay(w, "%s", "manual: cycle running"); return false; }
  if (!on && !s_manPumpActive)  { clearSay(w, "%s", "pump: not a manual output"); return false; }
  if (on) {                                   // both the vent (S4) and the suction valve (S5) must read open
    bool s4 = w.valveOk[VSLOT_POS_S4] && w.valve[VSLOT_POS_S4] >= 50.0f;
    bool s5 = w.valveOk[VSLOT_V_S5]   && w.valve[VSLOT_V_S5]   >  0.5f;
    if (!s4 || !s5) {
      clearSay(w, "%s", (!s4 && !s5) ? "pump: S4/S5 not open" : (!s4 ? "pump: S4 not open" : "pump: S5 not open"));
      return false;
    }
  }
  return manPumpWrite(w, on, on ? "pump started" : "pump stopped", true);
}

//  batch 13.1: S5 (Air_S5, BOOL) sits between the collector and the pump.
//  Closing it with the pump running stops the pump first, like S4.
static bool manS5(PlcSnapshot& w, bool on) {
  if (on && w.actionWord != 0) { clearSay(w, "%s", "manual: cycle running"); return false; }
  if (!on && !s_manOwnS5)      { clearSay(w, "%s", "S5: not a manual output"); return false; }
  bool pumpOn = s_manPumpActive || (w.valveOk[VSLOT_P_COND] && w.valve[VSLOT_P_COND] > 0.5f);
  bool stoppedPump = false;
  if (!on && pumpOn) {
    stoppedPump = manPumpWrite(w, false, "pump stopped", true);
    if (!stoppedPump) return false;
  }
  bool ok = eip.writeBool(TAG_V_S5, on);
  if (!ok)              clearSay(w, "write %s failed", TAG_V_S5);
  else if (stoppedPump) clearSay(w, "%s", "S5 closed, pump stopped");
  else                  clearSay(w, "%s", on ? "S5 opened" : "S5 closed");
  if (ok) { manArm(2, on); s_manOwnS5 = on; }
  return ok;
}

//  PLC gone: drop the read-back checks (a verdict after a reconnect would
//  judge a bit that changed for other reasons) but KEEP the pump ownership
//  and its start time (review I3): if the pump is still running when the
//  link returns, the level-18 / 180 s stop must still apply.
static void manDisarm() { for (uint8_t i = 0; i < MAN_N; i++) s_manChk[i] = false; }

//  After pollValvesInto(), PLC connected: auto-stop first, then the verdicts
//  (only for sweeps that started after the due time, as in clearVerifyTick),
//  then ownership catch-up from the read-back.
static void manualTick(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  unsigned long now = millis();
  if (s_manPumpActive) {
    bool low  = w.ok[SSLOT_LEVEL] && w.sensor[SSLOT_LEVEL] < MAN_PUMP_STOP_LEVEL;
    bool late = now - s_manPumpOnMs >= MAN_PUMP_MAX_MS;
    if (low || late) {
      wdWherePlc(WD_AT_CIPWRITE);
      manPumpWrite(w, false, low ? "pump stopped: level 18" : "pump stopped: 180 s", false);
      wdWherePlc(WD_AT_VALVES);
    }
  }
  for (uint8_t i = 0; i < MAN_N; i++) {
    if (!s_manChk[i] || (long)(s_valveSweepStartMs - s_manDueMs[i]) < 0) continue;
    s_manChk[i] = false;
    const uint8_t slot = MAN_SLOT[i];
    if ((long)(now - s_manDueMs[i]) > (long)CLEAR_VERIFY_STALE_MS || !w.valveOk[slot]) {
      clearSay(w, "%s unverified", MAN_WHO[i]);    // review I6: too late or unread -> no judgement
      continue;
    }
    if (manReadOn(w, i) != s_manExpect[i]) {
      clearSay(w, "%s re-asserted by PLC", MAN_WHO[i]);
      if (i == 0) s_manOwnS4 = false; else if (i == 1) s_manPumpActive = false; else s_manOwnS5 = false;
    }
  }
  //  Outside a verdict window, an output we own that reads OFF was taken
  //  back by the PLC (its own stop, a download, a reset): ownership ends.
  if (s_manOwnS4      && !s_manChk[0] && w.valveOk[MAN_SLOT[0]] && !manReadOn(w, 0)) s_manOwnS4      = false;
  if (s_manPumpActive && !s_manChk[1] && w.valveOk[MAN_SLOT[1]] && !manReadOn(w, 1)) s_manPumpActive = false;
  if (s_manOwnS5      && !s_manChk[2] && w.valveOk[MAN_SLOT[2]] && !manReadOn(w, 2)) s_manOwnS5      = false;
  //  batch 13.1 review: a valve the manual pump depends on (S4 vent, S5
  //  suction) read closed by ANYONE -- the PLC re-asserting it, as it does
  //  with Cond_Pump -- stops the pump at once. Otherwise the Lefoo would
  //  dead-head against a closed suction line for the full 180 s: the level
  //  cannot fall, so the level stop never fires.
  for (uint8_t i = 0; i < MAN_N; i += 2) {           // 0 = S4, 2 = S5
    if (s_manPumpActive && !s_manChk[i] && w.valveOk[MAN_SLOT[i]] && !manReadOn(w, i)) {
      char why[32];
      snprintf(why, sizeof why, "pump stopped: %s closed", MAN_WHO[i]);
      wdWherePlc(WD_AT_CIPWRITE);
      manPumpWrite(w, false, why, false);
      wdWherePlc(WD_AT_VALVES);
    }
  }
}

// ---------------------------------------------------------------------
//  Commands from main. The gate is checked again here: a controlEnabled
//  that went false between the post and this tick drops the queue. Every
//  write reports into w.lastError with the old firmware's wording so the
//  dashboard reads the same.
// ---------------------------------------------------------------------
static void applyCommand(PlcSnapshot& w, const Cmd& c) {
  const bool on = c.value != 0.0f;
  bool ok = false;
  switch (c.tag) {
    case CMD_START_BUTTON:
      ok = eip.writeBool(TAG_START_BUTTON, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? (on ? "start set" : "start cleared") : "Start_Button write failed");
      break;
    case CMD_STOP_BUTTON:
      ok = eip.writeBool(TAG_STOP_BUTTON, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? (on ? "stop set" : "stop cleared") : "write Stop_Button failed");
      break;
    case CMD_RESET_BUTTON:
      ok = eip.writeBool(TAG_RESET_BUTTON, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? (on ? "reset set" : "reset cleared") : "write Reset_Button failed");
      break;
    case CMD_PURGE_BUTTON:
      ok = eip.writeBool(TAG_PURGE_BUTTON, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? (on ? "purge set" : "purge cleared") : "write Purge_Button failed");
      break;
    case CMD_ADSORP_TIME_MS: {
      int32_t mins = (int32_t)c.value;
      if (mins < ADSORP_TIME_MIN_MIN) mins = ADSORP_TIME_MIN_MIN;   // clamp where the write happens too
      if (mins > ADSORP_TIME_MAX_MIN) mins = ADSORP_TIME_MAX_MIN;
      ok = eip.writeDint(TAG_ADSORP_TIME, mins * MS_PER_MIN);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? "adsorp time set" : "write Timer_3.PRE failed");
      break;
    }
    case CMD_DESORP_TIME_MS: {
      int32_t mins = (int32_t)c.value;
      if (mins < DESORP_TIME_MIN_MIN) mins = DESORP_TIME_MIN_MIN;
      if (mins > DESORP_TIME_MAX_MIN) mins = DESORP_TIME_MAX_MIN;
      int32_t ms   = mins * MS_PER_MIN;
      bool okT6  = eip.writeDint(TAG_DESORP_PRE_T6,  ms);
      wdBeatPlc();
      bool okT11 = eip.writeDint(TAG_DESORP_PRE_T11, ms);
      ok = okT6 && okT11;
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? "desorp time set"
                  : (!okT6 && !okT11) ? "write Timer_6/11[3].PRE failed"
                  : !okT6 ? "write Timer_6[3].PRE failed"
                          : "write Timer_11[3].PRE failed");
      break;
    }
    case CMD_HP_ENABLE:
      ok = eip.writeBool(TAG_HP_ENABLE, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? "hp enable set" : "hp enable write failed");
      break;
    case CMD_HP_MODE_COOL:
      ok = eip.writeBool(TAG_HP_COOLREQ, on);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? "hp mode set" : "hp mode write failed");
      break;
#if FLOW_TO_PLC_ENABLE
    case CMD_FLOW_RESET_TOTAL:
      ok = waterOnReset(w);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "%s",
               ok ? "all water totals reset" : "water reset: PLC write failed");
      if (ok) LOGLN("[FLOW] all water totals reset (gateway + PLC lifetime, trip, history)");
      break;
#endif
    case CMD_CLEAR_PRESS_ERROR: ok = clearFault(w, 0); break;   // batch 12
    case CMD_CLEAR_TEMP_ERROR:  ok = clearFault(w, 1); break;
    case CMD_CLEAR_GEN_ERROR:   ok = clearFault(w, 2); break;
    case CMD_MAN_S4:            ok = manS4(w, on);     break;   // batch 13
    case CMD_MAN_PUMP:          ok = manPump(w, on);   break;
    case CMD_MAN_S5:            ok = manS5(w, on);     break;   // batch 13.1
    default:
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "cmd %u not in batch 1", (unsigned)c.tag);
      break;
  }
  wdBeatPlc();
  LOG("[CTRL] cmd "); LOG(c.tag); LOG(" value="); LOG(c.value);
  LOG(" -> "); LOG(ok ? "OK" : "FAIL"); LOG(" (cip=0x"); LOG(eip.lastCipStatus(), HEX); LOGLN(")");
}

//  allowWrites is false on the tick the PLC is (re)connecting: eip.begin()
//  may have just succeeded a few lines above the call, which would make
//  eip.connected() true and let a command queued while the PLC was away
//  fire on the very tick it comes back. Commands posted while the PLC was
//  offline are dropped and reported, never held for the reconnect.
static void drainCommands(PlcSnapshot& w, bool allowWrites) {
  SHARED_ASSERT_ON_PLC();
  CtrlState ctrl;
  sharedCtrlRead(ctrl);
  Cmd c;
  while (sharedCmdTake(c)) {
    if (!allowWrites) {
      LOG("[CTRL] dropped, plc offline this tick: tag="); LOGLN(c.tag);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "control: plc offline");
      continue;
    }
    //  The master switch means "a human is operating the machine". Zeroing
    //  the water books is bookkeeping, not operation, so the reset command
    //  passes this gate (it still needs the PLC online, below).
    const bool bookkeeping = (c.tag == CMD_FLOW_RESET_TOTAL);
    if (!ctrl.controlEnabled && !bookkeeping) {
      LOG("[CTRL] dropped, control disabled: tag="); LOGLN(c.tag);
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "control disabled");
      continue;
    }
    if (!eip.connected()) {
      snprintf(w.lastError, PlcSnapshot::LASTERR_CAP, "control: plc offline");
      continue;
    }
    applyCommand(w, c);
  }
}

static void plcThreadBody() {
  g_plcThreadId = osThreadGetId();
  s_plcStack.begin();          // shallowest point of this stack
  PlcSnapshot w = {};
  unsigned long reconnectWait = RECONNECT_BACKOFF_MS;
  unsigned long lastReconnect = 0;
  uint32_t tickN = 0;
  LOGLN("[PLC] thread started");
  flowWatchInit();

  for (;;) {
    unsigned long tick0 = millis();
    wdBeatPlc();

    //  The meter is wired to the Opta, not the PLC: measure every tick,
    //  session up or down. Litres accumulate in the journal until the PLC
    //  is back to take them.
    flowTick(w);

    if (!eip.connected()) {
      w.plcConnected = false;
      //  Nothing was read this tick: say so, or main would keep treating
      //  last tick's values as fresh.
      for (size_t k = 0; k < N_SENSORS; k++) w.ok[k] = false;
      for (size_t k = 0; k < N_VALVE;   k++) w.valveOk[k] = false;
      clearDisarm();                          // batch 12: no verdict on a bit read after a reconnect
      manDisarm();                            // batch 13: same, and the manual pump timer is void
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
      //  commands posted while the PLC was away are dropped and reported,
      //  never held for the reconnect
      wdWherePlc(WD_AT_CIPWRITE);
      drainCommands(w, false);
    } else {
      wdWherePlc(WD_AT_SENSORS);
      pollSensorsInto(w);
      if (eip.connected()) {
        pollValvesInto(w);
        clearVerifyTick(w);                   // batch 12: read-back of any clear written >= 4 s ago
        manualTick(w);                        // batch 13: manual pump auto-stop + S4/pump read-back
        if (tickN % STATE_EVERY_TICKS == 0) pollPlcStateInto(w);
        if (tickN % STATE_EVERY_TICKS == 1) pollHeatPumpInto(w);
#if FLOW_TO_PLC_ENABLE
        wdWherePlc(WD_AT_CIPWRITE);
        waterToPlc(w);                        // rate-limited inside to 5 s
#endif
      }
      wdWherePlc(WD_AT_CIPWRITE);
      drainCommands(w, true);
    }
    tickN++;

    s_plcStack.sample();
    w.plcStackFree = s_plcStack.minFree();
    w.cmdDropped   = g_cmdDropped;

    flowWatchTick(w);                         // batch 7: judge the finished discharge, overlay lastError

    wdWherePlc(WD_AT_PLCPUBLISH);
    sharedPublish(w);
    wdWherePlc(WD_AT_NONE);

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
