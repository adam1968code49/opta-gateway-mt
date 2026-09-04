# Batch 0 final review — carry-over for batch 1

Whole-branch review of f0abb53..a43c01d, 2026-09-04, after the firmware was
already on IP2 (OTA 12:00 PT). Verdict: both isolation claims hold in the
code (one mutex, four one-copy critical sections, all Mail ops non-blocking);
nothing had to be fixed before the overnight soak. Everything below waits
for batch 1 unless the soak or the serial check says otherwise.

## Fix in batch 1

1. `opta-gateway-mt.ino` `panelLeds()` lives inside the `#if ENABLE_SERIAL_DEBUG`
   heartbeat. A production build with serial off has dead panel LEDs. Move it out.
2. `shared.h` globals are `static` in a header. Correct with one TU; a second
   `.cpp` including it (batch 6 storage) would silently get its own mutex and
   snapshot. Add a non-static, non-inline `void sharedTuGuard() {}` so a second
   TU is a link error, not a runtime mystery. (Closes the ledger's extern/static item.)
3. `plc_thread.h` beats the watchdog once per tick. Batch 0 worst tick ~24-28 s;
   batch 1 adds pollValves/pollPlcState and pushes toward 60 s. Beat after each
   MSP chunk and each DINT read inside `pollSensorsInto`.
4. `cloud_probe.h` re-runs `gethostbyname` on every knock while DNS fails
   (mbed: 5000 ms x 3 attempts), so the "500 ms" probe is a 15-30 s main-thread
   block every 5 s with the internet down. Cache the negative result or throttle DNS separately.
5. `.ino` `plcStalled` uses `s_local.stampMs`, initially 0. If setup() ever exceeds
   6 s, the first loop pass fakes "plc thread stalled". Guard with `s_local.seq != 0`.
6. `plc_thread.h` reconnect branch skips `pollSensorsInto`, so `ok[]` keeps the
   previous tick's truth. Harmless in batch 0; batch 2 water accounting must not
   read `ok[]` as "read this tick" on that path. Clear `ok[]` there or rename the semantics.
7. `EtherNetIP.cpp` has five function-level static buffers: the client is
   single-thread by construction. `SHARED_ASSERT_ON_PLC()` guards only
   `pollSensorsInto`. Put it in the reconnect branch and `drainCommands()` too,
   or better inside `sendRRData()`.
8. `Serial` is written from both threads via `LOG`. With no console attached
   `USBSerial::write` returns 0 immediately (zero risk tonight). With a console
   attached the output interleaves (garbled, not deadlocked). Residual risk: a
   console that stops draining blocks both threads in `AsyncWrite::wait` with no
   timeout, and the feeder resets the board 60 s later, exactly while debugging.
   Give LOG a mutex, or keep the PLC thread off the serial port.
9. `wd_feeder.h` `s_stallMaxMs` never decays: `loopStallMs` is pinned at the boot
   connect (5999) and a later 20 s stall is invisible. Sliding-window max, or a
   cloud-side reset.
10. Nothing checks at compile time that `cloud_side.h`'s 28 `TAKE()` cover slots
    0..27 exactly once. A constexpr bitmask assert would.
11. `tools/build.sh` has no warning gate; the "zero warnings" claim rests on a grep.
    Add `2>&1 | tee` plus `! grep -q "warning:"`.
12. `cloud_side.h` header comment says "seven String properties"; batch 0 builds one
    (`lastError`). Fix the comment.

## What the dashboard sees since 12:00 PT 2026-09-04

- 155 properties registered by the old firmware, 43 by batch 0. The other 112
  stop updating and hold their last value (not zero): all valves/pumps/positions,
  PLC state words, adsorp/desorp timers, all water accounting, all AI, all heat
  pump, the error flags, usbLogStat, pushStat.
- Still live: 28 sensors, plcConnected, loopMs/cloudMs/eipMs, lastError,
  fwVersion, bootReason, stackFree (main only), loopStallMs/stallWhere,
  wifiRssi, heapUsed/heapFree/uptimeS. `otaPending` is registered but never set.
- Control path is fully cut: controlEnabled/systemRun/stop/reset/purge, timers,
  hp setpoints, resets. Buttons on the dashboard still store values in the cloud;
  the board does not consume them.
- **Batch 1 trap:** ArduinoIoTCloud delivers cloud-stored READWRITE values as
  onChange at first sync. A start pressed during batch 0 could reach the PLC the
  instant batch 1 connects. Normalise those cloud values before batch 1 goes on.
- LED_FAULT now means only "PLC disconnected or snapshot stale".
- Rollback: the old firmware has no archived binary. Rollback = rebuild
  `opta-plc-gateway-ip2` with its `tools/build.sh` and OTA, or DFU at the board.

## The one thing to verify on a serial console

How long one PLC tick lasts against a half-dead PLC (port 44818 open, no CIP
reply). Estimate ~24 s, but `EtherNetIP.cpp` `readExact` resets its timer on
every byte, so a peer trickling bytes stretches the tick without bound while
`wdBeatPlc()` fires only at tick start. Point `plcIp` at `nc -l 44818` and
watch `stall=` `@6`. Over 40 s promotes item 3 to "before batch 1".
