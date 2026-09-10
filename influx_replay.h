#ifndef INFLUX_REPLAY_H
#define INFLUX_REPLAY_H

// =====================================================================
//  CLOUD THREAD ONLY. Batch 11 phase 2: fill the cloud outage gaps in
//  InfluxDB from the board itself.
//
//  While the cloud is down, one record every 10 s from the PLC snapshot
//  (epoch, the 28 sensor slots with an "actually read" mask, the three
//  water figures, plcConnected) into a RAM ring -- 60 min deep. Before a
//  ladder reset the ring goes into the KVStore in 8 KB chunks and is
//  reloaded at boot. Once the cloud has been back for 2 min the ring is
//  drained: three records per POST as line protocol shaped exactly like
//  the forwarder's live points
//
//      arduino_iot,thing_name=IP_2_thing,variable_name=t1HotTank value=23.4 1757500000
//
//  so dashboards need no change. A record is dropped only after a 2xx.
//  Only outage-time records are ever captured, so replay never doubles
//  up on live data; a sensor whose slot was not read that tick is not
//  written (a zero is a reading); a record with no valid RTC is not kept.
//
//  Why the cloud thread: it already consumes the snapshot every pass,
//  knows the cloud state, and owns the BearSSL client (influx_push.h).
// =====================================================================

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>
#include <Arduino_KVStore.h>
#include "config.h"
#include "plc_tags.h"
#include "shared.h"
#include "wd_feeder.h"
#include "boot_reason.h"     // g_kvMutex, KV_LOCK_WAIT_MS
#include "cloud_side.h"      // cloudSideSnapshot()
#include "influx_push.h"

#define RP_PERIOD_MS        10000UL   // one record while offline
#define RP_MAX              360       // 60 min of records; oldest overwritten
#define RP_SETTLE_MS        120000UL  // cloud back this long before draining
#define RP_PER_POST         3         // records per POST: a fully-read record is 32 lines ~2.75 KB, 3 fit in INFLUX_MAX_BODY (4 did not: review C2)
#define RP_CHUNK_RECS       60        // records per KVStore chunk (60 x 136 = 8160 B)
#define RP_KEY_META         "rp_meta"
#define RP_KEY_CHUNK_FMT    "rp_%u"
#define RP_META_MAGIC       0x52500000UL   // 'RP' + layout version 0 in the high half of the meta int; count in the low 16 bits
#define RP_FRESH_MS         (3 * SAMPLE_INTERVAL_MS)   // a snapshot older than this is a stalled PLC thread, not a reading

//  okMask bits 0..27 = sensor slot read this tick. Upper bits:
#define RP_OK_FLOW          (1UL << 28)   // flowRate/flowBatch valid (the meter is the Opta's own: always, when fresh)
#define RP_OK_CUM           (1UL << 29)   // displayWaterVolume valid: PLC base was readable (plcConnected)

struct ReplayRec {
  uint32_t epoch;            // seconds, RTC (NTP-set); never 0 in the ring
  uint32_t okMask;           // bit k = sensor slot k was read this tick
  float    v[28];            // SENSOR_TAGS slot order
  float    flowRate, flowBatch, cum;
  uint8_t  plc;              // plcConnected
  uint8_t  pad[3];
};
static_assert(sizeof(ReplayRec) == 136, "ReplayRec layout is the KVStore format: 136 bytes");
static_assert(N_SENSORS == 28, "ReplayRec::v assumes 28 sensor slots");

//  Cloud variable names in SENSOR_TAGS slot order -- the same contract as
//  cloud_side.h's TAKE list. A wrong entry here writes history under the
//  wrong name with no way to tell later; keep it aligned with that list.
static const char* const RP_SENSOR_VAR[28] = {
  "t1HotTank", "t2ColdTank", "t3TopChIn", "t4TopChOut", "t5BotChIn", "t6BotChOut",
  "t7HotHxIn", "t8HotHxOut", "t9BoosterIn", "t10BoosterOut", "t11CondenserOut", "t12HeatPumpIn",
  "vtVaisalaTemp",
  "p1TopChamber", "p2BotChamber", "p3ChamberRedun", "p4Booster", "p5Collector", "p6VaisalaWvp",
  "hp1HotInlet", "hp1ColdInlet", "hp2HotInlet", "hp2ColdInlet",
  "atmTemp", "atmRh", "atmTempDs", "atmRhDs", "tankLevel"
};

static ReplayRec     s_rp[RP_MAX];
static uint16_t      s_rpHead  = 0;     // next slot to write
static uint16_t      s_rpCount = 0;     // valid records
static unsigned long s_rpLastCapMs = 0;
static unsigned long s_rpCloudUpSince = 0;
static uint32_t      s_rpSentRecs = 0, s_rpPosts = 0, s_rpPostFails = 0, s_rpDropped = 0;
static uint16_t      s_rpLoaded = 0;    // records restored from the KVStore at boot
static uint32_t      s_rpRejected = 0;  // records dropped because the server refused the body itself (400/413/422)

inline uint16_t replayQueued() { return s_rpCount; }

static inline uint16_t rpOldestIdx() { return (uint16_t)((s_rpHead + RP_MAX - s_rpCount) % RP_MAX); }

static void replayCapture(const PlcSnapshot& s, unsigned long now) {
  time_t epoch = time(nullptr);
  if (epoch < (time_t)1600000000L) return;         // no RTC: a record without a time is not a record
  //  A stalled PLC thread leaves s_local frozen with ok[] still true: that
  //  is not 28 readings, it is one reading repeated. Skip the tick.
  if (now - s.stampMs > RP_FRESH_MS) return;
  ReplayRec& r = s_rp[s_rpHead];
  r.epoch = (uint32_t)epoch;
  r.okMask = RP_OK_FLOW;
  for (size_t k = 0; k < 28; k++) { r.v[k] = s.sensor[k]; if (s.ok[k]) r.okMask |= (1UL << k); }
  r.flowRate = s.flowRate; r.flowBatch = s.flowBatch; r.cum = s.liveCum;
  if (s.plcConnected) r.okMask |= RP_OK_CUM;       // the lifetime total is PLC base + owed: no base, no number
  r.plc = s.plcConnected ? 1 : 0;
  r.pad[0] = r.pad[1] = r.pad[2] = 0;
  s_rpHead = (uint16_t)((s_rpHead + 1) % RP_MAX);
  if (s_rpCount < RP_MAX) s_rpCount++; else s_rpDropped++;
}

//  newlib-nano has no %f: fixed 3 decimals, plain digits, no exponent.
static int rpFmtFloat(char* out, size_t cap, float f) {
  if (isnan(f) || isinf(f)) return -1;
  bool neg = f < 0; if (neg) f = -f;
  if (f > 1e9f) return -1;
  unsigned long ip = (unsigned long)f;
  unsigned long fp = (unsigned long)((f - (float)ip) * 1000.0f + 0.5f);
  if (fp >= 1000) { ip++; fp -= 1000; }
  return snprintf(out, cap, "%s%lu.%03lu", neg ? "-" : "", ip, fp);
}

static int rpLine(char* out, size_t cap, const char* var, float val, uint32_t epoch) {
  char num[24];
  if (rpFmtFloat(num, sizeof num, val) < 0) return 0;
  int n = snprintf(out, cap, "arduino_iot,thing_name=%s,variable_name=%s value=%s %lu\n",
                   INFLUX_THING_NAME, var, num, (unsigned long)epoch);
  return (n > 0 && (size_t)n < cap) ? n : -1;
}

//  Build one POST body from the oldest `nrec` records. Returns bytes, and
//  the number of records it consumed in *used.
static size_t rpBuildBatch(char* buf, size_t cap, uint16_t nrec, uint16_t* used) {
  size_t n = 0; *used = 0;
  if (nrec > s_rpCount) nrec = s_rpCount;       // never read past the queue: 2026-09-10 the first live drain
                                                 // encoded two never-written slots (epoch 0) and underflowed the count
  uint16_t idx = rpOldestIdx();
  for (uint16_t i = 0; i < nrec; i++) {
    const ReplayRec& r = s_rp[(idx + i) % RP_MAX];
    if (r.epoch < 1600000000UL) { (*used)++; continue; }   // belt and braces: a record without a real time is skipped, not sent
    size_t mark = n;
    bool full = false;
    for (size_t k = 0; k < 28 && !full; k++) {
      if (!(r.okMask & (1UL << k))) continue;
      int w = rpLine(buf + n, cap - n, RP_SENSOR_VAR[k], r.v[k], r.epoch);
      if (w < 0) { full = true; break; }
      n += (size_t)w;
    }
    const char* wv[3] = { "flowRate", "flowBatch", "displayWaterVolume" };
    float       wf[3] = { r.flowRate, r.flowBatch, r.cum };
    uint32_t    wm[3] = { RP_OK_FLOW, RP_OK_FLOW, RP_OK_CUM };
    for (int k = 0; k < 3 && !full; k++) {
      if (!(r.okMask & wm[k])) continue;
      int w = rpLine(buf + n, cap - n, wv[k], wf[k], r.epoch);
      if (w < 0) { full = true; break; }
      n += (size_t)w;
    }
    if (!full) {
      int w = snprintf(buf + n, cap - n, "arduino_iot,thing_name=%s,variable_name=plcConnected value=%d %lu\n",
                       INFLUX_THING_NAME, (int)r.plc, (unsigned long)r.epoch);
      if (w < 0 || (size_t)w >= cap - n) full = true; else n += (size_t)w;
    }
    if (full) { n = mark; break; }          // this record did not fit: leave it for the next POST
    (*used)++;
  }
  buf[n] = 0;
  return n;
}

static void rpDropOldest(uint16_t k) { if (k > s_rpCount) k = s_rpCount; s_rpCount -= k; s_rpSentRecs += k; }
static void rpRejectOldest(uint16_t k) { if (k > s_rpCount) k = s_rpCount; s_rpCount -= k; s_rpRejected += k; }

//  Persist oldest-first so the loader is a straight read. CLOUD THREAD,
//  quiet (reset path): no LOG after the marker.
static bool replayPersist() {
  if (s_rpCount == 0) return false;
  if (!g_kvMutex.trylock_for(std::chrono::milliseconds(KV_LOCK_WAIT_MS))) return false;
  KVStore kv;
  bool ok = false;
  if (kv.begin()) {
    static ReplayRec chunk[RP_CHUNK_RECS];
    uint16_t idx = rpOldestIdx(), left = s_rpCount, ci = 0;
    ok = true;
    while (left > 0 && ok) {
      uint16_t n = left < RP_CHUNK_RECS ? left : RP_CHUNK_RECS;
      for (uint16_t i = 0; i < n; i++) chunk[i] = s_rp[(idx + i) % RP_MAX];
      char key[12]; snprintf(key, sizeof key, RP_KEY_CHUNK_FMT, (unsigned)ci);
      int w = kv.putBytes(key, (const uint8_t*)chunk, (size_t)n * sizeof(ReplayRec));
      ok = (w >= 0 && (size_t)w == (size_t)n * sizeof(ReplayRec));
      idx = (uint16_t)((idx + n) % RP_MAX); left -= n; ci++;
    }
    if (ok) ok = kv.putInt(RP_KEY_META, (int)(RP_META_MAGIC | s_rpCount)) != 0;   // magic+layout version guards a future ReplayRec change (putInt returns bytes written)
    kv.end();
  }
  g_kvMutex.unlock();
  return ok;
}

//  MAIN, once in setup() before the threads. Restores the ring and clears
//  the keys so a stale copy can never be replayed twice.
inline void replayLoad() {
  KVStore kv;
  if (!kv.begin()) return;
  uint32_t meta = kv.exists(RP_KEY_META) ? (uint32_t)kv.getInt(RP_KEY_META, 0) : 0;
  int cnt = ((meta & 0xFFFF0000UL) == RP_META_MAGIC) ? (int)(meta & 0xFFFFUL) : 0;   // wrong magic/version: ignore, then clean up
  if (meta != 0 && cnt == 0) { kv.remove(RP_KEY_META); for (unsigned k = 0; k < (RP_MAX / RP_CHUNK_RECS) + 1; k++) { char key[12]; snprintf(key, sizeof key, RP_KEY_CHUNK_FMT, k); if (kv.exists(key)) kv.remove(key); } }
  if (cnt > 0 && cnt <= RP_MAX && s_rpCount == 0) {
    uint16_t left = (uint16_t)cnt, ci = 0;
    while (left > 0) {
      uint16_t n = left < RP_CHUNK_RECS ? left : RP_CHUNK_RECS;
      char key[12]; snprintf(key, sizeof key, RP_KEY_CHUNK_FMT, (unsigned)ci);
      int got = kv.getBytes(key, (uint8_t*)&s_rp[s_rpHead], (size_t)n * sizeof(ReplayRec));
      if (got != (int)((size_t)n * sizeof(ReplayRec))) break;
      s_rpHead = (uint16_t)((s_rpHead + n) % RP_MAX); s_rpCount += n; left -= n; ci++;
    }
    s_rpLoaded = s_rpCount;
    //  consumed: remove so a later boot without a fresh persist finds nothing
    for (uint16_t k = 0; k < ci; k++) { char key[12]; snprintf(key, sizeof key, RP_KEY_CHUNK_FMT, (unsigned)k); kv.remove(key); }
    kv.remove(RP_KEY_META);
  }
  kv.end();
  if (s_rpLoaded) { LOG("[REPLAY] restored "); LOG((int)s_rpLoaded); LOGLN(" outage records from the KVStore"); }
}


static void rpStatus(int lastCode) {
  char st[128];
  snprintf(st, sizeof st, "replay q=%u sent=%lu posts=%lu/%lu last=%d drop=%lu rej=%lu",
           (unsigned)s_rpCount, (unsigned long)s_rpSentRecs, (unsigned long)s_rpPosts,
           (unsigned long)s_rpPostFails, lastCode, (unsigned long)s_rpDropped, (unsigned long)s_rpRejected);
  pushStatSet(st);
}

//  CLOUD THREAD, every pass, after update() and the ladder.
static void replayTick(unsigned long now, bool cloudUp) {
  SHARED_ASSERT_ON_CLOUD();
  if (!cloudUp) {
    s_rpCloudUpSince = 0;
    if (cloudSideHasSnapshot() && (s_rpLastCapMs == 0 || now - s_rpLastCapMs >= RP_PERIOD_MS)) {
      s_rpLastCapMs = now;
      replayCapture(cloudSideSnapshot(), now);
    }
    return;
  }
  if (s_rpCloudUpSince == 0) {
    s_rpCloudUpSince = now;
    if (s_rpCount) rpStatus(0);                   // first pass back: the queue is visible on the dashboard before anything is sent
  }
  if (s_rpCount == 0) return;
  if (now - s_rpCloudUpSince < RP_SETTLE_MS) return;
  if (!influxPushReady(now)) return;

  static char body[INFLUX_MAX_BODY];
  uint16_t used = 0;
  size_t n = rpBuildBatch(body, sizeof body, RP_PER_POST, &used);
  if (used == 0) { if (s_rpCount) { s_rpCount--; s_rpDropped++; } return; }   // a record that cannot be encoded is not worth a hang
  if (n == 0) { rpRejectOldest(used); return; }             // only unusable records in this batch: nothing to send
  int code = influxPush(body, n);
  s_rpPosts++;
  if (code >= 200 && code < 300) rpDropOldest(used);
  else {
    s_rpPostFails++;
    //  The server read the body and refused it: retrying the same bytes
    //  forever would wedge the queue head (review C3). Drop that batch,
    //  count it, move on. Auth/bucket errors and transport failures keep
    //  the data and back off (influxPush handles the pacing).
    if (code == 400 || code == 413 || code == 422) rpRejectOldest(used);
  }
  rpStatus(code);
  LOG("[REPLAY] "); LOG((int)used); LOG(" rec -> code "); LOG(code); LOG(", left "); LOGLN((int)s_rpCount);
}

#endif // INFLUX_REPLAY_H
