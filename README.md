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
  the board was offline 11:48-12:19 PT while Adam switched its WiFi over serial from the office AP to the
  Starlink router (three restarts from the clear/reconfigure, not faults); the pending OTA applied on
  reconnect. wifiRssi now -26 dBm (Starlink router beside the board; was -78..-82 on the office AP). Soak baseline n=50.
- 2026-09-08 14:28 PT batch 6.1 on IP2 via OTA, fw 95684a6: probe address expiry + re-resolve, fail-open after 60 s,
  WiFi-up-cloud-down 15 min marked reset (waits for waterOwedL==0, 20 min cap), where-codes 16/17, probe counters
  in [HB]. Fixes the 12:34-14:09 outage (probe knocked a stale broker IP forever; update() never ran). n=51 -> 52.
  Soak baseline n=52.
- 2026-09-08 18:06 PT batch 7 on IP2 via OTA, fw e463f5b: flow watch (level drop vs metered litres after each
  discharge, verdict latched into lastError; flowMis in [HB]). Boot n=52 -> 53, lastError ok, plcConnected true,
  stackFree 18144, stallWhere 14 at boot, first post-boot discharge flowBatch 0.57 L. Verified through the
  Arduino Cloud API; the Influx bridge delivered the same rows a few minutes late.
  Soak baseline n=53. Serial to confirm on site: one `[FLOW] pump ..s level ..->.. meter ..mL ok` per discharge.
- 2026-09-09 09:10 PT batch 8 on IP2 via OTA, fw 9a63b1a: cloud offline ladder replaces the flat 15 min reset
  (WiFi re-association at 5/15 min, marked reset at 20 min only when the probe still reaches the broker port,
  60 min regardless; marker carries `p=oks/fails fo=N wr=N`). Boot n=54 -> 55, lastError ok, plcConnected true,
  stackFree 18128, stallWhere 14 at boot, wifiRssi -26. Verified through the Arduino Cloud API. Soak baseline n=55.
  Serial to confirm on site: `[HB] ... wr=0 off=0`; the 03:44-04:02 event of 2026-09-09 (`cloud offline 15min n=54`)
  is what this batch answers.
- 2026-09-09 15:44 PT batch 9 on IP2 via OTA, fw cf66c50, boot n=56 -> 57; first payoff on the spot: plcFailTag =
  `Atmosphere_Temp` (the `read fail x2 (cip=0x0)` the colleague's PLC download had left since 14:08). Soak baseline n=57.
  Batch 9 (evidence) content: No-SYNC guard (attached 6 min without SYNC -> one WiFi re-association);
  ladder reset marker now `offline 60m p=oks/fails fo=N wr=N gw=ok|fail|na` (was `cloud offline ..min ...`);
  plcFailTag names the first unread SENSOR tag first ('all sensors' when the whole sweep is unread; before
  this batch it named status-sweep tags only -- semantic break in the history at this timestamp); `[HB] sync= nsk=`;
  one boot-time gateway ping on serial. `nsk=0` does NOT mean the 2026-09-01 zero-report fault is fixed (see docs/batch9-final-review.md I-1).
- 2026-09-10 08:59 PT batch 10 on IP2 via OTA, fw cd52c03, boot n=61 -> 62, plcConnected true, plcFailTag still
  Atmosphere_Temp (colleague's PLC program). Soak baseline n=62. Batch 10 (flight recorder) content: While the cloud is down: one real DNS lookup a minute (from minute 2)
  and one log row a minute into RAM; persisted to the KVStore before a ladder reset (`epi_reset`) and after a
  self-healed episode >= 5 min (`epi_heal`); read back on serial at boot and at http://192.168.102.107/episode.
  Marker format now `off 20m p=8/0 fo=0 wr=2 gw=1 dns=0`. KVStore writers now share g_kvMutex.
- 2026-09-10 11:52 PT batch 11 phase 1 on IP2 via OTA, fw bae05df, boot n=64: direct-to-InfluxDB transport
  (BearSSL + Amazon Root CA 1) PROVEN -- TLS handshake and HTTP round trip in 2324 ms, heap fordblks
  7000 -> 1928 (in session) -> 6480. Server answered 403: the write token lacks permission on bucket
  Atoco_Opta_Live (a wrong token would be 401, an unknown bucket 404). pushStat = `FAIL code=403 ms=2324 heap 7000>1928>6480 n=0/1`.
  Token lives in secrets.h and is compiled in: fixing it means a new build + OTA. Next: phase 2 (offline ring + replay).
- 2026-09-10 13:38 PT batch 11 phase 1 rebuilt with the corrected write token, fw f975cde (same source, new
  secrets.h), boot n=66: pushStat = `ok code=204 ms=2573 heap 7048>1976>6528 n=1/0`. The board writes to
  InfluxDB Cloud directly. Open item: since batch 10 every OTA costs TWO boots (n=63 and n=65 never published
  a bootReason; the second boot reports `none`) -- serial needed to see the first one.
- 2026-09-10 14:56 PT batch 11 phase 2 on IP2 via OTA, fw 0ad2a68, boot n=68 (n=67 unpublished -- two boots per OTA again).
  REPLAY PROVEN: three-decimal points written by the board landed in arduino_iot at 14:56:40 / 14:57:12 / 14:57:42
  (t1HotTank 47.749, displayWaterVolume 170.891, plcConnected 1) beside the forwarder's live points. Two defects on
  the first live drain: (1) rpBuildBatch encoded past the queue -- two never-written slots (epoch 0) went out, the
  server said 400, and the count underflowed to 65534 (fixed in 0c51c2b); (2) the 0c51c2b OTA at 15:07 stalled in
  Fetch and the board went silent while the buggy replay kept posting beside the download -- review I3 in the flesh
  (push now stands down while an OTA runs, 87b8c33; the stuck OTA was cancelled at 15:38). Earlier the same day:
  13:33 `wd giveup @15 cloud 300s` (n=65) -- the first feeder reset since batch 6; cause not attributed.
  Batch 11 phase 2 (replay) content: While the cloud is down: one 136-byte record every 10 s (28 sensors with a
  read-mask, flowRate/flowBatch/displayWaterVolume, plcConnected) into a 360-record ring; persisted to the KVStore before a
  ladder reset; drained 3 records per POST into arduino_iot once the cloud has been back 2 min; 2xx deletes, 400/413/422
  drops the batch (rej=), auth errors back off 15 min. pushStat becomes `replay q= sent= posts= last= drop= rej=`.
  Static RAM 179 -> 247 KB (47%). Only 32 variables are replayed; valves/heat pump are not.
- 2026-09-10 15:54 PT IP2 came back by the ladder's evidence rung: `off 22m p=11/0 fo=0 wr=2 gw=1 dns=1 n=70` (n=69 never
  published). On 0ad2a68 it drained the 3 outage records in one POST (204). 16:14 PT batch 11 final `87b8c33` on IP2
  via OTA (single boot this time), n=71: queue-bounded batching + no push during an OTA download. Soak baseline n=71.
- 2026-09-10 heap/recursion audit of the sketch's own files (Adam's ask): no new/malloc, no STL containers, no
  recursion (rtos::Mail is a fixed pool). Arduino String appeared on 7 compiled lines; now 5, all at the CloudString
  boundary (lastError, plcFailTag, plcStateText, pushStat) and only when the text actually changes -- the 2 s
  unconditional rebuilds of lastError/plcFailTag are gone; readStringUntil replaced by a fixed buffer. What remains is
  library heap (ArduinoIoTCloud properties/CBOR, MQTT, mbed sockets, KVStore, DNS) watched via heapFree.
