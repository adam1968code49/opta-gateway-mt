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
- 2026-09-05 04:57 PT batch 2 on IP2 via OTA, fw 67d737e: 105 properties, flow meter and the whole
  PLC water path on the PLC thread. plcFlowWriteOk=True (all eight product_*/trip_* tags answer),
  boot tick refreshed the HMI (hmiWriteCount=1, ok). cumulative=display=8.04 L; trip=8.04 L because
  trip_watervolume_mark is 0 until the first HMI trip reset. waterOwedL=0, plcTotalRestores=0.
  Boot counter n=39 -> 40 (OTA). Soak baseline for batch 2: n=40.

- 2026-09-05 07:43 PT batch 5 on IP2 via OTA, fw bf7f74b: 149 properties; heat-pump sweep live
  (hpHtgSp1/Sp2 49/41, hpClgSp1/Sp2 6/10 consistent with staging rules -> REAL table aligned;
  hpDataStale false, HP_In analogs refreshing), plcReadFails 0. Boot counter n=41 -> 42.
  NOTE: batch 2 (n=40 at 04:57) had restarted once before this OTA (n=41 seen at 07:42);
  cause not yet attributed. Soak baseline for batch 5: n=42.

- 2026-09-08 12:19 PT batch 6 on IP2 via OTA, fw f22d58e: cloud thread live (stackFree=18136 of 24576 at boot,
  cloudMs/loopMs 0), three-budget feeder (main/plc 60 s, cloud 300 s) with give-up marker + self-reset,
  stallWhere 14 at boot (WiFi association phase), plcReadFails 0, hpDataStale false. Boot counter n=47 -> 50:
  the board was offline 11:48-12:19 PT on batch 5 (cause unknown, three restarts while unreachable); the
  pending OTA applied on reconnect. wifiRssi now -26 dBm (was -78..-82 for three days). Soak baseline n=50.
