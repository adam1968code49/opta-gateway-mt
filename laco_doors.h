#ifndef LACO_DOORS_H
#define LACO_DOORS_H

// =====================================================================
//  PLC THREAD ONLY. Included from plc_thread.h AFTER beatReadReals(),
//  clearSay() and the eip extern -- it uses all three.
//
//  The two LACO chamber doors, commanded THROUGH the PLC.
//
//  Why not Modbus straight to the LACO BOXIO: the IP2 controller already
//  runs the full LACO Modbus/TCP master (LACO_MsgCreate/Open/Read/Write,
//  LACO_CoilBytes, LACO_DiBytes -- all present in the 2026-09-02 tag
//  export) and rewrites every one of the 24 coils on its own scan. A
//  second master would have its coils overwritten within 100 ms, the same
//  way the PLC overwrote a manual Cond_Pump write on 2026-09-11. The bits
//  that program reads as ITS input -- LACO.<side>.Cmd_DoorOpen and
//  Cmd_ClampsOpen -- are where a request survives, so that is where the
//  gateway writes.
//
//  One switch per chamber moves both of its sides, as asked. But the two
//  sides are NOT commanded in the same breath: the air supply cannot
//  drive several actuators at once (debug handbook, 2026-06-13: "never
//  move several sides at once -- it collapses the instrument air and
//  trips AIR_LOW; move one side at a time"). Side A goes first, side B
//  follows LACO_STAGGER_MS later, from one press.
//
//  Open is gated on the sequencer being idle, the PLC's LACO link up, air
//  OK, and the chamber vented. Close needs only the sequencer idle: shut
//  is the safe state. LACO's own PLC still refuses whatever its own
//  interlocks forbid -- a door will not open under vacuum whatever we ask.
// =====================================================================

#define LACO_STAGGER_MS   10000UL   // side B follows side A by this much (air supply)
#define LACO_SETTLE_MS    30000UL   // then judge the result from the door limit pairs
#define LACO_I_AIR        8
#define LACO_I_COMM       9
#define LACO_N_TAGS       10

//  Slot order: TopA, TopB, BotA, BotB, each as (clamps, door), then the
//  two status bits. Side index s -> clamps 2s, door 2s+1; matches
//  PlcSnapshot::doorState[s] and SSLOT_DOOR_*0 + s.
static constexpr const char* const LACO_TAGS[LACO_N_TAGS] = {   // constexpr: the assert below reads it
  TAG_LACO_TOPA_CLAMPS, TAG_LACO_TOPA_DOOR,
  TAG_LACO_TOPB_CLAMPS, TAG_LACO_TOPB_DOOR,
  TAG_LACO_BOTA_CLAMPS, TAG_LACO_BOTA_DOOR,
  TAG_LACO_BOTB_CLAMPS, TAG_LACO_BOTB_DOOR,
  TAG_LACO_AIR_OK, TAG_LACO_COMM_OK
};
static const char* const LACO_SIDE_N[4] = { "TA", "TB", "BA", "BB" };

//  The table order is load-bearing: side s uses LACO_TAGS[2s]/[2s+1] and
//  doorState[s] / SSLOT_DOOR_*0 + s. Pin it the way plc_tags.h pins its slots.
static_assert(tagSlotIs(LACO_TAGS[0], TAG_LACO_TOPA_CLAMPS) &&
              tagSlotIs(LACO_TAGS[1], TAG_LACO_TOPA_DOOR)   &&
              tagSlotIs(LACO_TAGS[6], TAG_LACO_BOTB_CLAMPS) &&
              tagSlotIs(LACO_TAGS[7], TAG_LACO_BOTB_DOOR)   &&
              tagSlotIs(LACO_TAGS[LACO_I_AIR],  TAG_LACO_AIR_OK) &&
              tagSlotIs(LACO_TAGS[LACO_I_COMM], TAG_LACO_COMM_OK),
              "LACO_TAGS order drifted from the side indexing");

static float s_lacoVal[LACO_N_TAGS];
static bool  s_lacoOk[LACO_N_TAGS]  = { false };
static bool  s_lacoSeen = false;      // the poll has run at least once
static bool  s_lacoAborted = false;   // a job was killed by a PLC outage; say so once

inline bool lacoAirOk()  { return s_lacoOk[LACO_I_AIR]  && s_lacoVal[LACO_I_AIR]  > 0.5f; }
inline bool lacoCommOk() { return s_lacoOk[LACO_I_COMM] && s_lacoVal[LACO_I_COMM] > 0.5f; }

//  Read the eight command bits plus Air_OK / Comm_OK, and render them.
//  Kept out of the state sweep on purpose: if the PLC's LACO program is
//  not running these ten fail together, and that must not be mistaken for
//  the sequencer tags failing -- plcReadFails stays about the machine.
static void lacoPoll(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  wdWherePlc(WD_AT_LACO);
  beatReadReals(LACO_TAGS, s_lacoVal, s_lacoOk, LACO_N_TAGS);
  s_lacoSeen = true;

  int unread = 0;
  for (uint8_t i = 0; i < LACO_N_TAGS; i++) if (!s_lacoOk[i]) unread++;

  char*  s = w.lacoStat;
  size_t cap = PlcSnapshot::LACOSTAT_CAP, used = 0;
  s[0] = 0;
  if (unread == LACO_N_TAGS) {                 // no such tags: program absent
    snprintf(s, cap, "LACO program not in the PLC");
    return;
  }
  used += snprintf(s + used, cap - used, "air:%s comm:%s",
                   s_lacoOk[LACO_I_AIR]  ? (lacoAirOk()  ? "1" : "0") : "-",
                   s_lacoOk[LACO_I_COMM] ? (lacoCommOk() ? "1" : "0") : "-");
  //  Each side as clamps,door exactly as the PLC holds them now.
  for (uint8_t k = 0; k < 4 && used + 8 < cap; k++) {
    char c = s_lacoOk[k * 2]     ? (s_lacoVal[k * 2]     > 0.5f ? '1' : '0') : '-';
    char d = s_lacoOk[k * 2 + 1] ? (s_lacoVal[k * 2 + 1] > 0.5f ? '1' : '0') : '-';
    used += snprintf(s + used, cap - used, " %s:%c%c", LACO_SIDE_N[k], c, d);
  }
}

//  One chamber's job: side A now, side B after the stagger, then judge.
struct LacoJob {
  bool          active;
  bool          open;
  uint8_t       step;        // 0 = write A, 1 = stagger, 2 = write B, 3 = settle
  unsigned long atMs;
  uint8_t       sideA;       // 0 = TopA (chamber "top"), 2 = BotA
};
static LacoJob s_lacoTop = { false, false, 0, 0, 0 };
static LacoJob s_lacoBot = { false, false, 0, 0, 2 };

inline bool lacoBusy() { return s_lacoTop.active || s_lacoBot.active; }

//  Clamps and door are one request. If the second write fails the first is
//  put back: "clamps released, door still shut" is the one state nobody
//  asked for (review C2). Names the tag that actually failed.
static bool lacoWriteSide(PlcSnapshot& w, uint8_t side, bool open) {
  const bool okC = eip.writeBool(LACO_TAGS[side * 2],     open);   // clamps
  wdBeatPlc();
  if (!okC) { clearSay(w, "write %s failed", LACO_TAGS[side * 2]); return false; }
  const bool okD = eip.writeBool(LACO_TAGS[side * 2 + 1], open);   // door
  wdBeatPlc();
  if (!okD) {
    eip.writeBool(LACO_TAGS[side * 2], !open);                     // put the clamps back
    wdBeatPlc();
    clearSay(w, "write %s failed", LACO_TAGS[side * 2 + 1]);
    return false;
  }
  return true;
}

//  Gates for a new request. Everything the operator could be told is told
//  here, in one place, so a refusal always names its reason.
static bool lacoGate(PlcSnapshot& w, bool top, bool open) {
  if (w.actionWord != 0)                 { clearSay(w, "%s", "laco: cycle running");      return false; }
  if (!s_lacoSeen)                       { clearSay(w, "%s", "laco: not polled yet");     return false; }
  if (!lacoCommOk())                     { clearSay(w, "%s", "laco: plc link to LACO down"); return false; }
  if (!open) return true;                // closing needs nothing more
  if (!lacoAirOk())                      { clearSay(w, "%s", "laco: control air low");    return false; }
  //  Vented check on the near side; LACO interlocks the far side itself.
  const size_t slot = top ? SSLOT_P1_TOP : SSLOT_P2_BOT;
  if (!w.ok[slot])                       { clearSay(w, "%s", "laco: chamber pressure unread"); return false; }
  if (w.sensor[slot] < LACO_VENTED_MBAR) { clearSay(w, "%s", "laco: chamber not vented");  return false; }
  return true;
}

//  Accept a dashboard request. Returns false (with lastError set) when a
//  gate refused; the caller writes the switch back to the truth.
static bool lacoStart(PlcSnapshot& w, bool top, bool open) {
  SHARED_ASSERT_ON_PLC();
  LacoJob& j = top ? s_lacoTop : s_lacoBot;
  //  Flipping the switch back while the doors are still moving must stop
  //  them, not be refused as "busy" (review C1): turn the job round from
  //  the top, so side A is re-commanded first and side B follows again.
  if (j.active && j.open != open) {
    j.open = open; j.step = 0; j.atMs = millis();
    clearSay(w, open ? "%s doors: reversing to open" : "%s doors: reversing to close", top ? "top" : "bottom");
    return true;
  }
  if (j.active)                { clearSay(w, "%s", "laco: already moving that way"); return false; }
  if (lacoBusy())              { clearSay(w, "%s", "laco: other chamber moving");    return false; }
  if (!lacoGate(w, top, open)) return false;
  j.active = true; j.open = open; j.step = 0; j.atMs = millis();
  clearSay(w, open ? "%s doors opening" : "%s doors closing", top ? "top" : "bottom");
  return true;
}

//  Drive both jobs one step per PLC tick. Never writes two sides in the
//  same tick, and never two chambers in the same tick either: the top job
//  runs first, the bottom waits for the next tick.
static void lacoStep(PlcSnapshot& w, LacoJob& j, bool top, bool& wroteThisTick) {
  if (!j.active) return;
  const unsigned long now = millis();
  const char* who = top ? "top" : "bottom";
  switch (j.step) {
    case 0:
      if (wroteThisTick) return;
      //  The sequencer may have started since the request was accepted: the
      //  copy lacoGate saw can be a whole state sweep old (review I4).
      if (j.open && w.actionWord != 0) { clearSay(w, "%s", "laco: cycle started, cancelled"); j.active = false; return; }
      wdWherePlc(WD_AT_LACO);
      if (!lacoWriteSide(w, j.sideA, j.open)) { j.active = false; return; }
      wroteThisTick = true;
      j.step = 1; j.atMs = now;
      break;
    case 1:
      if ((long)(now - (j.atMs + LACO_STAGGER_MS)) < 0) return;   // let the air recover
      j.step = 2;
      break;
    case 2:
      if (wroteThisTick) return;
      if (j.open && w.actionWord != 0) { clearSay(w, "%s", "laco: cycle started, cancelled"); j.active = false; return; }
      wdWherePlc(WD_AT_LACO);
      if (!lacoWriteSide(w, j.sideA + 1, j.open)) { j.active = false; return; }
      wroteThisTick = true;
      j.step = 3; j.atMs = now;
      break;
    case 3: {
      if ((long)(now - (j.atMs + LACO_SETTLE_MS)) < 0) return;
      j.active = false;
      //  Judge from the door limit pairs, not from what we asked.
      const uint8_t want = j.open ? DOOR_OPEN : DOOR_SHUT;
      const uint8_t a = w.doorState[j.sideA], b = w.doorState[j.sideA + 1];
      if (a == DOOR_UNKNOWN || b == DOOR_UNKNOWN)
        clearSay(w, "%s doors: no limit reading", who);        // a failed sweep is not a failed door
      else if (a == DOOR_CONFLICT || b == DOOR_CONFLICT)
        clearSay(w, "%s doors: limit sensors conflict", who);
      else if (a == want && b == want) clearSay(w, j.open ? "%s doors open" : "%s doors shut", who);
      else if (a != want && b != want) clearSay(w, "%s doors did not move", who);
      else {
        char msg[PlcSnapshot::LASTERR_CAP];
        snprintf(msg, sizeof msg, "%s: %s did not move", who,
                 LACO_SIDE_N[a == want ? j.sideA + 1 : j.sideA]);
        clearSay(w, "%s", msg);
      }
      break;
    }
    default: j.active = false; break;
  }
}

static void lacoTick(PlcSnapshot& w) {
  SHARED_ASSERT_ON_PLC();
  if (s_lacoAborted) {
    s_lacoAborted = false;
    clearSay(w, "%s", "laco: aborted, doors mid-travel");
  }
  bool wrote = false;
  lacoStep(w, s_lacoTop, true,  wrote);
  lacoStep(w, s_lacoBot, false, wrote);
  w.lacoJobs = (uint8_t)((s_lacoTop.active ? 1 : 0) | (s_lacoBot.active ? 2 : 0));
}

//  Abandon anything in flight: the link to the PLC went away, so neither
//  the writes nor the verdict mean anything now.
static void lacoAbort() {
  if (s_lacoTop.active || s_lacoBot.active) s_lacoAborted = true;   // reported on the next tick that has a snapshot
  s_lacoTop.active = false;
  s_lacoBot.active = false;
  s_lacoSeen = false;        // air / link values predate the outage (review I2)
}

#endif // LACO_DOORS_H
