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
  static constexpr size_t LASTERR_CAP   = 48;
  static constexpr size_t FAILTAG_CAP   = 40;
  static constexpr size_t STATETEXT_CAP = 128;

  uint32_t seq;            // monotonic, bumped on every publish
  uint32_t stampMs;        // millis() at publish, so main can age it

  // ---- sensors (batch 0) ----
  float    sensor[N_SENSORS];
  bool     ok[N_SENSORS];
  int32_t  fails;          // sensor slots not read this tick

  // ---- valves / pumps / positions / fault flags / heat-pump St_* (batch 1) ----
  //  Slot order == VALVE_TAGS. BOOL tags arrive as 0.0/1.0; main tests > 0.5.
  float    valve[N_VALVE];
  bool     valveOk[N_VALVE];
  int32_t  valveFails;                 // -> plcReadFails
  char     failTag[FAILTAG_CAP];       // first failing valve-sweep tag, "ok" when none

  // ---- PLC state words (batch 1, every 3rd tick) ----
  uint32_t actionWord;                 // bit N = Action_(N+1)
  uint32_t stateWord;                  // Start,Stop,Reset,Purge,State_1,State_2,Fan1,Fan2,TopA,TopB,BotA,BotB
  char     stateText[STATETEXT_CAP];   // one line, '?' prefix when a tag was unread
  int32_t  stateFails;
  uint32_t stateSeq;                   // +1 each time the state sweep ran; main assigns only on change
  int32_t  adsorpElapsedS;             // Timer_3.ACC / 1000
  int32_t  desorpElapsedT6S;           // Timer_6[3].ACC / 1000
  int32_t  desorpElapsedT11S;          // Timer_11[3].ACC / 1000
  int32_t  adsorpPreMin;               // Timer_3.PRE / 60000, 0 = not read
  int32_t  desorpPreMin;               // Timer_6[3].PRE / 60000, 0 = not read

  // ---- link / diagnostics ----
  bool     plcConnected;
  int32_t  eipMs;          // duration of the last eip.begin() attempt, ms
  uint16_t lastCipStatus;
  char     lastError[LASTERR_CAP];   // "ok", "read fail x3 (cip=0x5)", ...
  uint32_t plcStackFree;   // PLC thread's own stack watermark, bytes
  uint32_t cmdDropped;     // commands lost to a full queue, cumulative

  // ---- water accounting (batch 2), all computed on the PLC thread ----
  float    flowRate;           // L/min from the pulse meter
  float    flowTotal;          // litres since the last reset, gateway-side
  float    flowBatch;          // litres of THIS discharge, held through the idle gap
  float    waterOwedL;         // measured but not yet committed to the PLC total
  float    liveCum;            // PLC base + owed: the live lifetime total  -> displayWaterVolume
  float    liveTrip;           // liveCum - trip mark, rollover-aware        -> tripWaterVolume
  bool     liveTripValid;      // mark was readable and sane this pass
  bool     plcFlowWriteOk;     // rate + volume + base path all succeeded this pass
  bool     hmiWriteOk;         // last hourly HMI write succeeded
  uint32_t hmiWriteCount;      // hourly HMI writes performed
  uint32_t plcTotalRestores;   // lifetime-total restores after a PLC download
  int32_t  desorpPreMinT11;    // Timer_11[3].PRE / 60000, 0 = not read (consistency check vs T6)
  int32_t  flowMismatchCount;  // batch 7: discharges where the level dropped but the meter saw nothing

  // ---- heat pump through the PLC (batch 5, every 3rd tick, offset 1) ----
  //  Slot order == HP_REAL_TAGS / HP_BOOL_TAGS. Words (15..20) are REAL
  //  bit fields, main truncates to int; BOOLs arrive as 0.0/1.0.
  float    hpReal[N_HP_REAL];
  bool     hpRealOk[N_HP_REAL];
  float    hpBool[N_HP_BOOL];
  bool     hpBoolOk[N_HP_BOOL];
  int32_t  hpFails;            // slots unread in the last heat-pump sweep
  int32_t  hpDataAgeS;         // seconds since any HP_In analog last changed
  bool     hpDataStale;        // hpDataAgeS >= HP_STALE_TIMEOUT_MS/1000
  uint32_t hpSeq;              // +1 each heat-pump sweep; main assigns only on change
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

//  cloud thread only (was main before batch 6). Copies published into out. Returns true if seq advanced
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

//  cloud thread only (was main before batch 6). Non-blocking: a full queue drops the command and counts it.
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
static osThreadId_t g_plcThreadId   = nullptr;   // set first thing in plcThreadBody
static osThreadId_t g_cloudThreadId = nullptr;   // set first thing in cloudThreadBody

#if ENABLE_SERIAL_DEBUG
  #define SHARED_ASSERT_ON_PLC()   do { if (g_plcThreadId && osThreadGetId() != g_plcThreadId) { \
      Serial.println("!! OWNERSHIP: PLC-side code ran off the PLC thread"); for(;;){} } } while (0)
  #define SHARED_ASSERT_ON_CLOUD() do { if (g_cloudThreadId && osThreadGetId() != g_cloudThreadId) { \
      Serial.println("!! OWNERSHIP: Cloud* touched off the cloud thread"); for(;;){} } } while (0)
#else
  #define SHARED_ASSERT_ON_PLC()   do {} while (0)
  #define SHARED_ASSERT_ON_CLOUD() do {} while (0)
#endif

// ---------------------------------------------------------------------
//  Panel LEDs. The cloud thread owns the properties the lights depend on;
//  main owns the pins. One byte crosses: bit0 PLC session, bit1 cloud
//  link, bit2 fault. Single writer, single reader, byte-atomic.
// ---------------------------------------------------------------------
#define LED_BIT_PLC    0x01
#define LED_BIT_CLOUD  0x02
#define LED_BIT_FAULT  0x04
static volatile uint8_t g_ledBits = 0;

//  Non-static, non-inline on purpose: every object in this header is
//  `static`, so a second translation unit that included it would silently
//  get its own mutex and its own snapshot. With this symbol here, the
//  second inclusion is a duplicate-definition link error instead.
void sharedTuGuard() {}

#endif // SHARED_H
