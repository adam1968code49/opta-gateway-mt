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
static void pollValvesInto(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  wdWherePlc(WD_AT_VALVES);
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
