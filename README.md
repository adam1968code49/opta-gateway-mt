# opta-gateway-mt

The IP2 Opta gateway on two threads: a PLC thread that owns the EtherNet/IP
client and a main thread that owns every Arduino Cloud property, sharing one
mutex-guarded snapshot and one command queue.

Design and batch plan:
`../docs/superpowers/specs/2026-09-04-opta-gateway-mt-design.md`

Build with `tools/build.sh` only. It generates `version.h` from `git describe`;
the Arduino IDE skips that step and produces a firmware with no version, which
OTA cannot tolerate.

Batch 0 (this repository's first milestone): PLC thread reads the 28 sensor
tags into a snapshot; main publishes them. Nothing else is migrated yet.

## Batch 0 verification log

- 2026-09-04 12:00 PT first boot on IP2 via OTA, fw a43c01d: both threads up, 28 sensors
  publishing every 2 s, main stackFree=26256, PLC session in 3 ms, boot-connect stall
  5999 ms @2 (cloud, expected). plcStk not recorded: board not on a serial console.
  Boot counter n=35 -> 36 (the OTA reboot). Soak baseline n=36.
- 2026-09-05 02:19 PT batch 1 on IP2 via OTA, fw 8120373: 94 properties, valve/state sweeps live
  (plcReadFails=0, plcFailTag=ok, plcStateWord=0xF04), controls quiet-gated and all False.
  Boot counter n=38 -> 39 (OTA). Old firmware had restarted once overnight (n 37->38, ~2.4 h uptime).
  Soak baseline for batch 1: n=39. Observed: pressError=True on the PLC; desorp preset reads 9 min.
