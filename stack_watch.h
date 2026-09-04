#ifndef STACK_WATCH_H
#define STACK_WATCH_H

// =====================================================================
//  Hand-kept stack watermark, one instance per thread.
//
//  osThreadGetStackSpace() needs OS_STACK_WATERMARK, which the precompiled
//  libmbed was built without, so the watermark is kept by hand: paint the
//  unused region with a pattern once, then count how much is still intact.
//  Safe on Cortex-M because the thread runs on PSP while interrupts run
//  on MSP -- the region under SP is genuinely dead.
//
//  Made a struct so the PLC thread can have its own. begin() MUST be
//  called from inside the thread it watches, at that thread's shallowest
//  point (end of setup() for main; first line of the thread body for plc).
// =====================================================================

#include "rtx_os.h"

#define STACK_PAINT   0x5A5A5A5AUL
#define STACK_MARGIN  2048          // left unpainted below the live frame

struct StackWatch {
  uint32_t* base  = nullptr;   // lowest address of this thread's stack
  uint32_t  words = 0;         // words painted
  uint32_t  minB  = 0;         // smallest headroom seen, bytes

  void begin() {
    osRtxThread_t* t = osRtxInfo.thread.run.curr;
    if (!t || !t->stack_mem || !t->stack_size) return;
    uint32_t b   = (uint32_t)t->stack_mem;
    uint32_t end = b + (uint32_t)t->stack_size;
    uint32_t here = (uint32_t)&t;              // live SP to within a few words
    //  CONTAINMENT, both bounds. If this frame is not inside the region
    //  the TCB describes, (here - b) is garbage and painting it would
    //  scribble over unrelated memory. Refuse rather than brick.
    if (here <= b + STACK_MARGIN || here > end) return;
    base  = (uint32_t*)b;
    words = (uint32_t)((uint32_t*)(here - STACK_MARGIN) - base);
    for (uint32_t i = 0; i < words; i++) base[i] = STACK_PAINT;
    minB = words * 4;
  }

  void sample() {
    if (!base || !words) return;
    uint32_t intact = 0;
    while (intact < words && base[intact] == STACK_PAINT) intact++;
    uint32_t freeB = intact * 4;
    if (freeB < minB) minB = freeB;
  }

  uint32_t minFree() const { return minB; }
};

#endif // STACK_WATCH_H
