#ifndef EPISODE_LOG_H
#define EPISODE_LOG_H

// =====================================================================
//  Offline episode log. While the cloud is down the cloud thread writes
//  one short line a minute into a RAM ring; before a ladder reset (and
//  after an episode of 5 min or more heals by itself) the ring goes into
//  the KVStore. The next boot reads it back onto the serial log and the
//  RJ45 page (/episode).
//
//  Why: 2026-09-10 the board reset itself three times in one night, each
//  marker saying "uplink fine, router fine, re-association useless, our
//  own stack wedged" -- and nothing else, because a reset clears RAM and
//  nobody watches the serial port at 04:00. This is the flight recorder
//  for that night. It is not the USB logger (that one costs the USB port).
//
//  Two keys, so a self-healed 6 min wobble at 07:00 cannot overwrite the
//  04:00 reset before anyone has read it. Each block starts with a header
//  line naming why, which boot, and how long the episode had run.
//
//  Ownership: the ring belongs to the cloud thread. s_epiPrev* are written
//  once in setup(), before any thread starts, and read by main (web).
//  KVStore writes take g_kvMutex (boot_reason.h) like every other writer.
// =====================================================================

#include <Arduino.h>
#include <string.h>
#include <stdio.h>
#include <Arduino_KVStore.h>
#include "boot_reason.h"     // g_kvMutex, bootSeq()

#define EPI_PERIOD_MS       60000UL   // one row a minute while offline
#define EPI_ROWS            40        // ring depth: 40 min of an episode, oldest overwritten
#define EPI_ROW_CAP         48        // bytes per row incl. NUL
#define EPI_KEY_RESET       "epi_reset"   // written just before a ladder reset
#define EPI_KEY_HEAL        "epi_heal"    // written when an episode >= EPI_PERSIST_MIN_MS heals by itself
#define EPI_PERSIST_MIN_MS  300000UL  // an episode that heals by itself is kept only if it lasted this long
#define EPI_BLOCK_CAP       (EPI_ROWS * EPI_ROW_CAP + 64)   // rows + header line

static char     s_epiRows[EPI_ROWS][EPI_ROW_CAP];
static uint8_t  s_epiHead  = 0;    // next slot to write
static uint8_t  s_epiCount = 0;    // valid rows (<= EPI_ROWS)

static char     s_epiPrevReset[EPI_BLOCK_CAP] = "";   // last persisted blocks, loaded at boot
static char     s_epiPrevHeal [EPI_BLOCK_CAP] = "";

inline void episodeLogClear() { s_epiHead = 0; s_epiCount = 0; }

inline void episodeLogAdd(const char* line) {
  snprintf(s_epiRows[s_epiHead], EPI_ROW_CAP, "%s", line);
  s_epiHead = (uint8_t)((s_epiHead + 1) % EPI_ROWS);
  if (s_epiCount < EPI_ROWS) s_epiCount++;
}

inline uint8_t episodeLogRows() { return s_epiCount; }

//  Header line, then the rows oldest-first, '\n'-separated. Returns bytes used.
static size_t episodeLogJoin(char* buf, size_t cap, const char* why, unsigned long offMin) {
  int h = snprintf(buf, cap, "# %s n=%lu off=%lum rows=%u\n", why, (unsigned long)bootSeq(), offMin, (unsigned)s_epiCount);
  size_t n = (h > 0 && (size_t)h < cap) ? (size_t)h : 0;
  uint8_t first = (uint8_t)((s_epiHead + EPI_ROWS - s_epiCount) % EPI_ROWS);
  for (uint8_t i = 0; i < s_epiCount; i++) {
    const char* row = s_epiRows[(first + i) % EPI_ROWS];
    size_t len = strlen(row);
    if (n + len + 1 >= cap) break;
    memcpy(buf + n, row, len); n += len;
    buf[n++] = '\n';
  }
  buf[n] = 0;
  return n;
}

//  CLOUD THREAD. quiet=true on the reset path: the marker is already in
//  QSPI and nothing may LOG between it and the reset (a wedged thread may
//  hold g_logMutex). Returns true when the block landed.
static bool episodeLogPersist(const char* why, bool quiet, unsigned long offMin) {
  if (s_epiCount == 0) return false;
  static char buf[EPI_BLOCK_CAP];
  size_t n = episodeLogJoin(buf, sizeof buf, why, offMin);
  const char* key = quiet ? EPI_KEY_RESET : EPI_KEY_HEAL;
  if (!g_kvMutex.trylock_for(std::chrono::milliseconds(KV_LOCK_WAIT_MS))) {
    if (!quiet) LOGLN("[EPI] KVStore busy, episode not saved");
    return false;
  }
  KVStore kv;
  bool ok = false;
  if (kv.begin()) {
    int w = kv.putBytes(key, (const uint8_t*)buf, n);     // res_t is int: negative on failure
    kv.end();
    ok = (w >= 0 && (size_t)w == n);
  }
  g_kvMutex.unlock();
  if (!quiet) {
    LOG("[EPI] "); LOG(ok ? "saved " : "FAILED to save "); LOG((int)s_epiCount);
    LOG(" rows ("); LOG((unsigned long)n); LOG(" B) as "); LOGLN(key);
  }
  return ok;
}

static void episodeLogLoadOne(KVStore& kv, const char* key, char* dst, size_t cap) {
  dst[0] = 0;
  if (!kv.exists(key)) return;
  size_t len = kv.getBytesLength(key);
  if (len >= cap) len = cap - 1;
  int got = kv.getBytes(key, (uint8_t*)dst, len);
  if (got < 0) { dst[0] = 0; return; }
  dst[(size_t)got < len ? (size_t)got : len] = 0;
}

//  MAIN, once in setup() after bootReasonLog(), before the threads start.
inline void episodeLogLoad() {
  KVStore kv;
  if (!kv.begin()) { LOGLN("[EPI] KVStore begin failed"); return; }
  episodeLogLoadOne(kv, EPI_KEY_RESET, s_epiPrevReset, sizeof s_epiPrevReset);
  episodeLogLoadOne(kv, EPI_KEY_HEAL,  s_epiPrevHeal,  sizeof s_epiPrevHeal);
  kv.end();
  if (!s_epiPrevReset[0] && !s_epiPrevHeal[0]) { LOGLN("[EPI] no previous episode"); return; }
  if (s_epiPrevReset[0]) { LOGLN("[EPI] last episode that ended in a reset:"); LOG(s_epiPrevReset); }
  if (s_epiPrevHeal[0])  { LOGLN("[EPI] last episode that healed by itself:"); LOG(s_epiPrevHeal); }
}

inline const char* episodeLogPrevReset() { return s_epiPrevReset; }
inline const char* episodeLogPrevHeal()  { return s_epiPrevHeal; }

#endif // EPISODE_LOG_H
