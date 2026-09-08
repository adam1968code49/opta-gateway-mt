// =====================================================================
//  boot_reason.h  --  why did the gateway restart?
//
//  Written to settle a specific question: between 2026-08-31 07:34 and
//  2026-09-01 06:47 this board reset 10 times. One was a reflash; the
//  other nine were unexplained. The telemetry says what they were NOT --
//  loopMs was at its 123 ms baseline right up to the last sample, no PLC
//  read failures, no stale heat-pump data, the AB PLC kept sequencing
//  through every gap, and each outage was a uniform ~60 s. That is a
//  clean instantaneous reset, not a hang and not a cabinet power loss.
//  What it does not say is WHICH clean reset, and the three candidates
//  need completely different fixes:
//
//      brownout / power-on   -> the Opta's own 12-24 V rail
//      software reset        -> something called NVIC_SystemReset(),
//                               i.e. the unauthenticated GET /clear on
//                               the RJ45 config page, or serial config
//      watchdog              -> something else entirely
//
//  TWO INDEPENDENT WITNESSES, because neither is trustworthy alone:
//
//  (1) RCC->RSR, the hardware reset-status register. Read FIRST in
//      setup() and cleared, else the flags accumulate across resets and
//      every boot looks like all of them at once.
//
//      Its weakness: anything that runs before us can clear it, and the
//      Opta's bootloader is not ours. If that happens the register reads
//      back as no-flags-set and the decode is worthless -- so the RAW
//      32-bit value is published as hex alongside the decode. A decode
//      is an opinion about bit positions; the raw word is the evidence,
//      and it can be re-read differently later WITHOUT reflashing. If
//      bootReason comes back "none 0x00000000" on every boot, the
//      instrument is deaf and this whole approach is dead -- better to
//      learn that from one field boot than to trust a pretty label.
//
//  (2) A KVStore marker written by every reset path we own, just before
//      it calls NVIC_SystemReset(). Present on boot = we did it, and it
//      says which path. Absent = nobody in this firmware asked. This is
//      what actually answers the GET /clear question, and it does so
//      even if (1) is deaf, because it does not depend on the register
//      at all.
//
//  Include AFTER config.h (needs LOG/LOGLN) and after the KVStore keys.
// =====================================================================
#pragma once
#include <Arduino.h>
#include <Arduino_KVStore.h>
#include <mbed_error.h>

//  Set by a reset path we own, read and DELETED on the next boot. A
//  one-shot: it describes the reset that just happened, so leaving it in
//  place would mislabel every later boot.
#define BOOT_WHY_KEY   "boot_why"

//  Restart counter. Lives in flash beside the marker, so unlike the backup
//  domain it actually survives, and unlike the marker it is never consumed.
//  It exists because until now "how many times did the gateway restart" had
//  to be inferred from a 141-variable republish burst in InfluxDB -- a
//  heuristic that works but that nobody should have to re-derive. One
//  integer answers it directly. At ~9 restarts a day the flash wear is
//  nothing; KVStore wear-levels underneath anyway.
#define BOOT_SEQ_KEY   "boot_seq"

static char s_bootReason[96] = "unknown";
static uint32_t s_bootRsr    = 0;
static uint32_t s_bootSeq    = 0;

// ---------------------------------------------------------------------
//  WITNESS 5, TRIED AND REMOVED: SRAM RETENTION.
//
//  The one bit that would halve this search is whether VDD went away.
//  RCC->RSR holds it and the bootloader clears RSR. RTC->BKP0R holds it
//  and something in the boot path issues a backup-domain reset. The idea
//  here was that plain SRAM needs neither: its contents survive a core
//  reset and are lost when the rail drops, so a magic word placed in the
//  linker's .uninitialized (NOLOAD) region would answer it directly.
//
//  It does not work, and unlike the first two attempts this one was
//  qualified before it was believed. The magic was written at n=23 on
//  2026-09-03 and was gone at n=24 -- an OTA reset, rail up throughout,
//  which is exactly the case it had to survive. Placement was verified in
//  the ELF rather than assumed: s_axiMark sat at 0x24001ac0, inside
//  .uninitialized (0x24001aa8, ALLOC, no contents) and BELOW
//  __bss_start__ at 0x24001ae0, so the startup zeroing loop never touched
//  it. The memory was cleared by something else.
//
//  Which is not mysterious. The Arduino H7 bootloader is itself a full
//  mbed application -- USB mass storage, QSPI, the OTA image copy -- and
//  its own .data and .bss live in AXI SRAM around this address. Every
//  reset runs the bootloader. So AXI SRAM cannot carry anything across a
//  reset on this board, and the failure is not specific to OTA.
//
//  A second marker in D3 SRAM (0x38000000, which the linker script
//  declares as 64 K and then allocates nothing to) was written and then
//  removed unbuilt: Adam's call, and the right one on a gateway that is
//  only reachable over the air. A read of memory that is not present
//  faults, a fault here happens before setup(), and a board that does not
//  boot is a trip to the panel with a DFU cable. The bit is not worth
//  that when the watchdog feeder answers a different half of the same
//  question with no such risk.
//
//  ONE READING SURVIVES AND IT IS WORTH KEEPING
//
//  Two boots reported it, n=23 and n=24, and both said the magic was
//  gone. n=24 was an OTA -- a core reset with power up -- so "gone" there
//  means the witness is blind, not that the rail dropped. That is why
//  n=23 proves nothing either, and why nothing in this file should be
//  read as evidence about the 24 V supply.
// ---------------------------------------------------------------------

inline const char* bootReasonStr() { return s_bootReason; }

//  Call from a reset path we own, immediately before NVIC_SystemReset().
//  Keep the tag short (it shares the 48-byte reason string with the raw
//  register value) and make it name the PATH, not the intent: the point
//  is to tell "/clear was hit" apart from "/save was hit", because those
//  two have very different implications for who is on the network.
// ---------------------------------------------------------------------
//  WITNESS 4 -- a caught crash names itself.
//
//  With RCC->RSR cleared by the bootloader and the backup domain wiped on
//  every boot, "crash" and "the rail dropped" are the two candidates left
//  and nothing so far can separate them. This closes the gap from the other
//  side: if the firmware faults, it says so on the way down, and a restart
//  that carries NO marker at all is then a restart that was neither asked
//  for by our own config paths nor caused by a fault we could catch --
//  which is the power event, by elimination.
//
//  mbed calls this hook on hard faults, asserts and RTOS errors before it
//  finishes dying. Writing flash from that context is not guaranteed to
//  work: interrupts may be off and the flash driver wants RTOS services.
//  So this is qualified on the bench with a deliberate fault (see
//  BOOT_FAULT_TEST) rather than assumed -- if the write hangs, the board
//  hangs instead of restarting, and that is a visible, benign failure on a
//  desk. What is NOT acceptable is shipping it untested and having it turn
//  every field crash into a hang.
//
//  Only the error code and the faulting address are kept. A full mbed error
//  context will not fit in a cloud string, and those two are what identify
//  the site in a map file.
// ---------------------------------------------------------------------
inline void bootMarkIntentional(const char* tag);

//  DISABLED 2026-09-01. Enabling this hook produced a board that did not
//  come back: after flashing, USB stopped enumerating entirely -- no serial
//  port, no DFU device -- so the hang is early and takes the USB stack with
//  it. Note WHEN it hung: not on the deliberate crash, which was never even
//  sent, but during a normal boot. That means mbed calls this hook on the
//  ordinary startup path (a non-fatal error it reports and recovers from),
//  and writing flash from inside it deadlocks against whatever the caller
//  already holds.
//
//  So the shape of witness 4 is wrong, not just its timing. If it is
//  revisited, the hook must NOT touch flash: it should stash the error code
//  in a RAM location and reset, leaving the flash write to the next boot --
//  except that RAM does not survive, which is the whole problem this file
//  keeps running into. The honest alternative is to leave crashes to be
//  identified by elimination (no marker at all) and spend the effort on
//  making that elimination trustworthy instead.
#ifndef BOOT_FAULT_HOOK
#define BOOT_FAULT_HOOK 0
#endif
#if BOOT_FAULT_HOOK
extern "C" void mbed_error_hook(const mbed_error_ctx* ctx) {
  char tag[24];
  snprintf(tag, sizeof(tag), "fault-%04X@%lX",
           (unsigned)(ctx ? (ctx->error_status & 0xFFFF) : 0),
           (unsigned long)(ctx ? ctx->error_address : 0));
  bootMarkIntentional(tag);
}
#endif  // BOOT_FAULT_HOOK

inline void bootMarkIntentional(const char* tag) {
  KVStore kv;
  if (!kv.begin()) { LOGLN("[BOOT] KVStore begin failed, reset will look unexplained"); return; }
  kv.putString(BOOT_WHY_KEY, tag);
  kv.end();                              // flush before the reset lands
  LOG("[BOOT] marked intentional reset: "); LOGLN(tag);
}

// ---------------------------------------------------------------------
//  WITNESS 3 -- REMOVED 2026-09-01. Do not add it back without reading this.
//
//  The idea was sound and the implementation worked: write a magic word to
//  RTC->BKP0R, and since the backup domain survives a system reset but not
//  a loss of VSW power, its presence on the next boot separates "the core
//  reset" from "the rail dropped" -- the exact distinction RCC->RSR could
//  not make on this board.
//
//  It does not work on the Opta, and the bench run says why without any
//  guessing. Instrumented to print both what it read on entry and what it
//  read back right after writing, every boot gave:
//
//      [BOOT] bkp0 entry=0x00000000 echo=0x41574721
//
//  echo = the magic, so the write lands and the PWR_CR1_DBP / RTCAPBEN
//  access is correct. entry = 0 on EVERY boot, including a software reset
//  where the 12-24 V rail provably never moved. Something in the Opta boot
//  path issues a backup-domain reset before setup() runs -- most likely the
//  HAL asserting RCC_BDCR_BDRST while bringing up the LSE.
//
//  So the field would have read pwr=lost on every single restart. Shipping
//  that is worse than shipping nothing: a field that always says "lost"
//  gets read as "the supply is failing", and the next person spends a day
//  on a power problem this code invented.
//
//  If it is ever revisited: the thing to check first is whether the boot
//  path can be made to leave BDRST alone, not whether the register access
//  is right. That part was never the problem.
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
//  Call as the FIRST statement in setup(), before Serial, before the
//  cloud, before anything that could itself reset. Two reasons for the
//  order: RCC->RSR must be read before any library clears it, and the
//  marker must be consumed before a second reset can overwrite it.
// ---------------------------------------------------------------------
inline void bootReasonCapture() {
  s_bootRsr = RCC->RSR;

  //  Decode via the CMSIS masks, never hand-counted bit positions -- the
  //  RSR layout differs across the H7 sub-families and getting it wrong
  //  silently mislabels the answer. Each flag is guarded: a mask this
  //  device's headers do not define simply is not tested, and the raw
  //  word below still carries it.
  const char* why = "none";
#ifdef RCC_RSR_LPWRRSTF
  if (s_bootRsr & RCC_RSR_LPWRRSTF)  why = "illegal-lowpower";
#endif
#ifdef RCC_RSR_WWDG1RSTF
  if (s_bootRsr & RCC_RSR_WWDG1RSTF) why = "window-watchdog";
#endif
#ifdef RCC_RSR_IWDG1RSTF
  if (s_bootRsr & RCC_RSR_IWDG1RSTF) why = "watchdog";
#endif
#ifdef RCC_RSR_SFTRSTF
  if (s_bootRsr & RCC_RSR_SFTRSTF)   why = "software";
#endif
#ifdef RCC_RSR_PINRSTF
  if (s_bootRsr & RCC_RSR_PINRSTF)   why = "nrst-pin";
#endif
#ifdef RCC_RSR_BORRSTF
  if (s_bootRsr & RCC_RSR_BORRSTF)   why = "brownout";
#endif
#ifdef RCC_RSR_PORRSTF
  //  NOT "power-on". On the H7 this single flag is documented as
  //  "POR/PDR or BOR reset flag" -- the silicon merges a cold power-up
  //  with a supply that sagged through the brownout threshold, and no
  //  register bit separates them. Labelling it "power-on" would quietly
  //  answer the question we are asking (is the 12-24 V rail dipping?)
  //  with the one reading that exonerates the rail. So it stays honest
  //  and ambiguous, and the disambiguation comes from context: a reset
  //  at 03:58 with nobody on site is not somebody cycling the breaker.
  if (s_bootRsr & RCC_RSR_PORRSTF)   why = "power-or-brownout";
#endif
  //  Ordered weakest-claim-first so the strongest surviving flag wins: a
  //  power-up sets PIN and POR together on many H7 parts, and reporting
  //  "nrst-pin" for a power cycle would send someone looking for a person
  //  with a paperclip.

  //  Clear, or the next boot inherits these flags on top of its own and
  //  the very first reset poisons every reading after it.
#ifdef RCC_RSR_RMVF
  RCC->RSR |= RCC_RSR_RMVF;
#endif

  //  The marker outranks the register when both are present: a software
  //  reset is only interesting if we do NOT know who asked for it, and
  //  the marker knows. When the marker is absent on a software reset the
  //  string stays "software", which is exactly the alarming case -- it
  //  means NVIC_SystemReset() ran on a path we did not mark.
  char tag[24] = {0};
  KVStore kv;
  if (kv.begin()) {
    if (kv.exists(BOOT_WHY_KEY)) {
      kv.getString(BOOT_WHY_KEY, tag, sizeof(tag));
      kv.remove(BOOT_WHY_KEY);           // one-shot
    }
    //  The counter is read, incremented and written back here rather than
    //  anywhere later, so a board that crashes during the rest of setup()
    //  still counts the boot it did not survive.
    s_bootSeq = (uint32_t)kv.getInt(BOOT_SEQ_KEY, 0) + 1;
    kv.putInt(BOOT_SEQ_KEY, (int)s_bootSeq);
    kv.end();
  }

  //  Three fields, because each one alone has already been shown to
  //  mislead: the marker (or "none" when no reset path we own was taken),
  //  the raw register, and the restart count.
  //
  //  There is deliberately NO field here saying whether the rail dropped.
  //  Three separate witnesses were built to answer that and all three
  //  failed -- see the removed-witness notes above. Adding a fourth that
  //  guesses would be worse than the gap, because a field that reads
  //  "the power failed" on every boot sends the whole investigation to
  //  the 24 V rail and keeps it there.
  snprintf(s_bootReason, sizeof(s_bootReason), "%s rsr=0x%08lX n=%lu",
           tag[0] ? tag : why, (unsigned long)s_bootRsr,
           (unsigned long)s_bootSeq);

  //  NO LOG() here. This function runs before Serial.begin(), so anything
  //  printed from it is written into a dead port and lost -- which is how
  //  the first bench run of this file produced a board that knew why it
  //  had restarted and could not say so. bootReasonLog() below does the
  //  printing, and setup() calls it once Serial is up.
}

//  Print the captured reason. Call AFTER Serial.begin(). Separate from the
//  capture on purpose: the capture has to be first and the printing has to
//  be late, and those two constraints cannot be met by one function.
inline void bootReasonLog() {
  LOG("[BOOT] reset reason: "); LOGLN(s_bootReason);
}
