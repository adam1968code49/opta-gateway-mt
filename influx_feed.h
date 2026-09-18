#ifndef INFLUX_FEED_H
#define INFLUX_FEED_H

// =====================================================================
//  CLOUD THREAD ONLY. Batch 17: the board is IP2's only writer to InfluxDB.
//
//  AWH_Bridge no longer forwards IP_2_thing (Adam switched it off on
//  2026-09-18), so everything the Grafana dashboards read must come from
//  here, under the same tags (thing_name=IP_2_thing, measurement
//  arduino_iot, fields value / value_str) so no dashboard changes.
//
//  Four layers, one drain:
//    1  the 10 s ring in influx_replay.h -- 28 sensors, flow, totals,
//       plcConnected. Captured every 10 s whether the cloud is up or not,
//       360 deep (1 h), persisted before a ladder reset. Unchanged format.
//    2  slow analogs every FEED_SLOW_MS (120 s): heat-pump temperatures /
//       pressures / current, water totals, board diagnostics. A 30-deep
//       ring = 1 h. Not persisted (a slow variable's last minutes are not
//       worth a KVStore write during a reset).
//    3  on-change discretes: valves, pumps, faults, heat-pump status bits
//       and words, setpoints, the proportional valve positions (0.5
//       deadband), sequencer words, counters, the control switches. Each
//       new PLC snapshot (2 s) is compared with what was last SENT; a
//       change is queued with its epoch. First valid reading after boot is
//       "changed", so a full baseline goes out once. 256-deep queue.
//    4  strings: lastError, plcFailTag, plcStateText, doorStat, lacoStat,
//       fwVersion, bootReason -> value_str, on change. 16-deep queue.
//
//  Every FEED period (= RP_PERIOD_MS, 10 s) the layer-1 capture arms a
//  flush; a flush posts ONE body per cloud pass (influxPush paces at 2 s
//  after a 2xx) until all four queues are empty. A body is filled in layer
//  order, each item "fits or waits for the next POST": layer 1 up to
//  RP_PER_POST records, then layer 2 records, then layer-3 entries, then
//  layer-4 entries. 2xx pops what was sent; 400/413/422 pops it too (the
//  server read those bytes and refused them: retrying forever would wedge
//  the head, review C3 of batch 11); anything else keeps the data and
//  backs off (influxPush). Steady state: one POST every 10 s carrying one
//  layer-1 record (~3 KB) plus whatever changed. After a 1 h outage:
//  3 layer-1 records per POST, 2 s apart, ~4 min to catch up, the other
//  layers riding along in the space left in each body.
//
//  Gates carried over from batch 11: RP_SETTLE_MS after the Arduino Cloud
//  comes back (MQTT is busy re-syncing), RP_LIVE_AFTER_MS into an outage
//  before the probe path is used, and influxPushReady() (configured, not
//  during an OTA download, WiFi up, path real, backoff elapsed).
//
//  Timestamps are whole seconds (precision=s). Every numeric goes as a
//  float `value=`, booleans as 0.000/1.000, matching the field type the
//  bridge established -- a mixed type on one field is a rejected write.
//
//  RAM: layer 2 1.9 KB, layer 3 1.5 KB, layer 4 1 KB, all static (17.1 halved
//  them: with the full set heapFree ran 8.2 KB steady / 3 KB at the TLS peak). Zero
//  dynamic allocation, no String, no recursion.
// =====================================================================

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "config.h"
#include "plc_tags.h"
#include "shared.h"
#include "thingProperties.h"
#include "cloud_side.h"
#include "influx_push.h"
#include "influx_replay.h"   // ring, rpLine, rpBuildBatch, rpDropOldest, rpRejectOldest, replayCaptureTick, RP_* gates

#define FEED_SLOW_MS        120000UL
#define FEED_SLOW_MAX       15            // 30 min of layer-2 records (17.1: was 30; heapFree sat at 8.2 KB steady, 3 KB at the TLS peak)
#define FEED_CHG_MAX        128           // layer-3 queue (17.1: was 256)
#define FEED_STR_MAX        8             // layer-4 queue (17.1: was 16)
#define FEED_STR_CAP        120           // text stored per string entry (plcStateText cap is 128)
#define FEED_POS_DEADBAND   0.5f          // proportional valve position: change worth a row
#define FEED_REBASE_MS      600000UL      // review C1: re-send every on-change value every 10 min. The bridge re-sent
                                          // everything on each cloud update; without a heartbeat, Grafana last() over
                                          // a 6 h window on a 3-day-old board returns No Data for every switch and setpoint.
#define FEED_STATUS_MS      300000UL      // review I1: pushStat is a String publish; at most every 5 min in steady state

// ---------------------------------------------------------------------
//  Layer 2: slow analogs
// ---------------------------------------------------------------------
enum : uint8_t { L2_HP_LOOP = 0, L2_HP_ACT1, L2_HP_ACT2, L2_HP_DISCH_T, L2_HP_SUCT_P, L2_HP_DISCH_P,
                 L2_HP_EVAP_T, L2_HP_COND_T, L2_HP_SUCTLINE_T, L2_HP_SUPERHEAT, L2_HP_EEV, L2_HP_COMP_I,
                 L2_HP_OD_IN, L2_HP_OD_OUT, L2_HP_ID_IN,                                   // 0..14 = HP_REAL_TAGS slots 0..14
                 L2_HP_DATA_AGE,
                 L2_W_HMI_TOTAL, L2_W_CUM, L2_W_FLOW_TOTAL, L2_W_OWED, L2_W_TRIP,
                 L2_D_UPTIME, L2_D_HEAP, L2_D_STACK, L2_D_RSSI, L2_D_STALL_MS, L2_D_STALL_WHERE,
                 L2_D_LOOP_MS, L2_D_CLOUD_MS, L2_D_EIP_MS,
                 L2_N };
static const char* const FEED_SLOW_VAR[L2_N] = {
  "hpLoopTemp", "hpStage1ActivT", "hpStage2ActivT", "hpDischTemp", "hpSuctionPress", "hpDischPress",
  "hpEvapTemp", "hpCondTemp", "hpSuctionLineT", "hpSuperheat", "hpEevPosition", "hpCompCurrent",
  "hpOutdoorIn", "hpOutdoorOut", "hpIndoorIn",
  "hpDataAgeS",
  "hmiWaterTotal", "cumulativeWaterVolume", "flowTotal", "waterOwedL", "tripWaterVolume",
  "uptimeS", "heapFree", "stackFree", "wifiRssi", "loopStallMs", "stallWhere",
  "loopMs", "cloudMs", "eipMs"
};
static_assert(L2_N == 30, "layer-2 table is 30 variables (128 B record)");
static_assert(N_HP_REAL >= 15, "layer 2 reads HP_REAL_TAGS slots 0..14");

struct SlowRec { uint32_t epoch; uint32_t mask; float v[L2_N]; };
static_assert(sizeof(SlowRec) == 128, "SlowRec is 128 B");
static SlowRec       s_l2[FEED_SLOW_MAX];
static uint16_t      s_l2Head = 0, s_l2Count = 0;
static unsigned long s_l2LastMs = 0;
static uint32_t      s_l2Dropped = 0;

static void feedCaptureSlow(const PlcSnapshot& s, uint32_t epoch) {
  SlowRec& r = s_l2[s_l2Head];
  r.epoch = epoch; r.mask = 0;
  for (uint8_t k = 0; k < L2_N; k++) r.v[k] = 0.0f;
  for (uint8_t k = 0; k <= L2_HP_ID_IN; k++)
    if (s.hpRealOk[k]) { r.v[k] = s.hpReal[k]; r.mask |= (1UL << k); }
  if (s.hpSeq) { r.v[L2_HP_DATA_AGE] = (float)s.hpDataAgeS; r.mask |= (1UL << L2_HP_DATA_AGE); }
  if (s.valveOk[22]) { r.v[L2_W_HMI_TOTAL] = s.valve[22]; r.mask |= (1UL << L2_W_HMI_TOTAL); }
  if (s.valveOk[23]) { r.v[L2_W_CUM] = s.valve[23]; r.mask |= (1UL << L2_W_CUM); }
  r.v[L2_W_FLOW_TOTAL] = s.flowTotal;  r.mask |= (1UL << L2_W_FLOW_TOTAL);
  r.v[L2_W_OWED] = s.waterOwedL;       r.mask |= (1UL << L2_W_OWED);
  if (s.liveTripValid) { r.v[L2_W_TRIP] = s.liveTrip; r.mask |= (1UL << L2_W_TRIP); }
  //  Board diagnostics: the cloud thread's own 30 s samples (thingProperties).
  r.v[L2_D_UPTIME]      = (float)(int)uptimeS;
  r.v[L2_D_HEAP]        = (float)(int)heapFree;
  r.v[L2_D_STACK]       = (float)(int)stackFree;
  r.v[L2_D_RSSI]        = (float)(int)wifiRssi;
  r.v[L2_D_STALL_MS]    = (float)(int)loopStallMs;
  r.v[L2_D_STALL_WHERE] = (float)(int)stallWhere;
  r.v[L2_D_LOOP_MS]     = (float)(int)loopMs;
  r.v[L2_D_CLOUD_MS]    = (float)(int)cloudMs;
  r.v[L2_D_EIP_MS]      = (float)s.eipMs;
  for (uint8_t k = L2_D_UPTIME; k < L2_N; k++) r.mask |= (1UL << k);
  s_l2Head = (uint16_t)((s_l2Head + 1) % FEED_SLOW_MAX);
  if (s_l2Count < FEED_SLOW_MAX) s_l2Count++; else s_l2Dropped++;
}
static inline uint16_t l2OldestIdx() { return (uint16_t)((s_l2Head + FEED_SLOW_MAX - s_l2Count) % FEED_SLOW_MAX); }

//  Append whole layer-2 records while they fit. Returns bytes appended; *used = records.
static size_t feedBuildSlow(char* buf, size_t cap, uint16_t* used) {
  size_t n = 0; *used = 0;
  uint16_t idx = l2OldestIdx();
  for (uint16_t i = 0; i < s_l2Count; i++) {
    const SlowRec& r = s_l2[(idx + i) % FEED_SLOW_MAX];
    size_t mark = n; bool full = false;
    for (uint8_t k = 0; k < L2_N && !full; k++) {
      if (!(r.mask & (1UL << k))) continue;
      int w = rpLine(buf + n, cap - n, FEED_SLOW_VAR[k], r.v[k], r.epoch);
      if (w < 0) full = true; else n += (size_t)w;   // w == 0: unencodable value, skip the line
    }
    if (full) { n = mark; break; }
    (*used)++;
  }
  buf[n] = 0;
  return n;
}
static void l2Pop(uint16_t k) { if (k > s_l2Count) k = s_l2Count; s_l2Count -= k; }

// ---------------------------------------------------------------------
//  Layer 3: on-change discretes
// ---------------------------------------------------------------------
enum FeedKind : uint8_t { K_VB, K_VF, K_HB, K_HR, K_SNAP, K_CLOUD };
enum : uint8_t { S_ACTION, S_STATE, S_READFAILS, S_RESTORES, S_HMI_COUNT, S_FLOW_WR_OK, S_HMI_OK,
                 S_ADSORP_MIN, S_DESORP_MIN, S_FLOW_MIS, S_HP_STALE,
                 C_CTRL_EN, C_SYS_RUN, C_OTA_PENDING };
struct FeedVar { const char* name; uint8_t kind; uint8_t idx; };
static const FeedVar FEED_CHG[] = {
  // valve sweep BOOLs (VALVE_TAGS slots)
  {"valveS1",K_VB,0}, {"valveS5",K_VB,1}, {"valveS6",K_VB,2}, {"valveS7",K_VB,3}, {"valveS10",K_VB,4},
  {"valveV1A",K_VB,5}, {"valveV1B",K_VB,6}, {"valveV2A1",K_VB,7}, {"valveV2A2",K_VB,8}, {"valveV2B",K_VB,9},
  {"pumpScroll",K_VB,10}, {"pumpCond",K_VB,11}, {"pumpHotWater",K_VB,12}, {"pumpColdWater",K_VB,13}, {"boosterVfdRun",K_VB,14},
  {"pressError",K_VB,24}, {"tempError",K_VB,25}, {"genError",K_VB,26},
  {"stStage1",K_VB,27}, {"stStage2",K_VB,28}, {"stAuxHeat",K_VB,29}, {"stIndoorCirc",K_VB,30}, {"stIndoorFlow",K_VB,31},
  {"stOutdoorFlow",K_VB,32}, {"stLockout",K_VB,33}, {"stPhaseFault",K_VB,34}, {"stBACnetControl",K_VB,35},
  // proportional valve positions (0..100), deadband
  {"posS2",K_VF,15}, {"posS3",K_VF,16}, {"posS4",K_VF,17}, {"posS8",K_VF,18}, {"posS9",K_VF,19}, {"posV10",K_VF,20}, {"posV11",K_VF,21},
  // heat pump BOOLs (HP_BOOL_TAGS slots)
  {"hpStage1Call",K_HB,0}, {"hpStage2Call",K_HB,1}, {"hpStage1Sat",K_HB,2}, {"hpStage2Sat",K_HB,3},
  {"hpY1Cmd",K_HB,4}, {"hpY2Cmd",K_HB,5}, {"hpReverseCmd",K_HB,6}, {"hpCoolRequest",K_HB,7},
  {"hpTempValid",K_HB,8}, {"hpEnableSt",K_HB,9}, {"hpManualOvrSt",K_HB,10},
  // heat pump words + setpoints (HP_REAL_TAGS slots 15..28), exact compare
  {"hpOperationMode",K_HR,15}, {"hpLimitsWord",K_HR,16}, {"hpPermAlarms1",K_HR,17}, {"hpPermAlarms2",K_HR,18},
  {"hpBoardFaults",K_HR,19}, {"hpSensorFaults",K_HR,20},
  {"hpHtgSp1",K_HR,21}, {"hpHtgSp2",K_HR,22}, {"hpHtgDelta1",K_HR,23}, {"hpHtgDelta2",K_HR,24},
  {"hpClgSp1",K_HR,25}, {"hpClgSp2",K_HR,26}, {"hpClgDelta1",K_HR,27}, {"hpClgDelta2",K_HR,28},
  // snapshot scalars
  {"plcActionWord",K_SNAP,S_ACTION}, {"plcStateWord",K_SNAP,S_STATE}, {"plcReadFails",K_SNAP,S_READFAILS},
  {"plcTotalRestores",K_SNAP,S_RESTORES}, {"hmiWriteCount",K_SNAP,S_HMI_COUNT},
  {"plcFlowWriteOk",K_SNAP,S_FLOW_WR_OK}, {"hmiWriteOk",K_SNAP,S_HMI_OK},
  {"adsorpTimeMs",K_SNAP,S_ADSORP_MIN}, {"desorpTimeMs",K_SNAP,S_DESORP_MIN},
  {"flowMismatchCount",K_SNAP,S_FLOW_MIS}, {"hpDataStale",K_SNAP,S_HP_STALE},
  // cloud-side switches (the operator's, readable on this thread)
  {"controlEnabled",K_CLOUD,C_CTRL_EN}, {"systemRun",K_CLOUD,C_SYS_RUN}, {"otaPending",K_CLOUD,C_OTA_PENDING},
};
#define FEED_N_CHG (sizeof(FEED_CHG) / sizeof(FEED_CHG[0]))
static_assert(FEED_N_CHG <= 255, "layer-3 index is a uint8_t");
static_assert(N_HP_BOOL >= 11 && N_HP_REAL >= 29 && N_VALVE >= 36, "layer-3 slot indices match the sweep tables");

//  Read one layer-3 variable. Returns false when it has no valid value now
//  (slot unread, PLC away, preset not read).
static bool feedRead(const PlcSnapshot& s, const FeedVar& f, float* out) {
  switch (f.kind) {
    case K_VB: if (!s.valveOk[f.idx])  return false; *out = s.valve[f.idx] > 0.5f ? 1.0f : 0.0f; return true;
    case K_VF: if (!s.valveOk[f.idx])  return false; *out = s.valve[f.idx];                     return true;
    case K_HB: if (!s.hpBoolOk[f.idx]) return false; *out = s.hpBool[f.idx] > 0.5f ? 1.0f : 0.0f; return true;
    case K_HR: if (!s.hpRealOk[f.idx]) return false; *out = s.hpReal[f.idx];                    return true;
    case K_SNAP:
      switch (f.idx) {
        case S_ACTION:     if (!s.stateSeq) return false; *out = (float)s.actionWord;        return true;
        case S_STATE:      if (!s.stateSeq) return false; *out = (float)s.stateWord;         return true;
        case S_READFAILS:  *out = (float)s.valveFails;                                        return true;
        case S_RESTORES:   *out = (float)s.plcTotalRestores;                                  return true;
        case S_HMI_COUNT:  *out = (float)s.hmiWriteCount;                                     return true;
        case S_FLOW_WR_OK: *out = s.plcFlowWriteOk ? 1.0f : 0.0f;                             return true;
        case S_HMI_OK:     *out = s.hmiWriteOk ? 1.0f : 0.0f;                                 return true;
        case S_ADSORP_MIN: if (s.adsorpPreMin <= 0) return false; *out = (float)s.adsorpPreMin; return true;
        case S_DESORP_MIN: if (s.desorpPreMin <= 0) return false; *out = (float)s.desorpPreMin; return true;
        case S_FLOW_MIS:   *out = (float)s.flowMismatchCount;                                 return true;
        case S_HP_STALE:   if (!s.hpSeq) return false; *out = s.hpDataStale ? 1.0f : 0.0f;    return true;
        default: return false;
      }
    case K_CLOUD:
      switch (f.idx) {
        case C_CTRL_EN:     *out = (bool)controlEnabled ? 1.0f : 0.0f; return true;
        case C_SYS_RUN:     *out = (bool)systemRun      ? 1.0f : 0.0f; return true;
        case C_OTA_PENDING: *out = (bool)otaPending     ? 1.0f : 0.0f; return true;
        default: return false;
      }
    default: return false;
  }
}

struct ChgEntry { uint32_t epoch; float val; uint8_t idx; uint8_t pad[3]; };
static_assert(sizeof(ChgEntry) == 12, "ChgEntry is 12 B");
static ChgEntry s_l3[FEED_CHG_MAX];
static uint16_t s_l3Head = 0, s_l3Count = 0;
static uint32_t s_l3Dropped = 0;
static float    s_l3Last[FEED_N_CHG];
static bool     s_l3Sent[FEED_N_CHG];     // a value has been QUEUED at least once (baseline)
static uint32_t s_l3LastSeq = 0;

static void l3Push(uint8_t idx, float val, uint32_t epoch) {
  ChgEntry& e = s_l3[s_l3Head];
  e.epoch = epoch; e.val = val; e.idx = idx; e.pad[0] = e.pad[1] = e.pad[2] = 0;
  s_l3Head = (uint16_t)((s_l3Head + 1) % FEED_CHG_MAX);
  if (s_l3Count < FEED_CHG_MAX) s_l3Count++; else s_l3Dropped++;   // full: the oldest change is overwritten
}
static inline uint16_t l3OldestIdx() { return (uint16_t)((s_l3Head + FEED_CHG_MAX - s_l3Count) % FEED_CHG_MAX); }

static void feedCaptureChanges(const PlcSnapshot& s, uint32_t epoch) {
  if (s.seq == s_l3LastSeq) return;          // same snapshot as last pass: nothing new to compare
  s_l3LastSeq = s.seq;
  for (uint8_t i = 0; i < FEED_N_CHG; i++) {
    float v;
    if (!feedRead(s, FEED_CHG[i], &v)) continue;
    bool changed;
    if (!s_l3Sent[i]) changed = true;
    else if (FEED_CHG[i].kind == K_VF) { float d = v - s_l3Last[i]; if (d < 0) d = -d; changed = d >= FEED_POS_DEADBAND; }
    else changed = v != s_l3Last[i];
    if (!changed) continue;
    s_l3Last[i] = v; s_l3Sent[i] = true;
    l3Push(i, v, epoch);
  }
}

static size_t feedBuildChanges(char* buf, size_t cap, uint16_t* used) {
  size_t n = 0; *used = 0;
  uint16_t idx = l3OldestIdx();
  for (uint16_t i = 0; i < s_l3Count; i++) {
    const ChgEntry& e = s_l3[(idx + i) % FEED_CHG_MAX];
    int w = rpLine(buf + n, cap - n, FEED_CHG[e.idx].name, e.val, e.epoch);
    if (w < 0) break;                          // does not fit: next POST
    n += (size_t)w; (*used)++;                 // w == 0 (unencodable) still consumes the entry
  }
  buf[n] = 0;
  return n;
}
static void l3Pop(uint16_t k) { if (k > s_l3Count) k = s_l3Count; s_l3Count -= k; }
//  Review I2: the server refused a body that carried these changes. s_l3Last
//  already holds the new value, so nothing would re-send it until the NEXT
//  change -- hours for a rare valve. Re-arm those variables; the next snapshot
//  re-queues their current value. Only on refusal: doing this on a ring
//  overwrite would livelock a permanently full queue.
static void l3Rearm(uint16_t k) {
  uint16_t idx = l3OldestIdx();
  if (k > s_l3Count) k = s_l3Count;
  for (uint16_t i = 0; i < k; i++) s_l3Sent[s_l3[(idx + i) % FEED_CHG_MAX].idx] = false;
}

// ---------------------------------------------------------------------
//  Layer 4: strings -> value_str
// ---------------------------------------------------------------------
enum : uint8_t { S4_LASTERR = 0, S4_FAILTAG, S4_STATETEXT, S4_DOORSTAT, S4_LACOSTAT, S4_FW, S4_BOOT, S4_N };
static const char* const FEED_STR_VAR[S4_N] = {
  "lastError", "plcFailTag", "plcStateText", "doorStat", "lacoStat", "fwVersion", "bootReason" };
struct StrEntry { uint32_t epoch; uint8_t idx; char text[FEED_STR_CAP + 3]; };
static_assert(sizeof(StrEntry) == 128, "StrEntry is 128 B");
static StrEntry s_l4[FEED_STR_MAX];
static uint16_t s_l4Head = 0, s_l4Count = 0;
static uint32_t s_l4Dropped = 0;
static char     s_l4Last[S4_N][FEED_STR_CAP + 3];
static bool     s_l4Sent[S4_N];

static const char* feedStrValue(const PlcSnapshot& s, uint8_t idx) {
  switch (idx) {
    case S4_LASTERR:   return s.lastError;
    case S4_FAILTAG:   return s.failTag;
    case S4_STATETEXT: return s.stateSeq ? s.stateText : "";
    case S4_DOORSTAT:  return s.doorStat;
    case S4_LACOSTAT:  return s.lacoStat;
    case S4_FW:        return FW_VERSION;
    case S4_BOOT:      return bootReasonStr();
    default:           return "";
  }
}
static uint32_t s_l4LastSeq = 0;
static void feedCaptureStrings(const PlcSnapshot& s, uint32_t epoch) {
  if (s.seq == s_l4LastSeq) return;            // review M4: one comparison per snapshot, not per 20 ms pass
  s_l4LastSeq = s.seq;
  for (uint8_t i = 0; i < S4_N; i++) {
    const char* v = feedStrValue(s, i);
    if (!v || !v[0]) continue;                       // empty = not known yet (stateText before the first sweep)
    char cut[FEED_STR_CAP + 3];
    snprintf(cut, sizeof cut, "%s", v);
    if (s_l4Sent[i] && strcmp(cut, s_l4Last[i]) == 0) continue;
    snprintf(s_l4Last[i], sizeof s_l4Last[i], "%s", cut);
    s_l4Sent[i] = true;
    StrEntry& e = s_l4[s_l4Head];
    e.epoch = epoch; e.idx = i;
    snprintf(e.text, sizeof e.text, "%s", cut);
    s_l4Head = (uint16_t)((s_l4Head + 1) % FEED_STR_MAX);
    if (s_l4Count < FEED_STR_MAX) s_l4Count++; else s_l4Dropped++;
  }
}
static inline uint16_t l4OldestIdx() { return (uint16_t)((s_l4Head + FEED_STR_MAX - s_l4Count) % FEED_STR_MAX); }

//  One line-protocol string field: value_str="...", with \ and " escaped.
static int feedStrLine(char* out, size_t cap, const char* var, const char* text, uint32_t epoch) {
  char esc[2 * FEED_STR_CAP + 8]; size_t j = 0;
  for (const char* p = text; *p && j < sizeof esc - 3; p++) {
    if (*p == '"' || *p == '\\') esc[j++] = '\\';
    esc[j++] = (*p == '\n' || *p == '\r') ? ' ' : *p;
  }
  esc[j] = 0;
  int n = snprintf(out, cap, "arduino_iot,thing_name=%s,variable_name=%s value_str=\"%s\" %lu\n",
                   INFLUX_THING_NAME, var, esc, (unsigned long)epoch);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}
static size_t feedBuildStrings(char* buf, size_t cap, uint16_t* used) {
  size_t n = 0; *used = 0;
  uint16_t idx = l4OldestIdx();
  for (uint16_t i = 0; i < s_l4Count; i++) {
    const StrEntry& e = s_l4[(idx + i) % FEED_STR_MAX];
    int w = feedStrLine(buf + n, cap - n, FEED_STR_VAR[e.idx], e.text, e.epoch);
    if (w < 0) break;
    n += (size_t)w; (*used)++;
  }
  buf[n] = 0;
  return n;
}
static void l4Pop(uint16_t k) { if (k > s_l4Count) k = s_l4Count; s_l4Count -= k; }

// ---------------------------------------------------------------------
//  Status + the unified tick
// ---------------------------------------------------------------------
inline uint16_t feedQueued2() { return s_l2Count; }
inline uint16_t feedQueued3() { return s_l3Count; }
inline uint16_t feedQueued4() { return s_l4Count; }

static uint32_t s_feedSentItems = 0;      // layer 2/3/4 items acknowledged (layer-1 records are s_rpSentRecs)
static uint32_t s_feedRejItems  = 0;      // layer 2/3/4 items the server refused (rej= keeps meaning layer-1 records)
static unsigned long s_feedPostMs   = 0;  // last POST start: steady-state cadence gate (review C2)
static unsigned long s_feedStatusMs = 0;  // last pushStat publish (review I1)
static unsigned long s_feedRebaseMs = 0;  // last on-change re-arm (review C1)

static void feedStatus(int lastCode) {
  char st[128];
  snprintf(st, sizeof st, "live q=%u/%u/%u/%u sent=%lu+%lu posts=%lu/%lu last=%d drop=%lu rej=%lu+%lu badt=%lu",
           (unsigned)s_rpCount, (unsigned)s_l2Count, (unsigned)s_l3Count, (unsigned)s_l4Count,
           (unsigned long)s_rpSentRecs, (unsigned long)s_feedSentItems,
           (unsigned long)s_rpPosts, (unsigned long)s_rpPostFails, lastCode,
           (unsigned long)(s_rpDropped + s_l2Dropped + s_l3Dropped + s_l4Dropped),
           (unsigned long)s_rpRejected, (unsigned long)s_feedRejItems, (unsigned long)s_rpBadEpoch);
  pushStatSet(st);
}

static bool s_feedFlush = false;          // armed by the 10 s capture; cleared when all queues are empty

//  CLOUD THREAD, every pass, after update() and the ladder.
static void influxFeedTick(unsigned long now, bool cloudUp) {
  SHARED_ASSERT_ON_CLOUD();
  static unsigned long downSince = 0;      // first pass that saw connected() false; 0 = up
  if (!cloudUp) { s_rpCloudUpSince = 0; if (downSince == 0) downSince = now; }
  else          { downSince = 0;          if (s_rpCloudUpSince == 0) s_rpCloudUpSince = now; }

  // ---- heartbeat (review C1): every FEED_REBASE_MS forget what was sent so
  //  every on-change value and string is queued again on the next snapshot.
  //  ~80 items, one extra body per 10 min; keeps last() alive on the dashboards.
  if (s_feedRebaseMs == 0 || now - s_feedRebaseMs >= FEED_REBASE_MS) {
    s_feedRebaseMs = now;
    memset(s_l3Sent, 0, sizeof s_l3Sent);
    memset(s_l4Sent, 0, sizeof s_l4Sent);
  }

  // ---- capture -------------------------------------------------------------
  if (replayCaptureTick(now)) s_feedFlush = true;          // layer 1, every 10 s
  if (cloudSideHasSnapshot()) {
    const PlcSnapshot& s = cloudSideSnapshot();
    time_t tnow = time(nullptr);
    uint32_t epoch = (uint32_t)tnow;
    //  Same RTC sanity as layer 1: no real time, no row (2026-09-11: rows in 2102).
    if (tnow > 0 && epoch >= RP_EPOCH_MIN && epoch <= RP_EPOCH_MAX && now - s.stampMs <= RP_FRESH_MS) {
      if (s_l2LastMs == 0 || now - s_l2LastMs >= FEED_SLOW_MS) { s_l2LastMs = now; feedCaptureSlow(s, epoch); }
      feedCaptureChanges(s, epoch);
      feedCaptureStrings(s, epoch);
    }
  }

  // ---- drain: one POST per pass while a flush is armed and anything is queued
  if (!s_feedFlush) return;
  if (s_rpCount == 0 && s_l2Count == 0 && s_l3Count == 0 && s_l4Count == 0) { s_feedFlush = false; return; }
  if (!cloudUp && now - downSince < RP_LIVE_AFTER_MS) return;         // short blip: not worth a TLS session while MQTT reconnects
  if (cloudUp && now - s_rpCloudUpSince < RP_SETTLE_MS) return;       // just back: let the Thing finish syncing first
  //  Review C2: influxPush stamps its gap at POST entry and a POST takes ~2.3 s,
  //  so back-to-back passes would otherwise POST continuously whenever layer 3
  //  or 4 keeps something queued (a flapping lastError during a PLC download).
  //  Steady state is one POST per RP_PERIOD_MS; a real backlog (more layer-1
  //  records than one body takes, or a half-full change queue) drains at full speed.
  const bool backlog = s_rpCount > RP_PER_POST || s_l3Count >= FEED_CHG_MAX / 2;
  if (!backlog && s_feedPostMs != 0 && now - s_feedPostMs < RP_PERIOD_MS) return;
  if (!influxPushReady(now)) return;

  static char body[INFLUX_MAX_BODY];
  size_t n = 0;
  uint16_t u1 = 0, u2 = 0, u3 = 0, u4 = 0;
  if (s_rpCount) {
    size_t n1 = rpBuildBatch(body, sizeof body, RP_PER_POST, &u1);
    if (u1 == 0) { s_rpCount--; s_rpDropped++; return; }              // a record that cannot be encoded is not worth a hang
    if (n1 == 0) { rpRejectOldest(u1); u1 = 0; }                       // only unusable epochs in that batch: skip them
    n = n1;
  }
  n += feedBuildSlow(body + n, sizeof body - n, &u2);
  n += feedBuildChanges(body + n, sizeof body - n, &u3);
  n += feedBuildStrings(body + n, sizeof body - n, &u4);
  if (n == 0) { l2Pop(u2); l3Pop(u3); l4Pop(u4); return; }           // nothing encodable at the heads: pop and move on

  s_feedPostMs = now;
  int code = influxPush(body, n);
  s_rpPosts++;
  const bool ok = code >= 200 && code < 300;
  const bool refused = code == 400 || code == 413 || code == 422;
  if (ok || refused) {
    if (ok) { rpDropOldest(u1); s_feedSentItems += u2 + u3 + u4; }
    else    { rpRejectOldest(u1); s_feedRejItems += u2 + u3 + u4; l3Rearm(u3); }
    l2Pop(u2); l3Pop(u3); l4Pop(u4);
  }
  if (!ok) s_rpPostFails++;
  //  Review I1: pushStat is a String publish. Only on a failure, when the
  //  queues just emptied, or every FEED_STATUS_MS -- not after every 10 s POST.
  const bool empty = s_rpCount == 0 && s_l2Count == 0 && s_l3Count == 0 && s_l4Count == 0;
  if (!ok || empty || s_feedStatusMs == 0 || now - s_feedStatusMs >= FEED_STATUS_MS) { s_feedStatusMs = now; feedStatus(code); }
  LOG("[FEED] "); LOG((int)u1); LOG("+"); LOG((int)u2); LOG("+"); LOG((int)u3); LOG("+"); LOG((int)u4);
  LOG(" -> code "); LOG(code); LOG(", left "); LOG((int)s_rpCount); LOG("/"); LOG((int)s_l2Count); LOG("/"); LOG((int)s_l3Count); LOG("/"); LOGLN((int)s_l4Count);
}

#endif // INFLUX_FEED_H
