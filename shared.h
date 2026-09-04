#ifndef SHARED_H
#define SHARED_H

// =====================================================================
//  THE ONLY SURFACE THE TWO THREADS SHARE.
//
//  The PLC thread owns the EtherNet/IP client. Main owns every Cloud*
//  property. They never touch each other's objects. Everything that
//  crosses between them is here: one plain-data snapshot (plc -> main),
//  one plain-data control block (main -> plc, continuous), one queue of
//  one-shot commands (main -> plc). One mutex guards the two structs. The
//  lock is held for a struct copy and nothing else -- never for I/O, never
//  for allocation. Whether a later change can avoid touching this file is
//  the test of whether the thread boundary was drawn correctly.
//
//  Why plain data: seven of the cloud properties are Strings, and a String
//  assigned on one thread while another thread serialises it is a
//  use-after-free. So the snapshot carries char[] and main builds the
//  String from it at assignment time, on main, every time. The
//  static_asserts below make that a compile error to break.
// =====================================================================

#include <type_traits>
#include <string.h>
#include <mbed.h>
#include "rtos/Mutex.h"
#include "rtos/Mail.h"
#include "plc_tags.h"

// ---------------------------------------------------------------------
//  plc -> main. Filled by the PLC thread into its own `working` copy over
//  a whole tick, then published in one copy. The 28 sensor slots follow
//  SENSOR_TAGS order exactly; ok[] says which slots were read this tick so
//  main can leave a stale Cloud* alone rather than publish a zero.
// ---------------------------------------------------------------------
struct PlcSnapshot {
  static constexpr size_t LASTERR_CAP = 48;

  uint32_t seq;            // monotonic, bumped on every publish
  uint32_t stampMs;        // millis() at publish, so main can age it
  float    sensor[N_SENSORS];
  bool     ok[N_SENSORS];
  int32_t  fails;          // slots not read this tick
  bool     plcConnected;
  int32_t  eipMs;          // duration of the last eip.begin() attempt, ms
  uint16_t lastCipStatus;
  char     lastError[LASTERR_CAP];   // "ok", "read fail x3 (cip=0x5)", ...
  uint32_t plcStackFree;   // PLC thread's own stack watermark, bytes
  uint32_t cmdDropped;     // commands lost to a full queue, cumulative
};

// ---------------------------------------------------------------------
//  main -> plc, continuous. Read by the PLC thread each tick.
//  flowRate/flowBatch are batch 2; declared now so the struct's shape is
//  final and no later batch has to touch this file for them.
// ---------------------------------------------------------------------
struct CtrlState {
  bool  controlEnabled;
  float flowRate;
  float flowBatch;
};

// ---------------------------------------------------------------------
//  main -> plc, one-shot. Batch 0 posts none; the queue exists now so the
//  16 onXxxChange callbacks of batch 1 have a home that is not eip.
// ---------------------------------------------------------------------
enum CmdTag : uint16_t {
  CMD_NONE = 0,
  CMD_START_BUTTON, CMD_STOP_BUTTON, CMD_RESET_BUTTON, CMD_PURGE_BUTTON,
  CMD_ADSORP_TIME_MS, CMD_DESORP_TIME_MS,
  CMD_WATER_RESET_TOTAL, CMD_FLOW_RESET_TOTAL,
  CMD_AUTO_ADSORP_ENABLE,
  CMD_HP_ENABLE, CMD_HP_MODE_COOL, CMD_HP_HTG_SP1, CMD_HP_HTG_SP2,
  CMD_HP_HTG_DELTA1, CMD_HP_HTG_DELTA2, CMD_HP_CLG_SP1, CMD_HP_CLG_SP2,
  CMD_HP_CLG_DELTA1, CMD_HP_CLG_DELTA2, CMD_HP_HTG_HIGH_LIMIT, CMD_HP_MANUAL_OVERRIDE,
  CMD_AI_LEARN_ENABLE, CMD_AI_RESET_BASELINE,
};

struct Cmd {
  uint16_t tag;      // a CmdTag
  bool     isBool;   // value is 0/1
  float    value;
};

static_assert(std::is_trivially_copyable<PlcSnapshot>::value,
              "PlcSnapshot must be plain data: no String, no pointers");
static_assert(sizeof(PlcSnapshot) < 1024,
              "PlcSnapshot copy is meant to be a sub-kilobyte memcpy");
static_assert(std::is_trivially_copyable<CtrlState>::value, "CtrlState must be plain data");
static_assert(std::is_trivially_copyable<Cmd>::value, "Cmd must be plain data");

// ---------------------------------------------------------------------
//  The shared objects. One mutex for both structs: they are tiny, the
//  copies are microseconds, and two locks would only add a way to deadlock.
// ---------------------------------------------------------------------
static rtos::Mutex        g_shareMutex;
static PlcSnapshot        g_published = {};
static CtrlState          g_ctrl      = {};
static rtos::Mail<Cmd,16> g_cmdQueue;
static volatile uint32_t  g_cmdDropped = 0;

//  PLC thread only. Copies working into published and stamps it. The seq
//  bump happens inside the lock so main can never see a new seq with an
//  old body.
static inline void sharedPublish(PlcSnapshot& working) {
  working.stampMs = millis();
  g_shareMutex.lock();
  working.seq = g_published.seq + 1;
  g_published = working;                 // struct copy: the whole point
  g_shareMutex.unlock();
}

//  main only. Copies published into out. Returns true if seq advanced
//  since lastSeq, and updates lastSeq -- so main assigns Cloud* once per
//  PLC tick, not once per 100 ms pass.
static inline bool sharedTake(PlcSnapshot& out, uint32_t& lastSeq) {
  g_shareMutex.lock();
  out = g_published;
  g_shareMutex.unlock();
  if (out.seq == lastSeq) return false;
  lastSeq = out.seq;
  return true;
}

static inline void sharedCtrlWrite(const CtrlState& in) {
  g_shareMutex.lock();
  g_ctrl = in;
  g_shareMutex.unlock();
}

static inline void sharedCtrlRead(CtrlState& out) {
  g_shareMutex.lock();
  out = g_ctrl;
  g_shareMutex.unlock();
}

//  main only. Non-blocking: a full queue drops the command and counts it.
//  Main must never wait on the PLC thread.
static inline bool sharedCmdPost(const Cmd& c) {
  Cmd* slot = g_cmdQueue.try_alloc();
  if (slot == nullptr) { g_cmdDropped++; return false; }
  *slot = c;
  g_cmdQueue.put(slot);
  return true;
}

//  PLC thread only. Returns false when the queue is empty.
static inline bool sharedCmdTake(Cmd& out) {
  Cmd* m = g_cmdQueue.try_get();
  if (m == nullptr) return false;
  out = *m;
  g_cmdQueue.free(m);
  return true;
}

// ---------------------------------------------------------------------
//  Thread-ownership asserts. Debug builds only. A violation prints once
//  and spins, which on a bench with the serial console open is the
//  loudest possible failure and on a deployed board is caught by the
//  watchdog feeder giving up on the stuck thread.
// ---------------------------------------------------------------------
static osThreadId_t g_plcThreadId  = nullptr;
static osThreadId_t g_mainThreadId = nullptr;

#if ENABLE_SERIAL_DEBUG
  #define SHARED_ASSERT_ON_PLC()  do { if (g_plcThreadId && osThreadGetId() != g_plcThreadId) { \
      Serial.println("!! OWNERSHIP: PLC-side code ran off the PLC thread"); for(;;){} } } while (0)
  #define SHARED_ASSERT_ON_MAIN() do { if (g_mainThreadId && osThreadGetId() != g_mainThreadId) { \
      Serial.println("!! OWNERSHIP: Cloud* assigned off the main thread"); for(;;){} } } while (0)
#else
  #define SHARED_ASSERT_ON_PLC()  do {} while (0)
  #define SHARED_ASSERT_ON_MAIN() do {} while (0)
#endif

#endif // SHARED_H
