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
- 2026-09-10 16:35 PT batch 11.1 on IP2 via OTA, fw 9059e39, n=73 (single boot). At boot the one outage record was
  pushed BEFORE the Arduino Cloud connected (probe said the path was real): `replay q=0 sent=1 last=204` in the very
  first publish. Soak baseline n=73. Batch 11.1 content: replay drains whenever the internet path is real (MQTT up, OR the probe's knock landed
  within 2 probe periods while the Arduino Cloud is down), so a broker-only outage is filled live instead of after
  the broker returns. Upstream-down episodes still queue and drain later.
- Batch 11.2 -- pending OTA: capture only after connected() has been false 5 s (41 of 49 overnight "outages" were one-pass
  flickers); live drain only once an outage is 60 s old; epoch sanity [1.6e9, 2.0e9] and <= 1 day step at capture and
  send (16 records / 512 points landed in the year 2102 overnight -- undeletable on Cloud Serverless, harmless to
  time-ranged dashboards; ops queries must bound time <= now()).

- 2026-09-11 19:10 PT batch 12 on IP2 via OTA, fw 5bee9cb: three momentary READWRITE buttons
  clearPressError/clearTempError/clearGenError -> PLC writeBool(<fault tag>, false) behind the same gates as
  Reset Button, read-back verdict in lastError after 4 s (cleared -> confirmed / re-asserted by PLC / unverified),
  text latched 60 s in the ok gaps. Boot counter n=79 -> 80 (one boot this OTA). Gate path proven with the master
  switch off: API press -> lastError "control disabled", button back to false, PLC untouched. Dashboard
  "IP2 AWG v8" (ee836a7b) built from the v7 CLI template with the buttons under the fault Status widgets (REST
  PUT drops pages -> mobile breaks, so v7 was not edited). genError was TRUE on the machine at the time; the
  confirmed/re-asserted branch is Adam's to exercise. See docs/batch12-final-review.md.
- 2026-09-11 21:38 PT batch 13 on IP2 via OTA, fw 2bb5f73: manual drain switches manS4Open (Air_S4_Output REAL
  100/0) and manCondPump (Cond_Pump) behind the machine-control gates plus a sequencer-idle gate; pump interlocked
  on S4 read back open, auto-stop at collector level < 18 or 180 s; 4 s read-back verdicts; switches mirror the
  PLC's real outputs and only a gateway-switched output may be switched off (review C1). Boot counter n=80 -> 81.
  At boot: S4 0, pump off, sequencer idle, tankLevel 78.5 after three cycles aborted before their discharge step.
  Dashboard "IP2 AWG v9" (9d4dc360). The drain itself is Adam's to run. See docs/batch13-final-review.md.
  21:54 PT result: S4 opened and HELD (posS4 100); the pump write was overwritten by the PLC within one scan
  ("Cond_Pump re-asserted by PLC") -- Cond_Pump is program-driven every scan, a PLC-side manual bit is needed.
- 2026-09-11 22:25 PT batch 13.1 on IP2 via OTA, fw ef7e10d: manS5Open (Air_S5, tank -> pump valve); pump interlock
  needs S4 and S5 open; manual pump stops at once if S4 or S5 is read closed by anyone (review Critical: no
  dead-heading against a closed suction line). Dashboard "IP2 AWG v10" (da197c23), includes Adam's hand-added
  Purge_button. See docs/batch13-final-review.md.

- 2026-09-18 batch 17 built (fw 10ddbf4), OTA queued -- IP2 powered down for hardware work at the time: the board becomes
  IP2's ONLY InfluxDB writer (AWH_Bridge forwarding for IP_2_thing switched off by Adam at 09:33 PT). 10 s live feed from the
  batch-11 ring (now capturing unconditionally) + 120 s slow-analog ring + on-change discretes with a 10 min re-arm heartbeat +
  strings via value_str; one body per pass, steady state capped at one POST per 10 s, full-speed backlog drain, seconds
  precision, same tags so dashboards are untouched. Static RAM +10.2 KB (259048). See docs/batch17-final-review.md.
- 2026-09-18 14:38 PT batch 17 applied on IP2 (fw 10ddbf4, n=91 -> 93) the moment the board came back from a ~5 h
  power-down for hardware work (it had touched the cloud once at 14:06 and dropped). Accepted within 5 min: 1210 rows /
  142 variables in InfluxDB, t1HotTank every 10 s with ms=0, layer 2 every 120 s, layer 3 baseline + changes, strings in
  value_str, pushStat `live q=0/0/0/0 ... posts=22/0 last=204`, cloudMs 2 ms. heapFree read 8168 at 330 s.
- 2026-09-18 15:07 PT batch 17.1 on IP2 via OTA, fw 05b2529, n=93 -> 94: layer-2/3/4 buffers halved (slow ring 15 =
  30 min, change queue 128, strings 8; the 1 h layer-1 ring unchanged), static RAM 259048 -> 254568. heapFree 12432 at
  271 s, feed unchanged (10 s, ms=0, posts 16/0). CORRECTION: the layer-2 history shows 10ddbf4 at 16.1-16.4 KB free
  after 25 min -- the 8168 that motivated the trim was a boot transient. Judge heap on >= 20 min of steady state, not at
  boot. Follow-ups noted in docs/batch17-final-review.md (string queue during the settle window; first zero diag sample).
- 2026-09-18 evening: REBOOT STORM on 05b2529 once the Starlink uplink started flapping -- n=95 (unlogged), n=96 `off 22m
  p=4/22 fo=3` (ladder), n=97 `wd giveup @4 cloud 301s`, n=98 `off 22m p=23/12`. Root cause of the wd giveup: BearSSL's
  handshake loop has no timeout and relies on connected(), which on mbed reflects the WiFi interface, not the TCP peer;
  a half-open socket spins the cloud thread past its 300 s budget. Batch 11 hit that path rarely, batch 17 every 10 s.
  Adam's call: roll the live feed back (influx_feed.h removed; influx_replay.h / cloud_thread.h back to the batch-15
  behaviour), KEEP the fix -- DeadlineClient in influx_push.h bounds every POST to 20 s (code -4) -- and stop reading
  WiFi.RSSI() while not associated (94 s stall @16 seen the same evening). The bridge must forward IP_2_thing again.
  Batch 17's design and plant results stay in docs/batch17-final-review.md for when the feed comes back.

## CI

`.github/workflows/build.yml` compiles every push to `main` (and every PR) with `arduino-cli 1.4.1`,
core `arduino:mbed_opta@4.6.0` and the libraries pinned to the desk build of 2026-09-11, through the same
`tools/build.sh` warning gate. It builds with the `.example` credential files, so the artifact it keeps is
compile-proof only -- never OTA it. When a library is upgraded on the desk, bump the version here and the cache key.
