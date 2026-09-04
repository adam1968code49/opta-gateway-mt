#ifndef CLOUD_CTRL_H
#define CLOUD_CTRL_H

// =====================================================================
//  MAIN THREAD ONLY. Dashboard -> PLC, without touching eip.
//
//  The seven READWRITE callbacks run inside ArduinoCloud.update(). Each
//  one passes three gates and then posts a Cmd on the queue; the PLC
//  thread writes it on its next tick (<= 2 s). Nothing here blocks.
//
//  First-sync quiet period. On connect the library delivers the values
//  the cloud has stored for every READWRITE property as onChange, in no
//  particular order. For CTRL_SYNC_QUIET_MS after the first connection
//  every callback is ignored, BOOL controls are written back false so the
//  dashboard shows the truth, and controlEnabled is accepted only when it
//  flips false -> true AFTER the window. So after every boot the operator
//  turns the master switch off and on again before anything reaches the
//  PLC. A reconnect later does not restart the window.
// =====================================================================

#include "thingProperties.h"
#include "shared.h"
#include "plc_tags.h"
#include "cloud_side.h"

#define CTRL_SYNC_QUIET_MS  15000

static bool          s_cloudSeen      = false;
static unsigned long s_cloudFirstUpMs = 0;
static CtrlState     s_ctrlLocal      = {};

//  Call once per main pass.
inline void ctrlPoll() {
  if (!s_cloudSeen && ArduinoCloud.connected()) {
    s_cloudSeen = true;
    s_cloudFirstUpMs = millis();
    LOG("[CTRL] cloud up; ignoring stored control values for "); LOG(CTRL_SYNC_QUIET_MS / 1000); LOGLN(" s");
  }
}

inline bool ctrlControlEnabled() { return s_ctrlLocal.controlEnabled; }

static bool ctrlQuiet() {
  return !s_cloudSeen || (millis() - s_cloudFirstUpMs) < CTRL_SYNC_QUIET_MS;
}

//  Gates 1-3. Sets lastError on the refusals the operator should see.
static bool ctrlGate(const char* what) {
  if (ctrlQuiet()) {
    LOG("[CTRL] ignored (first sync): "); LOGLN(what);
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
  SHARED_ASSERT_ON_MAIN();
  const bool on = (bool)prop;
  LOG("[CTRL] "); LOG(what); LOG(" -> "); LOGLN(on ? "ON" : "OFF");
  if (!ctrlGate(what)) { if (on) prop = false; return; }
  ctrlPost(tag, true, on ? 1.0f : 0.0f, what);
}

void onControlEnabledChange() {
  SHARED_ASSERT_ON_MAIN();
  const bool on = (bool)controlEnabled;
  LOG("[CTRL] controlEnabled -> "); LOGLN(on ? "TRUE" : "FALSE");
  if (ctrlQuiet()) {
    LOGLN("[CTRL] ignored (first sync): controlEnabled");
    if (on) controlEnabled = false;          // show the truth; operator flips it on again
    return;
  }
  s_ctrlLocal.controlEnabled = on;
  sharedCtrlWrite(s_ctrlLocal);
  lastError = on ? "control enabled" : "control disabled";
}

void onSystemRunChange()   { ctrlBool(systemRun,   CMD_START_BUTTON, "systemRun");   }
void onStopButtonChange()  { ctrlBool(stopButton,  CMD_STOP_BUTTON,  "stopButton");  }
void onResetButtonChange() { ctrlBool(resetButton, CMD_RESET_BUTTON, "resetButton"); }
void onPurgeButtonChange() { ctrlBool(purgeButton, CMD_PURGE_BUTTON, "purgeButton"); }

//  Setpoints arrive in MINUTES. Clamp here, reflect the clamped value to
//  the cloud (a local assignment publishes without re-entering the
//  callback), post the minutes; the PLC thread does the x60000.
void onAdsorpTimeMsChange() {
  SHARED_ASSERT_ON_MAIN();
  LOG("[CTRL] adsorpTime (min) -> "); LOGLN((int)adsorpTimeMs);
  if (!ctrlGate("adsorpTime")) return;
  int32_t mins = (int32_t)adsorpTimeMs;
  if (mins < ADSORP_TIME_MIN_MIN) mins = ADSORP_TIME_MIN_MIN;
  if (mins > ADSORP_TIME_MAX_MIN) mins = ADSORP_TIME_MAX_MIN;
  if (mins != (int32_t)adsorpTimeMs) { LOG("[CTRL] clamped to "); LOGLN(mins); adsorpTimeMs = mins; }
  ctrlPost(CMD_ADSORP_TIME_MS, false, (float)mins, "adsorpTime");
}

void onDesorpTimeMsChange() {
  SHARED_ASSERT_ON_MAIN();
  LOG("[CTRL] desorpTime (min) -> "); LOGLN((int)desorpTimeMs);
  if (!ctrlGate("desorpTime")) return;
  int32_t mins = (int32_t)desorpTimeMs;
  if (mins < DESORP_TIME_MIN_MIN) mins = DESORP_TIME_MIN_MIN;
  if (mins > DESORP_TIME_MAX_MIN) mins = DESORP_TIME_MAX_MIN;
  if (mins != (int32_t)desorpTimeMs) { LOG("[CTRL] clamped to "); LOGLN(mins); desorpTimeMs = mins; }
  ctrlPost(CMD_DESORP_TIME_MS, false, (float)mins, "desorpTime");
}

#endif // CLOUD_CTRL_H
