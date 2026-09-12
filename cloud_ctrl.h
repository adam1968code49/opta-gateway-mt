#ifndef CLOUD_CTRL_H
#define CLOUD_CTRL_H

// =====================================================================
//  MAIN THREAD ONLY. Dashboard -> PLC, without touching eip.
//
//  The seven READWRITE callbacks run inside ArduinoCloud.update(). Each
//  one passes three gates and then posts a Cmd on the queue; the PLC
//  thread writes it on its next tick (<= 2 s). Nothing here blocks.
//
//  Sync-keyed quiet period. The library replays every READWRITE
//  property's stored cloud value through its onUpdate callback (via the
//  default onSync handler, CLOUD_WINS -> onForceCloudSync) while decoding
//  the LastValuesUpdateCmdId message -- and only AFTER that decode
//  finishes does it raise ArduinoIoTCloudEvent::SYNC
//  (ArduinoIoTCloudTCP.cpp:545-549). So SYNC is the reliable "the replay
//  just happened" signal: connected() is not, because MQTT can be up for
//  up to AIOT_CONFIG_TIMEOUT_FOR_LASTVALUES_SYNC_ms * retries before the
//  Thing finishes its RequestLastValues handshake and the replay lands.
//
//  For CTRL_SYNC_QUIET_MS after each SYNC every callback is ignored, BOOL
//  controls are written back false so the dashboard shows the truth, and
//  controlEnabled is accepted only when it flips false -> true AFTER the
//  window. DISCONNECT (fired on MQTT loss, ArduinoIoTCloudTCP.cpp:460)
//  clears the "have we synced" flag immediately, because a disconnect
//  resets the Thing state machine to Init -> RequestLastValues
//  (ResetCmdId in handle_Disconnect, ArduinoIoTCloudTCP.cpp:455-456), so
//  the next reconnect replays the stored values again and must be treated
//  exactly like the first boot. So after every boot AND after every
//  reconnect the operator turns the master switch off and on again before
//  anything reaches the PLC.
// =====================================================================

#include "thingProperties.h"
#include "shared.h"
#include "plc_tags.h"
#include "cloud_side.h"

#define CTRL_SYNC_QUIET_MS  15000

static bool          s_syncSeen  = false;   // a SYNC event has been seen since the last (dis)connect
static unsigned long s_syncMs    = 0;
static CtrlState     s_ctrlLocal = {};

//  Fired by the library AFTER it has replayed the cloud's stored values
//  through the onChange callbacks. Everything replayed before this point
//  was ignored (ctrlQuiet() was true); from here a 15 s window still
//  applies, and control is disarmed so the operator has to flip the
//  master switch on again.
static void ctrlOnCloudSync() {
  SHARED_ASSERT_ON_CLOUD();
  s_syncSeen = true;
  s_syncMs   = millis();
  s_ctrlLocal.controlEnabled = false;
  sharedCtrlWrite(s_ctrlLocal);
  if ((bool)controlEnabled) controlEnabled = false;   // show the truth on the dashboard
  LOG("[CTRL] cloud SYNC: control disarmed; quiet for "); LOG(CTRL_SYNC_QUIET_MS / 1000); LOGLN(" s");
}

//  Fired when the cloud connection drops. The next connection will replay
//  stored values again, so go back to ignoring until the next SYNC.
static void ctrlOnCloudDisconnect() {
  SHARED_ASSERT_ON_CLOUD();
  s_syncSeen = false;
  s_ctrlLocal.controlEnabled = false;
  sharedCtrlWrite(s_ctrlLocal);
  LOGLN("[CTRL] cloud DISCONNECT: control disarmed; waiting for SYNC");
}

//  Call once in setup(), after ArduinoCloud.begin().
inline void ctrlBegin() {
  ArduinoCloud.addCallback(ArduinoIoTCloudEvent::SYNC,       ctrlOnCloudSync);
  ArduinoCloud.addCallback(ArduinoIoTCloudEvent::DISCONNECT, ctrlOnCloudDisconnect);
}

inline bool ctrlControlEnabled() { return s_ctrlLocal.controlEnabled; }
inline bool          ctrlSyncSeen() { return s_syncSeen; }   // a SYNC since the last (dis)connect
inline unsigned long ctrlSyncMs()   { return s_syncMs; }

static bool ctrlQuiet() {
  return !s_syncSeen || (millis() - s_syncMs) < CTRL_SYNC_QUIET_MS;
}

//  Gates 1-3. Sets lastError on the refusals the operator should see.
static bool ctrlGate(const char* what) {
  if (ctrlQuiet()) {
    LOG("[CTRL] ignored (first sync): "); LOGLN(what);
    lastError = "control: syncing, wait 15 s";
    return false;
  }
  if (!s_ctrlLocal.controlEnabled) {
    LOG("[CTRL] blocked (controlEnabled=false): "); LOGLN(what);
    lastError = "control disabled";
    return false;
  }
  if (!cloudSidePlcConnected()) {
    LOG("[CTRL] blocked (plc offline): "); LOGLN(what);
    lastError = "control: plc offline";
    return false;
  }
  return true;
}

static void ctrlPost(uint16_t tag, bool isBool, float value, const char* what) {
  Cmd c;
  c.tag = tag; c.isBool = isBool; c.value = value;
  if (!sharedCmdPost(c)) {
    LOG("[CTRL] queue full, dropped: "); LOGLN(what);
    lastError = "control: queue full";
    return;
  }
  LOG("[CTRL] queued "); LOG(what); LOG(" = "); LOGLN(value);
}

//  A BOOL control that cannot be honoured is written back false so the
//  dashboard never shows ON for something the PLC was not told.
static void ctrlBool(CloudBool& prop, uint16_t tag, const char* what) {
  SHARED_ASSERT_ON_CLOUD();
  const bool on = (bool)prop;
  LOG("[CTRL] "); LOG(what); LOG(" -> "); LOGLN(on ? "ON" : "OFF");
  if (!ctrlGate(what)) { if (on) prop = false; return; }
  ctrlPost(tag, true, on ? 1.0f : 0.0f, what);
}

void onControlEnabledChange() {
  SHARED_ASSERT_ON_CLOUD();
  const bool on = (bool)controlEnabled;
  LOG("[CTRL] controlEnabled -> "); LOGLN(on ? "TRUE" : "FALSE");
  if (ctrlQuiet()) {
    LOGLN("[CTRL] ignored (first sync): controlEnabled");
    lastError = "control: syncing, wait 15 s";
    if (on) controlEnabled = false;          // show the truth; operator flips it on again
    return;
  }
  //  Quiet writes any replayed true back to false (ctrlOnCloudSync /
  //  ctrlOnCloudDisconnect), so a true seen here, after the window, can
  //  only be the operator flipping the dashboard switch.
  s_ctrlLocal.controlEnabled = on;
  sharedCtrlWrite(s_ctrlLocal);
  lastError = on ? "control enabled" : "control disabled";
}

void onSystemRunChange()   { ctrlBool(systemRun,   CMD_START_BUTTON, "systemRun");   }
void onStopButtonChange()  { ctrlBool(stopButton,  CMD_STOP_BUTTON,  "stopButton");  }
void onResetButtonChange() { ctrlBool(resetButton, CMD_RESET_BUTTON, "resetButton"); }
void onPurgeButtonChange() { ctrlBool(purgeButton, CMD_PURGE_BUTTON, "purgeButton"); }
void onHpEnableChange()    { ctrlBool(hpEnable,    CMD_HP_ENABLE,    "hpEnable");    }
void onHpModeCoolChange()  { ctrlBool(hpModeCool,  CMD_HP_MODE_COOL, "hpModeCool");  }

//  Setpoints arrive in MINUTES. Clamp here, reflect the clamped value to
//  the cloud (a local assignment publishes without re-entering the
//  callback), post the minutes; the PLC thread does the x60000.
void onAdsorpTimeMsChange() {
  SHARED_ASSERT_ON_CLOUD();
  LOG("[CTRL] adsorpTime (min) -> "); LOGLN((int)adsorpTimeMs);
  if (!ctrlGate("adsorpTime")) return;
  int32_t mins = (int32_t)adsorpTimeMs;
  if (mins < ADSORP_TIME_MIN_MIN) mins = ADSORP_TIME_MIN_MIN;
  if (mins > ADSORP_TIME_MAX_MIN) mins = ADSORP_TIME_MAX_MIN;
  if (mins != (int32_t)adsorpTimeMs) { LOG("[CTRL] clamped to "); LOGLN(mins); adsorpTimeMs = mins; }
  ctrlPost(CMD_ADSORP_TIME_MS, false, (float)mins, "adsorpTime");
}

void onDesorpTimeMsChange() {
  SHARED_ASSERT_ON_CLOUD();
  LOG("[CTRL] desorpTime (min) -> "); LOGLN((int)desorpTimeMs);
  if (!ctrlGate("desorpTime")) return;
  int32_t mins = (int32_t)desorpTimeMs;
  if (mins < DESORP_TIME_MIN_MIN) mins = DESORP_TIME_MIN_MIN;
  if (mins > DESORP_TIME_MAX_MIN) mins = DESORP_TIME_MAX_MIN;
  if (mins != (int32_t)desorpTimeMs) { LOG("[CTRL] clamped to "); LOGLN(mins); desorpTimeMs = mins; }
  ctrlPost(CMD_DESORP_TIME_MS, false, (float)mins, "desorpTime");
}

//  batch 13: manual S4 / Lefoo pump. ON passes ctrlGate AND needs the
//  sequencer idle (actionWord == 0) -- a manual output under a running cycle
//  would fight the PLC. OFF passes ctrlGate only: stopping is always allowed.
//  The switch is written back to the PLC's real state on refusal; after a
//  posted command the mirror in cloud_side.h is held 6 s so it does not flick
//  back before the PLC has acted.
#define MAN_MIRROR_HOLD_MS 6000UL
static unsigned long s_manHoldUntil = 0;
bool ctrlManualHoldActive() { return (long)(millis() - s_manHoldUntil) < 0; }

static void ctrlManual(CloudBool& prop, uint16_t tag, const char* what, bool actual) {
  SHARED_ASSERT_ON_CLOUD();
  const bool on = (bool)prop;
  LOG("[CTRL] "); LOG(what); LOG(" -> "); LOGLN(on ? "ON" : "OFF");
  if (!ctrlGate(what)) { if (on != actual) prop = actual; return; }
  if (on && cloudSideSnapshot().actionWord != 0) {
    LOGLN("[CTRL] refused: cycle running");
    lastError = "manual: cycle running";
    if (on != actual) prop = actual;
    return;
  }
  ctrlPost(tag, true, on ? 1.0f : 0.0f, what);
  s_manHoldUntil = millis() + MAN_MIRROR_HOLD_MS;
}

void onManS4OpenChange() {
  const PlcSnapshot& s = cloudSideSnapshot();
  ctrlManual(manS4Open, CMD_MAN_S4, "manS4Open", s.valveOk[VSLOT_POS_S4] && s.valve[VSLOT_POS_S4] >= 50.0f);
}
void onManCondPumpChange() {
  const PlcSnapshot& s = cloudSideSnapshot();
  ctrlManual(manCondPump, CMD_MAN_PUMP, "manCondPump", s.valveOk[VSLOT_P_COND] && s.valve[VSLOT_P_COND] > 0.5f);
}

//  batch 12: momentary clear-fault buttons. Same three gates as the machine
//  controls (a fault bit reset may let stopped equipment restart), momentary
//  like flowResetTotal: only a true acts, and the switch is always written
//  back false so a replayed true after reconnect cannot fire twice.
static void ctrlClearFault(CloudBool& prop, uint16_t tag, const char* what) {
  SHARED_ASSERT_ON_CLOUD();
  if (!(bool)prop) return;
  LOG("[CTRL] "); LOG(what); LOGLN(" pressed");
  if (ctrlGate(what)) ctrlPost(tag, true, 1.0f, what);
  prop = false;
}

void onClearPressErrorChange() { ctrlClearFault(clearPressError, CMD_CLEAR_PRESS_ERROR, "clearPressError"); }
void onClearTempErrorChange()  { ctrlClearFault(clearTempError,  CMD_CLEAR_TEMP_ERROR,  "clearTempError");  }
void onClearGenErrorChange()   { ctrlClearFault(clearGenError,   CMD_CLEAR_GEN_ERROR,   "clearGenError");   }

//  Momentary reset of the water totals. NOT behind controlEnabled: that gate
//  means "a human is operating the machine remotely"; zeroing a counter is
//  bookkeeping. The quiet period still applies -- a replayed true must not
//  wipe the totals -- and the switch is always written back false. With the
//  PLC away the command is dropped and reported ("control: plc offline");
//  press again when it is back.
void onFlowResetTotalChange() {
  SHARED_ASSERT_ON_CLOUD();
  if (!(bool)flowResetTotal) return;
  if (ctrlQuiet()) {
    LOGLN("[CTRL] ignored (first sync): flowResetTotal");
    flowResetTotal = false;
    return;
  }
  ctrlPost(CMD_FLOW_RESET_TOTAL, true, 1.0f, "flowResetTotal");
  flowResetTotal = false;
}

#endif // CLOUD_CTRL_H
