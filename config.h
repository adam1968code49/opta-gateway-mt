// =====================================================================
//  config.h  -  Site / hardware configuration for the Opta PLC gateway
//                                              *** IP2 -- NOT COMMISSIONED ***
// =====================================================================
//
//  Edit this file to match your installation. No other file should need
//  to change for normal commissioning (except the tag <-> cloud mapping
//  in opta-plc-gateway-ip2.ino and the secrets in arduino_secrets.h).
//
//  Forked from ../opta-plc-gateway/config.h (IP1) on 2026-08-17.
//  Network addresses are CONFIRMED for IP2: PLC 192.168.102.21 (NOT .22 --
//  see the note below), gateway RJ45 192.168.102.107 for instance 1.
//  What is NOT settled is the heat-pump pressure scaling -- see
//  IP2_HP_PRESSURE_SCALING_CONFIRMED in the .ino.
//
#pragma once

// ---------------------------------------------------------------------
//  PLC connection (Allen-Bradley Logix family, EtherNet/IP + CIP)
// ---------------------------------------------------------------------
//  IP2 PLC = 192.168.102.21   (corrected 2026-08-17)
//
//  NOT .22 -- that address holds an EtherNet/IP *adapter*, not the CPU. It
//  registers a session happily (so the gateway reported plc=1 and looked
//  healthy) but answers every symbolic tag read with CIP general status
//  0x08 = Service Not Supported, because adapters do not implement Read Tag
//  (0x4C). If tag reads ever fail wholesale again, print the CIP status
//  before assuming the tag NAMES are wrong: 0x08 means wrong device.
//
//  0x04/0x05 is NOT by itself "wrong tag", which is what this note used to
//  say. On 2026-08-31 15:16 the whole sweep failed with 0x04 and 0x05, and
//  the controller export taken the next day shows every one of the 47 tags
//  this gateway reads present and Read/Write. What actually happened was a
//  session-level failure: all 28 sensor tags and all 36 valve tags failed
//  together, plcConnected dropped, and the next reconnect was clean.
//
//  So the pattern matters more than the code:
//    * the WHOLE sweep fails at once and then recovers  -> the session,
//      not the names (plcTotalRestores will have advanced)
//    * INDIVIDUAL tags fail and keep failing            -> those names
//  A name that is wrong is wrong on every scan. Anything that heals by
//  itself was never a name.
//
//  The gateway's own RJ45 address is OPTA_ETH_IP_* below, on this same
//  192.168.102.x subnet.
#define PLC_IP_OCTET_0      192
#define PLC_IP_OCTET_1      168
#define PLC_IP_OCTET_2      102
#define PLC_IP_OCTET_3      21
#define PLC_ENIP_PORT       44818      // EtherNet/IP explicit messaging (TCP)

// CIP routing.
//  - CompactLogix / ControlLogix with the CPU reachable through the
//    Ethernet port: route to the backplane (port 1), slot 0 -> {0x01,0x00}.
//  - If your controller is in a different slot, change the second byte.
//  - Set USE_UNCONNECTED_SEND to 0 only if your device accepts a bare
//    CIP request (some embedded-port CompactLogix do). Leaving it on is
//    the most broadly compatible choice and matches pycomm3 defaults.
#define USE_UNCONNECTED_SEND   1
#define PLC_ROUTE_PATH_PORT    0x01    // 1 = backplane
#define PLC_ROUTE_PATH_SLOT    0x00    // CPU slot (usually 0)

// ---------------------------------------------------------------------
//  Network interface selection
// ---------------------------------------------------------------------
//  The Opta WiFi has BOTH an RJ45 (Ethernet) and WiFi radio.
//
//  Recommended industrial layout:
//    USE_ETHERNET_FOR_PLC = 1  -> PLC on the wired RJ45 (isolated control
//                                 LAN), Arduino Cloud over WiFi.
//  Simpler single-LAN layout (PLC, Opta and the WiFi router all on the same
//  subnet, e.g. 192.168.1.0/24):
//    USE_ETHERNET_FOR_PLC = 0  -> CIP traffic uses the SAME WiFi link as the
//                                 cloud connection. The RJ45 stays unused;
//                                 the OPTA_ETH_* settings below are ignored.
//
//  *** IMPORTANT: you CANNOT put the RJ45 and the WiFi on the SAME subnet. ***
//  Two interfaces on one subnet (e.g. both 192.168.1.0/24) make routing
//  ambiguous (lwIP picks the first matching netif) and CIP may egress the
//  wrong port. The whole point of using the RJ45 is to put the PLC on a
//  SEPARATE subnet from the WiFi LAN. If everything is genuinely on one
//  subnet, the RJ45 buys nothing -- use USE_ETHERNET_FOR_PLC = 0 instead.
//
//  So when USE_ETHERNET_FOR_PLC = 1, keep:
//    * RJ45 control subnet  : PLC + Opta-eth   (e.g. 192.168.1.0/24)
//    * WiFi LAN subnet      : router + Opta-wifi (a DIFFERENT subnet,
//                             e.g. 192.168.0.0/24) -- these must not overlap.
//
//  This site's case (NEW machine): the machine/control network is
//  192.168.1.0/24 -- PLC 192.168.1.10, GA500 VFD option card
//  192.168.1.62, engineering PC by convention 192.168.1.57. The Opta
//  joins it on the RJ45 at 192.168.1.107 (OPTA_ETH below).
//  (The OLD machine uses 192.168.102.0/24 with its PLC at .107 and the
//  Odot BN-8031 coupler at .70.)
//
//  *** WiFi check: the RJ45 is now on 192.168.1.x, so the WiFi/cloud
//  uplink must NOT be a 192.168.1.x network (see IMPORTANT above).
//  atoco Wi-Fi is 192.168.3.x per site IT, so no conflict. WiFi
//  credentials are hardcoded in arduino_secrets.h (reflash to change). ***
#define USE_ETHERNET_FOR_PLC   1

// Static IP for the Opta's Ethernet port. Must be on the machine network
// (192.168.102.x here) and a FREE address -- verify it is NOT already used by
// the PLC, any VFD, or other device on that network. Keep USE_ETH_DHCP = 0
// unless the machine network actually runs a DHCP server (most don't).
#define USE_ETH_DHCP           0
#define OPTA_ETH_IP_OCTET_0    192
#define OPTA_ETH_IP_OCTET_1    168
#define OPTA_ETH_IP_OCTET_2    102

// ---------------------------------------------------------------------
//  GATEWAY_INSTANCE -- which physical board is this build for?
//
//  Every Opta on the machine network needs its OWN address. Two boards
//  flashed with the same build get the SAME static IP, and a duplicate IP
//  does not fail cleanly: the switch and the PLC thrash their ARP tables
//  between the two MACs, TCP sessions get cut at random, and BOTH boards
//  drop in and out. That looks exactly like the "cloud randomly
//  disconnects" class of fault -- the most expensive kind to diagnose here.
//
//  So the address is selected by instance number, not edited in place.
//  Set this to match the board you are about to flash, and keep
//  OPTA_BOARD_INVENTORY.md (repo root) in step.
//
//    instance 1 -> .107   board 001A00393033511533333437  (has flash backup)
//    instance 2 -> .108   board 0034003F3133511437323532  (no flash backup)
//
//  Only ONE instance may be powered on the network per address. If you flash
//  a board, check the inventory first -- boards are visually identical and
//  the COM port number changes between plug-ins, so the USB SERIAL NUMBER is
//  the only reliable identifier:
//      arduino-cli board list --json   -> properties.serialNumber
// ---------------------------------------------------------------------
//  Default is 1: this directory IS the IP2 gateway, and instance 1 is the
//  board that carries IP2's cloud identity (Thing 89b91e1d..., IP_2_thing).
//  It was briefly set to 2 on 2026-08-17 to give a second board its own
//  address for a two-boards-on-one-switch test; leaving it at 2 would have
//  flashed IP2's gateway with the wrong IP.
#define GATEWAY_INSTANCE       1

#if   GATEWAY_INSTANCE == 1
  #define OPTA_ETH_IP_OCTET_3  107
#elif GATEWAY_INSTANCE == 2
  #define OPTA_ETH_IP_OCTET_3  108
#else
  #error "GATEWAY_INSTANCE must be 1 or 2 -- add a case above for a third board"
#endif

// Subnet mask for the RJ45 control network. The wired port is configured as
// a directly-connected subnet with NO gateway, so the WiFi link keeps the
// default route (internet / Arduino Cloud).
#define OPTA_ETH_MASK_0        255
#define OPTA_ETH_MASK_1        255
#define OPTA_ETH_MASK_2        255
#define OPTA_ETH_MASK_3        0

// ---------------------------------------------------------------------
//  Timing
// ---------------------------------------------------------------------
#define SAMPLE_INTERVAL_MS     2000    // sensor poll period (plclog15.py: 2 s)
#define PLC_IO_TIMEOUT_MS      1500    // per-request read/connect timeout
#define RECONNECT_BACKOFF_MS   5000    // FIRST wait after the session drops
//  Ceiling for the doubling. Sized against the thing it protects: the
//  unbounded TCP connect inside eip.begin() blocks ~17.5 s when no PLC
//  answers (measured 2026-09-02), so a 60 s ceiling leaves ~42 s of every
//  minute for the cloud keepalive, the RJ45 page and the USB flush. Raising
//  this trades PLC-return latency for loop health; lowering it below ~30 s
//  gives most of the loop back to the blocking connect.
#define RECONNECT_BACKOFF_MAX_MS 60000

// ---------------------------------------------------------------------
//  Heat-pump status -> cloud            (IP2 addition, 2026-08-17)
// ---------------------------------------------------------------------
//  IP2's PLC runs a two-stage heat-pump control program (program "HeatPump",
//  routine HeatPump_LD) that IP1 does not have. This block publishes its
//  STATUS to the cloud: measured loop temperature, per-stage demand/satisfied
//  flags, the Y1/Y2 and reversing-valve commands, mode, and the interlocks.
//
//  HP_STATUS_ENABLE 0 removes the whole feature from the build -- tags, cloud
//  variables, poll function, all of it. Nothing else needs changing. Use 0
//  when building for a machine whose PLC has no HeatPump program (IP1), or to
//  get the flash/RAM and the PLC round-trips back.
//
//  Two groups are published, both read-only:
//    * PLC staging status  -- what the PLC's logic has decided (loop temp PV,
//      per-stage call/satisfied, Y1/Y2 and reversing-valve commands, mode,
//      sensor-valid, enable, manual-override, computed activation temps);
//    * unit status via HP_In -- what the machine itself reports through the
//      RTA 460ES gateway at 192.168.102.100 (discharge temp, suction and
//      discharge pressure, evaporator/condenser/suction-line temps,
//      superheat, EEV position, compressor current, loop water temps,
//      operation mode, and the five fault/alarm words).
//  Having both matters when troubleshooting: one says what the PLC ASKED
//  for, the other says what the machine ACTUALLY did.
#define HP_STATUS_ENABLE       1   // batch 5: on (record only)

//  Status changes on the scale of minutes (minimum run/off timers are 5-10
//  min), so polling it as fast as the sensors would just burn round-trips.
//  The 21 REAL points batch into MSP reads, but the 11 BOOLs are individual
//  requests, so this interval directly sets the added PLC load.
//  Must be >= SAMPLE_INTERVAL_MS; it is quantised to the sensor tick.
#define HP_POLL_INTERVAL_MS    6000

// ---------------------------------------------------------------------
//  USB memory-stick data logging  (see usb_logger.h)
// ---------------------------------------------------------------------
//  One CSV row per sensor sweep onto a FAT32 USB-C stick, so the plant
//  keeps a local record independent of the cloud link.
//
//  COSTS THE USB PORT ENTIRELY -- worse than this note used to claim.
//
//  It said the serial port was lost "with a stick plugged in", which reads
//  as: pull the stick, get the console back. That is wrong. Measured on the
//  bench 2026-09-01: the board vanished from USB the moment usbLogBegin()
//  ran, with NO stick inserted -- no serial port and no DFU. The Opta's
//  USB-C is either a device (serial + DFU) or a host (mass storage), never
//  both, and arming the logger switches it to host at every boot.
//
//  So with this at 1 there is no serial console, no serial WiFi config, and
//  no `arduino-cli upload` for as long as the firmware runs. Use the RJ45
//  web page (http://<opta-eth-ip>/) for WiFi.
//
//  THE WAY BACK: double-tap RESET. The bootloader runs before the sketch,
//  so it still enumerates as DFU and can be reflashed with this set to 0.
//  That is the only route back, and it needs physical access to the board.
//
//  ALSO REQUIRES 12-24 VDC on the terminals: the stick occupies USB-C, so
//  the board can no longer be powered over USB.
// ---------------------------------------------------------------------
//  BENCH ONLY -- deliberate fault, to qualify the crash witness
// ---------------------------------------------------------------------
//  Adds a serial command "crash" that faults on purpose, so witness 4 in
//  boot_reason.h (mbed_error_hook -> KVStore marker) can be proven to work
//  before it is relied on. Writing flash from a fault context is not
//  guaranteed, and an untested hook would turn every field crash into a
//  hang -- strictly worse than no hook at all.
//
//  STAYS 0 IN THE FILE. Enable it for a bench build on the command line:
//
//      arduino-cli compile --fqbn arduino:mbed_opta:opta //          --build-property compiler.cpp.extra_flags=-DBOOT_FAULT_TEST=1 .
//
//  so a gateway that reaches a plant cannot carry a crash command no
//  matter who builds it. The #ifndef is what makes that possible: the
//  command line wins, the file stays safe.
#ifndef BOOT_FAULT_TEST
#define BOOT_FAULT_TEST        0
#endif

//  BENCH RESULT 2026-09-01: enabling this proved nothing, because with the
//  flag on there is NO WAY TO OBSERVE the logger. LED_D0 never lit, and
//  that is not evidence of anything: usbLogFlush() raises the LED and then
//  lowers it again inside the failure branch, so a mount that fails leaves
//  it high for microseconds. Success and failure look identical to an eye.
//
//  Serial is gone (see above), so "[USB] mount failed" cannot be read. The
//  only channels left are that one LED and the RJ45 page. BEFORE TRYING
//  AGAIN, give the logger somewhere to report: mount result, rows written
//  and current filename on the web page, which is the console once the
//  USB-C port belongs to the stick.
//
//  (A first reading of this run blamed a permanently blocked loop() -- the
//  RJ45 page did time out while its TCP port accepted connections in 1-20
//  ms. That was wrong: the heartbeat kept printing every 18 s, so loop()
//  was running. What is true is that ArduinoCloud.update() stalls it for
//  many seconds at a time while the configured WiFi is unreachable, which
//  is enough to swallow an HTTP request and to make a 22 s serial capture
//  look dead. Long stalls, not a dead loop.)
//  0 for now, at Adam's request 2026-09-04: with the stick mounted the
//  USB-C port is in host mode and there is no serial, no USB flashing, and
//  no way to watch the board while debugging the restart chase. Set back
//  to 1 when the gateway goes back on the machine for good -- the local
//  record is what survives an outage. Turning this off does not disturb
//  the watchdog feeder or the probes; they never depended on the stick.
#define USB_LOG_ENABLE         0       // 1 = log to a USB stick

//  Flush period. Rows buffer in RAM and go out in one mount/append/unmount
//  batch. Longer = the stick is mounted less often (safer to pull, less
//  wear) but more rows are at risk in RAM. 30 s at a 2 s sweep = 15 rows.
#define USB_LOG_FLUSH_MS       30000

// ---------------------------------------------------------------------
//  DIAGNOSTIC PUBLISH PERIOD
//
//  Clock for heapUsed / heapFree / uptimeS / usbLogStat. 30 s because the
//  cloud can never see closer than ~60-76 s to a restart anyway -- that is
//  the reset + WiFi + MQTT reconnect time, measured on the two restarts of
//  2026-09-02 -- so a faster sampler buys resolution the event itself does
//  not have, and each of these is a trend rather than a spike.
//
//  Independent of USB_LOG_FLUSH_MS on purpose: they happen to match today,
//  and tying them together would make one of them impossible to change.
// ---------------------------------------------------------------------
#define DIAG_PUBLISH_MS        30000

//  RAM ring depth. Must comfortably exceed USB_LOG_FLUSH_MS /
//  SAMPLE_INTERVAL_MS, with headroom for a stick that is out or failing;
//  past that the OLDEST rows are dropped and the gap is noted in the file.
//
//  Raised 40 -> 150 on 2026-09-02. 40 rows at one row per 2 s covered only
//  80 SECONDS, and "pull the stick, copy the file, put it back" is a normal
//  operation that takes minutes: the first bench run lost 108 rows exactly
//  that way, and the file said so. 150 rows is 5 minutes of cover, which is
//  a realistic swap, for ~48 KB of String against 30% RAM already in use.
#define USB_LOG_BUF_ROWS       150

#define USB_LOG_FOLDER         "IP2_LOG"

//  Front-panel status LED 1 (LED_D0, also named LED_RELAY1 in the variant).
//  This gateway drives no relays, so the lamp is free.
//
//  MEANING, as of 2026-09-02:
//      solid  = logging is live; a write may begin at any moment -- do not pull
//      dark   = ejected, or no stick -- safe to remove
//
//  It used to say "lit while mounted, dark = safe to remove". That intent
//  was right and unusable: the mounted window is a mount, an append and an
//  unmount, a few tens of milliseconds once every 30 s, so the lamp never
//  appeared to light at all and told the operator nothing. A steady state
//  can be seen; a 50 ms flash cannot. The guaranteed-safe window now comes
//  from /eject on the web page, not from watching this lamp -- see the
//  EJECTED block in usb_logger.h.
#define USB_LOG_LED            LED_D0

// ---------------------------------------------------------------------
//  FRONT-PANEL LEDS 2-4  --  the three questions asked from the cabinet
// ---------------------------------------------------------------------
//  Chasing faults on 2026-09-01/02 meant asking the same three things over
//  and over -- is the board alive, is the cloud up, is the PLC up -- and
//  each answer cost a USB cable, a serial capture or a web request. Three
//  lamps answer all of it from across the room, and they cost nothing: this
//  gateway drives no relays, so LED_D1..D3 are free.
//
//      LED 2  PLC   solid = CIP session up
//      LED 3  CLOUD solid = Arduino IoT Cloud connected
//      LED 4  FAULT solid = a PLC fault flag is set, or tags are unread
//
//  All steady states on purpose. A blink code would pack in more detail and
//  be unreadable in practice -- that lesson is one lamp to the left, where a
//  50 ms flash was supposed to mean "do not pull the stick".
#define LED_PLC                LED_D1
#define LED_CLOUD              LED_D2
#define LED_FAULT              LED_D3

// ---------------------------------------------------------------------
//  Heat-pump STALE-DATA detection
// ---------------------------------------------------------------------
//  Why this exists (2026-08-17): the heat pump lost power. Its BACnet MS/TP
//  link died, and the RTA 460ES kept serving the PLC its LAST-KNOWN input
//  image instead of zeros or a fault. So HP_In froze on a snapshot, the
//  gateway faithfully published it, and the dashboard showed discharge temp
//  74.79 degC / condenser 48.34 degC -- entirely plausible numbers that were
//  minutes old. Nothing anywhere in the chain flagged it.
//
//  It only came to light because the values were internally contradictory
//  (pressures separated as if the compressor were running, while compressor
//  current read 0). Nobody should have to notice that by eye.
//
//  Detection: the HP_In analog measurements are floats from real sensors.
//  On a live link they never repeat bit-for-bit between polls -- measured
//  over 60 s with the pump running, 8 of 11 changed every single poll. So
//  "not one of them changed for N minutes" is a reliable dead-link signal
//  rather than a heuristic.
//
//  Set to 0 to compile the check out.
#define HP_STALE_DETECT        1
//  How long every watched value must stay bit-identical before the data is
//  called stale. Generous on purpose: the 460ES read loop ran ~21 s when
//  healthy, so this is an order of magnitude above normal, and a false
//  "stale" alarm would train people to ignore it.
#define HP_STALE_TIMEOUT_MS    300000UL   // 5 minutes

// ---------------------------------------------------------------------
//  Heat-pump CONTROL  (cloud -> PLC)
// ---------------------------------------------------------------------
//  Writes go through the same master gate as everything else: nothing
//  reaches the PLC unless controlEnabled is TRUE.
//
//  Only tags that are INPUTS to the PLC's heat-pump logic are exposed.
//  HP_Stage*_Call / _Satisfied / _Activation and HP_Y1A/Y2A/Reverse_Cmd are
//  OUTPUTS the logic rewrites every scan -- writing them from the cloud
//  would be silently overwritten, so they stay read-only.
#define HP_CONTROL_ENABLE      1   // batch 5: on (record only)

//  *** The two switches below are OFF on purpose. Turning either on hands a
//  *** dashboard user something that can damage the compressor. Read the
//  *** reason before flipping one.
//
//  HP_Htg_HighLimit: the PLC comment says it "drops both stages immediately
//  bypassing minimum run time". It is a protection limit; a wrong value from
//  the cloud defeats the minimum-run protection.
#define HP_ALLOW_HIGHLIMIT_WRITE   0
//  HP_Manual_Override: the PLC comment says it bypasses the setpoint logic
//  entirely and commands the stages directly.
#define HP_ALLOW_MANUAL_OVERRIDE   0

//  Setpoint clamps, applied in the gateway BEFORE the write. The dashboard's
//  own min/max is only the first line of defence -- same rule the adsorption
//  and desorption times already follow.
#define HP_SP_MIN_C            5.0f    // no setpoint below this
#define HP_SP_MAX_C            60.0f   // no setpoint above this
#define HP_DELTA_MIN_C         0.5f    // hysteresis floor: below this the
                                       // stage would chatter on sensor noise
#define HP_DELTA_MAX_C         10.0f

//  Stage ordering, enforced in the gateway. The PLC's own comments require:
//    heating: SP_S2 must be BELOW SP_S1
//    cooling: SP_S2 must be ABOVE SP_S1
//  A cloud write that inverts a pair breaks staging, so the gateway refuses
//  it instead of passing it through. This gap keeps the two stages apart.
#define HP_STAGE_GAP_MIN_C     1.0f

// ---------------------------------------------------------------------
//  OTA (over-the-air firmware update)
// ---------------------------------------------------------------------
//  The transport, package signing and bootloader switch are Arduino Cloud's;
//  OTA_ENABLED is already 1 for ARDUINO_OPTA via the library's AIoTC_Config.h.
//  This switch only controls OUR apply-gate and its cloud variable.
//
//  Deferred by design: the update downloads, then onOTARequestCallback()
//  decides when it is applied. For this gateway a reboot costs about 30 s of
//  telemetry and nothing else -- the AB PLC keeps controlling the machine --
//  so the gate is deliberately light. It exists as the seam where the
//  controller firmware's real safety gate will go.
#define OTA_ENABLE             1

// ---------------------------------------------------------------------
//  Safety / control limits  (see SAFETY notes in README.md)
// ---------------------------------------------------------------------
//  Master gate: even when an operator flips the Start switch on the
//  dashboard, nothing is written to the PLC unless controlEnabled is TRUE.
//  Control is currently limited to starting the cycle (Start_Button) --
//  see PLC_CONTROL_TAGS.md for why other tags are not cloud-settable.
#define DEFAULT_CONTROL_ENABLED   false

//  ArduinoIoTCloud library log level: 0=ERROR 1=WARNING 2=INFO 3=DEBUG
//  4=VERBOSE. Raised from 2 to 4 on 2026-09-01 to diagnose a gateway that
//  prints "Connected to Arduino IoT Cloud" with the correct Thing ID, keeps
//  reporting cloud=up, and yet delivers ZERO properties: the cloud's own
//  timeseries shows 0 points for loopMs, which publishes every 5 s on a
//  timer and does not depend on the PLC. At level 2 the library says nothing
//  about property sync, so there is no way to tell a failed publish from one
//  that was never attempted.
//
//  RESULT: raising it to 4 changed nothing. Over 200 s at VERBOSE the
//  library printed exactly two lines -- "Connected to Arduino IoT Cloud"
//  and the Thing ID -- and nothing at all about property sync or publish.
//  The Arduino_DebugUtils macros are runtime-filtered, not compiled out, so
//  the level did take effect; this library simply does not log that path.
//  Back to 2: a knob that was proven not to say anything is not worth the
//  CPU on every cloud tick, and leaving a diagnostic raised "just in case"
//  is how a debugging setting becomes permanent.
#define CLOUD_DEBUG_LEVEL          2

#define ENABLE_SERIAL_DEBUG        1     // 1 = verbose Serial logging

// ---------------------------------------------------------------------
//  Pulse flow sensor on the Opta's OWN input terminal (local I/O)
// ---------------------------------------------------------------------
//  DIGITEN FL-S402B hall flow sensor, NPN (open-collector) output, wired
//  straight to Opta terminal I1. This is the first sensor on this machine
//  that does NOT go through the PLC -- the Opta reads it directly, so it
//  keeps working (and publishing) even when the CIP session is down.
//
//  ELECTRICAL (bench-verified 2026-08-19 -- these are MEASURED facts):
//    * Sensor supply is DC 3.5-12V. *** NEVER feed the red wire 24V. ***
//      Power it from 12V (run the whole bench at 12V, or add a 24->12V buck).
//    * The sensor is NOT a bare open collector: it has an INTERNAL ~10k
//      pull-up to VCC (label doesn't mention it). Unloaded it swings 0-12V,
//      but the Opta input's ~8.5k impedance TO GROUND divides that internal
//      pull-up down to ~5.5V -- just under the ~5.9V VIH threshold, so with
//      no external pull-up I1 never reads HIGH (measured: 5.5V idle, zero
//      counts). An idle reading of 0V is also possible: the impeller can
//      park with the hall output conducting.
//    * Fix: EXTERNAL pull-up, yellow wire to +12V, 1k-2.2k. Fitted 1k;
//      measured 8V idle = comfortably above VIH, NPN sinks ~11mA when low.
//      Bigger than 2.2k drifts back toward the threshold -- don't.
//    * Wiring: red -> +12V, black -> GND (COMMON with the Opta's supply
//      ground), yellow -> I1 with the external pull-up to +12V.
//
//  SIGNAL: label says F(Hz) = 23 * Q(L/min), so full scale 10 L/min = 230 Hz
//  (Opta DI is rated 4.5 kHz -- 20x headroom). Counted on FALLING edges
//  (the NPN's active edge) via interrupt, so no pulses are lost between
//  polls.
//
//  K IS CALIBRATED, NOT ASSUMED (2026-08-21). First real water run read
//  3.3 L for a measured 2.0 L, so the old K was low by that same ratio:
//      K = 23 * 3.3 / 2.0 = 37.95  ->  38
//  That lands on DIGITEN's web figure for this model (F = 38*Q), so the
//  "F = 23*Q" printed on the unit's own LABEL is simply wrong. Two
//  independent sources now agree; the label is the odd one out.
//  Consequence: 38 * 60 = 2280 pulses per litre (was 1380).
//
//  Re-calibrating: zero the total (flowResetTotal), run a known volume,
//  then K_new = K_now * displayed / actual. One 2 L run with a 0.1 L
//  readout resolves K to about +/-1.5 %, so do not chase the last digit
//  from a single small pour.
//
//  FLOW_ENABLE 0 compiles the whole feature out (pin, ISR, cloud vars).
#define FLOW_ENABLE          1   // batch 2: pulse meter on (record only; flow_meter.h does not test it)
#define FLOW_PIN             A0      // Opta terminal I1 (I1..I8 = A0..A7)
#define FLOW_K_HZ_PER_LPM    38.0f   // CALIBRATED 2026-08-21 (2 L run); label's 23 was wrong

//  BATCH TOTALISER -- litres of THIS discharge, not since boot.
//  The machine discharges in bursts: measured on IP2, ~25-50 s of flow at
//  2.4-2.5 L/min delivering ~0.9 L, then ~14 minutes of nothing. So a
//  threshold anywhere between 0 and 2.4 separates flowing from idle, and
//  an idle timeout anywhere between 50 s and 14 min separates one burst
//  from the next. The values below sit in the middle of both windows.
//
//  The counter is zeroed when the NEXT discharge starts, not when the
//  last one ends. Zeroing at the end would leave the HMI showing 0 for
//  the fourteen minutes in between and throw away the number someone
//  actually wants to read. Holding it means the display always answers
//  'this batch so far' while water runs, and 'the batch just finished'
//  the rest of the time.
#define FLOW_BATCH_ON_LPM    0.2f      // above this = water is running
#define FLOW_BATCH_IDLE_MS   30000UL   // no flow this long = batch over

// ---------------------------------------------------------------------
//  FLOW -> PLC  (publish the Opta's own flow measurement INTO the PLC)
//
//  The flow sensor hangs off the Opta, not the PLC, so the PLC has no
//  way to see it unless the gateway writes it. These two tags are the
//  agreed destination:
//      product_flowrate    <- flowRate  (L/min)
//      product_watervolume <- flowTotal (cumulative liters)
//
//  ⚠ THESE TAGS DO NOT EXIST YET (2026-08-19). They are absent from the
//  Studio 5000 export (IP2_20260814_4_Controller_Tags, 439 rows) and the
//  control engineer has confirmed the PLC program will be changed later.
//  So this feature ships DORMANT: every write fails with a CIP path
//  error until the tags appear, and then it starts working on its own --
//  no reflash, no OTA. plcFlowWriteOk on the dashboard is how you see
//  that moment happen.
//
//  BOTH TAGS MUST BE DECLARED **REAL** (agreed with the control engineer,
//  2026-08-19). This is written as a REAL and nothing else: CIP carries the
//  declared type, so if the tag ends up DINT the write is REJECTED, not
//  silently truncated -- plcFlowWriteOk stays false and the serial log
//  names the tag. That is the desired failure mode; a type mismatch that
//  "sort of works" would quietly drop every decimal (2.7 L/min -> 3).
//
//  BACKOFF: a missing tag costs a round-trip per attempt, so while the
//  writes fail the interval stretches to FLOW_PLC_RETRY_MS rather than
//  hammering the CIP session every 5 s forever. First success drops it
//  back to the normal cadence.
//
//  NOT gated by controlEnabled. That gate means "a human is operating
//  remotely" and exists to protect operator writes; this is telemetry
//  flowing INTO the PLC and must keep running unattended. Gating it
//  would mean the PLC only sees flow while somebody is on the dashboard.
#define FLOW_TO_PLC_ENABLE      1   // batch 2: water_plc.h on (record only)
#define TAG_PROD_FLOWRATE       "product_flowrate"     // REAL, L/min
#define TAG_PROD_WATERVOLUME    "product_watervolume"  // REAL, liters
#define TAG_CUMUL_VOL_RESET     "cumulative_watervolume_reset"  // BOOL, HMI reset

// ---------------------------------------------------------------------
//  FLOW WATCH -- did the water the tank lost actually pass the meter?
// ---------------------------------------------------------------------
//  2026-09-08: the hose from the condensate pump to the filter blew off
//  under back-pressure three discharges in a row (15:35, 15:45, 15:55).
//  The pump ran 20-30 s, the tank level fell 15-23 cm exactly as always,
//  and the meter counted zero -- correctly, no water passed it. Nothing
//  put those two facts together; the leak was found by the wet floor.
//  After every discharge flow_watch.h compares the level drop with the
//  metered litres and latches "flow mismatch" into lastError.
//
//  Measured basis (20 discharges, 2026-09-08 08:30-16:30, IP2):
//    normal:  pump 17-34 s, level -14..-24 cm, meter 0.5-1.1 L, peak 1.5-1.9 L/min
//    hose off: pump 20-30 s, level -15..-23 cm, meter 0 L
//    half off (15:24): 0.3 L over -14.5 cm -- deliberately NOT flagged
//  Pulses start 5-10 s after the pump, and the line keeps draining ~30 s
//  after it stops, hence the settle time before judging.
#define FLOW_WATCH_ENABLE         1           // 0 on a machine without this pump/meter layout (IP1)
#define FLOW_WATCH_LEVEL_DROP_CM  8.0f        // normal minimum 14 cm: ~2x margin
#define FLOW_WATCH_MIN_L          0.25f       // normal minimum 0.5 L, fault 0 L; 0.3 L half-fault passes
#define FLOW_WATCH_SETTLE_MS      45000UL     // line drains ~30 s after the pump stops
#define FLOW_WATCH_PUMP_MIN_MS    10000UL     // shorter = a jog, not a discharge: not judged. Also the only
                                              // defence against a level spike (56/72/102 cm seen just before
                                              // the pump) landing on the start tick and faking a drop
#define FLOW_WATCH_PUMP_MAX_MS    180000UL    // normal 17-34 s; longer = something else, give up
#define FLOW_WATCH_LATCH_MS       3600000UL   // lastError holds the verdict 60 min, or until a good discharge
//
//  RUNNING TOTALS -- COMPUTED HERE, STORED IN THE PLC (changed 2026-08-27;
//  the PLC used to compute these and the gateway only mirrored them).
//
//  Why the arithmetic moved to the gateway: the flow sensor is wired to the
//  Opta's I1, so the PLC cannot see a single pulse. Under the old split the
//  PLC had to EDGE-DETECT product_watervolume -- add its value once, at the
//  exact moment the discharge ended. Get that edge wrong and the lifetime
//  total is off by a multiple, not a rounding error: adding every scan
//  inflates it by thousands. The gateway already knows precisely when a
//  discharge ends, because it is the thing counting the pulses, so doing the
//  addition here deletes that synchronisation problem instead of solving it.
//
//  The division of labour is now:
//      PLC   = non-volatile storage   (tags survive power loss; RAM does not)
//      Opta  = arithmetic             (only it can see the flow)
//  Each side does the thing it is uniquely able to do.
//
//    cumulative_watervolume  the lifetime total. READ as the base, then
//                            written back with the finished discharge added.
//    display_watervolume     what the HMI and cloud show: the same lifetime
//                            total but including the discharge in progress.
//
//  display is derived, never independently accumulated:
//      display = cumulative + (liters measured but not yet committed)
//  which makes it continuous across a commit -- the instant cumulative jumps
//  up by 0.9 L the uncommitted figure drops by the same 0.9 L, so the number
//  on the HMI does not move. An independently accumulated display would drift
//  away from cumulative with no way to tell which one was right.
//
//  THE COMMIT ORDER IS THE WHOLE DESIGN. In writeProductFlow():
//      1. read cumulative                     <- the base
//      2. write cumulative = base + journal
//      3. check the write was ACCEPTED
//      4. only then subtract from the journal
//  Swap 3 and 4 and a failed write silently discards water. Skip 1 and cache
//  the base instead, and an HMI-side reset gets overwritten by a stale value.
//
//  AND STEP 1 MUST BE ABLE TO FAIL. If cumulative cannot be read the base is
//  unknown, and the correct action is to write NOTHING -- not to assume 0.
//  Assuming 0 would overwrite the lifetime total with this discharge alone,
//  destroying the number permanently on the first CIP hiccup. That guard is
//  the single most destructive line to get wrong in this file.
//
//  What survives what:
//    cloud/WiFi down     no effect at all -- the cloud is display only; the
//                        HMI reads the PLC and this path never touches WiFi
//    PLC session down    nothing is lost. Pulse counting is a local interrupt
//                        and the journal keeps accumulating across as many
//                        discharges as the outage lasts, committing the lot
//                        on reconnect
//    Opta down           the water that flows is not counted by anyone. No
//                        arrangement of these tags changes that; the sensor
//                        is on the Opta
//    PLC program download tags are reinitialised, so the lifetime total goes
//                        to 0. Unchanged by this move -- see the note in
//                        ops/README.md on recovering it from InfluxDB
//
//  ROLLOVER at WATERVOL_WRAP_L, like a mechanical water meter: subtract the
//  span rather than hard-clearing, so the overshoot is carried instead of
//  discarded and display still equals cumulative exactly. Consumers must
//  difference it as
//      delta = (WRAP - previous) + current      when current < previous
//  the same treatment flow_sensor.h already applies to its uint32 pulse
//  count. 500,000 keeps the value inside float32's exact-enough range:
//  spacing there is 0.03125 L and one discharge is ~0.9 L, so a per-discharge
//  add always lands. Committing on every PLC scan would NOT -- increments
//  below ~0.016 L vanish and the total would quietly stop growing near the
//  top. That is the real reason to commit per discharge rather than
//  continuously, and it is why raising this limit is not free.
// ---------------------------------------------------------------------
//  FAULT FLAGS from the PLC (read-only, added 2026-08-27)
//
//  Three BOOLs the PLC raises. Read up and published as-is: what each one
//  covers is defined by the PLC program, and a description kept here would
//  go stale the first time that logic changes.
//
//  They ride the valve sweep because that is already a batched read on the
//  same 2 s tick, and the CIP_TYPE_BOOL branch in readRealsMSP already
//  decodes discrete points -- three more tags cost no extra round-trip.
//
//  ⚠ A FALSE FAULT FLAG AND AN UNREAD ONE LOOK IDENTICAL. The scatter only
//  assigns when the read succeeded, so a missing or misnamed tag leaves the
//  cloud variable at its initial false -- reporting "no fault" forever, and
//  most convincingly at exactly the moment you would want to trust it. That
//  is why PLC_READ_FAIL_SURFACE exists below: without it these three are
//  indistinguishable from decoration.
#define TAG_PRESS_ERROR         "Press_Error"   // BOOL, pressure fault
#define TAG_TEMP_ERROR          "Temp_Error"    // BOOL, temperature fault
#define TAG_GEN_ERROR           "Gen_Error"     // BOOL, general fault

// ---------------------------------------------------------------------
//  SEQUENCER STATE / OPERATOR PANEL  (PLC -> cloud, added 2026-09-01)
// ---------------------------------------------------------------------
//  27 BOOLs the PLC already owns, published as TWO PACKED WORDS rather
//  than 27 cloud variables.
//
//  Why packed: the Arduino Cloud API does not expose variable creation
//  (every non-GET on things/{id}/properties answers 405), so each new
//  cloud variable is a manual click in the web app. 27 clicks is not the
//  real cost though -- 27 more ON_CHANGE properties is, on a Thing that
//  already carries 142. Two ints carry the same information in two
//  messages. hpLimitsWord, hpBoardFaults, hpSensorFaults and
//  hpPermAlarms1/2 already do exactly this, so the dashboard side already
//  has the habit.
//
//  Read on their OWN slower clock, not in the 2 s valve sweep: these are
//  operator-panel and sequencer-step signals, and 27 extra CIP round-trips
//  every 2 s would spend the session's budget on state that changes at
//  human speed.
//
//  Error_Signal is DELIBERATELY ABSENT. The controller export types it
//  BOOL[1024] -- an array, not a scalar -- so a symbolic read of the bare
//  name does not return "the error". Reading it needs specific elements
//  (Error_Signal[n]) and, before that, someone has to say which n mean
//  what. Adding it blind would publish a number nobody can interpret.
#define PLC_STATE_ENABLE       1   // batch 1: the state sweep is on (record only)
#define PLC_STATE_POLL_MS      6000    // >= SAMPLE_INTERVAL_MS, quantised to it

//  plcActionWord -- bit N = Action_(N+1), so Action_1 is bit 0.
#define TAG_ACTION_1      "Action_1"
#define TAG_ACTION_2      "Action_2"
#define TAG_ACTION_3      "Action_3"
#define TAG_ACTION_4      "Action_4"
#define TAG_ACTION_5      "Action_5"
#define TAG_ACTION_6      "Action_6"
#define TAG_ACTION_7      "Action_7"
#define TAG_ACTION_8      "Action_8"
#define TAG_ACTION_9      "Action_9"
#define TAG_ACTION_10     "Action_10"
#define TAG_ACTION_11     "Action_11"
#define TAG_ACTION_12     "Action_12"
#define TAG_ACTION_13     "Action_13"
#define TAG_ACTION_14     "Action_14"
#define TAG_ACTION_15     "Action_15"

//  plcStateWord -- bit order is FIXED. Appending is fine; reordering
//  silently relabels every historical row in InfluxDB, which nothing in
//  the data would reveal.
//    bit 0  Start_Button           bit 6  Fan1
//    bit 1  Stop_Button            bit 7  Fan2
//    bit 2  Reset_Button           bit 8  Indicator_TopA_Closed
//    bit 3  Purge_Button           bit 9  Indicator_TopB_Closed
//    bit 4  State_1                bit 10 Indicator_BotA_Closed
//    bit 5  State_2                bit 11 Indicator_BotB_Closed
//
//  Start_Button and Stop_Button are READ BACK here although the gateway
//  also writes them from systemRun. That is the point: today a write is
//  assumed to have landed because writeBool() returned true. Reading them
//  turns the assumption into a measurement, the same reason the valve
//  sweep reads back display_watervolume and cumulative_watervolume.
#define TAG_RESET_BUTTON  "Reset_Button"
#define TAG_PURGE_BUTTON  "Purge_Button"
#define TAG_STATE_1       "State_1"
#define TAG_STATE_2       "State_2"
#define TAG_FAN1          "Fan1"
#define TAG_FAN2          "Fan2"
#define TAG_IND_TOPA      "Indicator_TopA_Closed"
#define TAG_IND_TOPB      "Indicator_TopB_Closed"
#define TAG_IND_BOTA      "Indicator_BotA_Closed"
#define TAG_IND_BOTB      "Indicator_BotB_Closed"
//
//  Number of tags in the valve/status sweep that failed to read, published
//  to the cloud. Zero means every tag in that sweep answered.
//
//  It exists because "the value is false" and "nobody asked" are the same
//  picture from a dashboard, and this sweep now carries the fault flags. It
//  also catches what cost real time on 2026-08-27: a tag whose name or type
//  or ExternalAccess is wrong shows up here immediately, instead of being
//  inferred an hour later from a write that failed.
#define PLC_READ_FAIL_SURFACE   1

// ---------------------------------------------------------------------
//  HEAT PUMP STATUS BITS (read-only, added 2026-08-28)
//
//  The St_* family: the heat pump's OWN report of what it is doing, arriving
//  over BACnet (St_BACnetControl in the same family is the giveaway).
//
//  These are not the same thing as the HP_Stage*_Call / hpY*Cmd signals the
//  dashboard already carried. Those are the PLC's intent -- what we asked
//  for. Without the St_* bits a command that never took effect looks
//  identical to one that worked: hpY1Cmd lit, compressor stopped, dashboard
//  green. The bits that explain WHY it did not start -- lockout, phase
//  fault, no water flow -- were exactly the ones missing.
//
//  So the pairing is the point:
//      hpY1Cmd = 1, stStage1 = 0   ->  commanded but not running
//      stLockout / stPhaseFault / stIndoorFlow / stOutdoorFlow  ->  why
//
//  All BOOL, all riding the existing status sweep. That sweep is chunked at
//  EIP_MAX_MSP_TAGS (12) per request, so 37 tags cost 4 round-trips every
//  2 s instead of 3 -- one extra packet for the whole heat pump picture.
#define TAG_ST_STAGE1     "St_Stage1"        // compressor stage 1 running
#define TAG_ST_STAGE2     "St_Stage2"        // compressor stage 2 running
#define TAG_ST_AUXHEAT    "St_AuxHeat"       // auxiliary electric heat
#define TAG_ST_INDOORCIRC "St_IndoorCirc"    // indoor circulator
#define TAG_ST_INDOORFLOW "St_IndoorFlow"    // indoor flow switch
#define TAG_ST_OUTDOORFLOW "St_OutdoorFlow"  // outdoor flow switch
#define TAG_ST_LOCKOUT    "St_Lockout"       // unit locked out
#define TAG_ST_PHASEFAULT "St_PhaseFault"    // phase fault
#define TAG_ST_BACNETCTL  "St_BACnetControl" // under BACnet control

#define TAG_DISPLAY_WATERVOL    "display_watervolume"     // REAL, liters
#define TAG_CUMUL_WATERVOL      "cumulative_watervolume"  // REAL, liters
#define WATERVOL_WRAP_L         500000.0f                 // meter rollover
//
//  SANITY BAND for the base read. A value outside it is treated as unreadable
//  rather than as data: NaN, a negative, or a wild number means the tag is not
//  what we think it is (wrong type decoded, uninitialised memory), and adding
//  to it would write that garbage back as the lifetime total. Refusing to
//  write leaves the tag inspectable instead of overwritten.
#define WATERVOL_MAX_SANE_L     (WATERVOL_WRAP_L * 1.01f)
//
//  PLC PROGRAM DOWNLOAD PROTECTION (added 2026-08-27)
//
//  A full download reinitialises Logix tags, so cumulative_watervolume comes
//  back as 0. Power loss does NOT do this -- the tags are retentive -- so
//  this only happens when an engineer downloads, which they do regularly
//  while the program is still being written.
//
//  Zero passes every sanity check above: it is not NaN, not negative, not
//  absurd. So without this the gateway would accept 0 as the base and commit
//  the next discharge on top of it, and the lifetime total would restart from
//  ~0.9 L within seconds -- long before anyone could type the old value back.
//
//  The tell is DIRECTION. Nothing the gateway does moves this tag backwards:
//  commits only add, and the two operations that do lower it -- the 500,000 L
//  rollover and a reset -- are both performed BY the gateway, which updates
//  its own reference at the same time. So a base below the last known good
//  value, by more than float slop, can only mean the PLC's copy was replaced.
//
//  On detection the gateway writes its own last known good value back. That
//  is the whole point: a download becomes a non-event instead of a data-loss
//  incident needing a manual recovery out of InfluxDB. Water measured during
//  the outage is not lost either -- it is sitting in the journal, and goes in
//  on the next commit on top of the restored base.
//
//  It is never silent. plcTotalRestores counts every occurrence, so "did this
//  happen while nobody was watching" has an answer.
//
//  LIMIT: the reference lives in RAM. If the gateway restarts AND the PLC is
//  downloaded before the gateway reads a good value, there is nothing to
//  restore from -- the first base read after a boot is taken on trust. That
//  is the case to still recover manually (see ops/README.md).
//
//  Set WATERVOL_RESTORE_ENABLE 0 to detect and refuse-to-build-on without
//  writing back, if an operator ever needs to clear the tag by hand from
//  Studio 5000 rather than through the reset button.
#define WATERVOL_RESTORE_ENABLE 1
#define WATERVOL_DROP_EPS       0.5f   // below last-good by more than this
//
//  HMI RESET HANDSHAKE. The button only RAISES the flag (confirmed with
//  Adam 2026-09-05). The gateway zeroes both totals, the trip mark, the trip
//  counter and the history, then writes the flag back false as the
//  acknowledgement.
//
//  So the gateway polls the flag and, if the PLC accepts every zeroing write,
//  clears its own flowTotal/flowBatch journal and writes the flag back to
//  false. Writing it back is the acknowledgement -- the HMI can show the
//  button as taken, and a flag left true means either the gateway never saw
//  it (offline, or the tag is missing) or the PLC rejected the write, never a
//  reset that silently half-happened.
//
//  The control engineer must create it as BOOL with
//  ExternalAccess := Read/Write, like the others.
// ---------------------------------------------------------------------
//  HMI DISPLAY CADENCE -- hourly, deliberately (added 2026-08-27)
//
//  The customer's HMI must NOT track production in real time. It shows two
//  counters, both refreshed once an hour; remote monitoring keeps the live
//  figures. So the display cadence and the storage cadence are now different
//  things and must not be collapsed back together:
//
//    cumulative_watervolume   STORAGE. Written per discharge. Never shown.
//                             Frequent because a power cut loses whatever is
//                             not yet in it -- at most one discharge.
//    display_watervolume      HMI ODOMETER. Written hourly.
//    trip_watervolume         HMI TRIP. Written hourly.
//
//  Making storage hourly to match the display would trade a 0.9 L worst-case
//  loss for a full hour of production. Making the display per-discharge would
//  break the requirement. They are separate on purpose.
//
//  THE TRIP COUNTER IS DERIVED, NOT ACCUMULATED:
//      trip = cumulative - trip_watervolume_mark
//  where the mark is the lifetime total at the moment of the last trip reset.
//  There is exactly one accumulator in the system, so the trip counter cannot
//  drift away from the odometer -- a second accumulator would eventually
//  disagree with the first and nothing could say which was right. It also
//  makes a reset a single atomic write (mark := cumulative) instead of a
//  zeroing that has to be coordinated with an in-flight discharge.
//
//  Subtraction is rollover-aware: a negative difference means cumulative
//  wrapped past WATERVOL_WRAP_L since the mark, so add the span back.
//
//  HOUR BOUNDARIES ARE WALL CLOCK, not uptime -- the customer reads "the past
//  5 hours" against a clock, and an uptime timer would restart at every
//  reboot and drift away from it. NTP arrives with the cloud connection.
//  If the clock is NOT synced (no WiFi) the code falls back to a free-running
//  3600 s timer: an offline machine must still advance its HMI, and a display
//  frozen because the internet is down would be a far worse failure than an
//  hour boundary landing in the wrong place.
#define HMI_UPDATE_SEC          3600UL
//  Minimum real time between two HMI writes. Guards the hour gate against a
//  clock that JUMPS rather than advances: t/3600 changes the moment NTP
//  settles or resyncs, which is not the same event as an hour passing. Below
//  a full hour so a genuine boundary is never missed, high enough that a
//  resync cannot slip a second write in beside it. Costs at most one skipped
//  boundary when a reboot lands in the last ten minutes of an hour -- and the
//  boot tick has just written a fresh value anyway.
#define HMI_MIN_GAP_MS          3000000UL   // 50 min
#define TRIP_HIST_N             5
#define TAG_TRIP_WATERVOL       "trip_watervolume"        // REAL, hourly
#define TAG_TRIP_MARK           "trip_watervolume_mark"   // REAL, set at reset
#define TAG_TRIP_RESET          "trip_watervolume_reset"  // BOOL, HMI request
//  FIVE SEPARATE TAGS, not a REAL[5] array (control engineer's call,
//  2026-08-27 -- their toolchain does not take REAL arrays). _1 is the
//  newest reading and _5 the oldest, so the HMI can lay them out in reading
//  order without reversing anything.
#define TAG_TRIP_HIST1          "trip_history_1"          // REAL, newest
#define TAG_TRIP_HIST2          "trip_history_2"
#define TAG_TRIP_HIST3          "trip_history_3"
#define TAG_TRIP_HIST4          "trip_history_4"
#define TAG_TRIP_HIST5          "trip_history_5"          // REAL, oldest
//
//  HISTORY lives in these PLC tags and nowhere else: read 5, shift, write 5,
//  once an hour. Keeping the ring in Opta RAM would be cheaper but would lose
//  the customer's history on every reboot and OTA, and a five-hour window is
//  exactly the thing a reboot should not disturb.
//
//  The boot tick refreshes the two counters but does NOT push a history
//  entry. Otherwise a reboot would insert an off-schedule reading and the
//  five values would no longer span five hours, quietly contradicting what
//  the button claims to show.
//
//  A trip reset CLEARS the history. The readings are readings OF the trip
//  counter, so after a reset they belong to a trip that no longer exists;
//  showing them beside a counter that restarted at zero would invite exactly
//  the wrong subtraction.
#define FLOW_PLC_WRITE_MS       5000UL   // matches the flowRate cloud cadence
#define FLOW_PLC_RETRY_MS       60000UL  // while the tags are still missing

// ---------------------------------------------------------------------
//  VALVE / PUMP STATE  (read-only mimic data for the P&ID display)
//
//  Tag names verified against the Studio 5000 export
//  .claude_context/IP2_Tags/IP2_20260814_4_Controller_Tags 1.xlsx
//  (IP2 controller, 439 TAG/ALIAS rows). Every tag below was checked to
//  carry "ExternalAccess := Read/Write", so CIP symbolic reads are allowed;
//  a tag set to None would fail the read even with a perfect name.
//
//  WHAT IS READ: the PHYSICAL OUTPUT aliases, not the program's command
//  bits. Expansion:x:O.PtNN.Data is what is actually energised at the
//  terminal, which is what an operator watching a mimic diagram needs.
//  (The program bits S1/S6/V1A/ScrollPump exist too, but they are the
//  sequencer's intent -- they can differ from reality if an output is
//  forced, a fuse is blown, or a module has faulted.)
//
//  DISCRETE (BOOL, on the DO16 modules) vs ANALOG (REAL positioners on
//  Local:7:O channels, driven with MOVE 0/45/50 by the sequence). The
//  proportional ones publish a position, not an on/off state.
//
//  These reads are BOOL, and batched MSP decoding of BOOL only works
//  because EtherNetIP.cpp gained a CIP_TYPE_BOOL branch -- without it
//  16 valves would need 16 separate round-trips per poll instead of 2.
#define VALVE_ENABLE         1   // batch 1: the valve sweep is on (record only; plc_thread.h does not test it)

//  Discrete pneumatic valves (Expansion:3:O)
#define TAG_V_S1      "Air_S1"
#define TAG_V_S5      "Air_S5"
#define TAG_V_S6      "Air_S6"          // top chamber -> turbo
#define TAG_V_S7      "Air_S7"          // bottom chamber -> turbo
#define TAG_V_S10     "Air_S10"
//  Discrete water valves (Expansion:3:O)
#define TAG_V_V1A     "Water_V1A"       // top chamber inlet
#define TAG_V_V1B     "Water_V1B"       // bottom chamber inlet
#define TAG_V_V2A1    "Water_V2A_1"     // chamber outlet -> hot hx
#define TAG_V_V2A2    "Water_V2A_2"
#define TAG_V_V2B     "Water_V2B"
//  Pumps (Expansion:2:O, except the two program BOOLs)
#define TAG_P_SCROLL  "Scroll_Pump"     // Edwards scroll (roughing) pump
#define TAG_P_COND    "Cond_Pump"       // Lefoo condensate pump
#define TAG_P_HOTW    "Hot_Water_Pump"  // Grundfos
#define TAG_P_COLDW   "Cold_Water_Pump" // AMT
//  Booster_Pump REMOVED 2026-08-28. The tag exists in the PLC and reads
//  cleanly, but the program never writes it: seven days of samples, constant
//  0, never a single transition, while BoosterVFD_Run tracked Scroll_Pump at
//  ~54% on. So the dashboard was showing "vacuum booster off" every second of
//  every day -- not a status, a decoration that reads exactly like a stopped
//  pump and would eventually be believed. There is one Roots booster
//  (ETR-WEH1200) and BoosterVFD_Run is its real state.
#define TAG_P_BVFDRUN "BoosterVFD_Run"  // booster VFD run command
//  Proportional valve position commands (Local:7:O analog channels)
#define TAG_POS_S2    "Air_S2_Output"
#define TAG_POS_S3    "Air_S3_Output"
#define TAG_POS_S4    "Air_S4_Output"   // collection chamber vent
#define TAG_POS_S8    "Air_S8_Output"
#define TAG_POS_S9    "Air_S9_Output"
#define TAG_POS_V10   "Air_V10_Output"  // top chamber vent
#define TAG_POS_V11   "Air_V11_Output"  // bottom chamber vent

// ---------------------------------------------------------------------
//  AUTO ADSORPTION TIME -- ambient-RH closed loop (cloud switch gated)
//
//  When autoAdsorpEnable is ON the gateway sets the adsorption time from
//  the ambient relative humidity and writes it to the PLC by itself:
//      RH <  40 %  ->  20 min          RH >= 40 %  ->  10 min
//
//  Dry air carries less water, so the bed needs LONGER to pick up the same
//  load; in humid air it saturates sooner and a shorter pass is enough.
//  (Corrected 2026-08-25 -- this was the other way round, which followed
//  the request as first worded but ran against the process.)
//
//  RH SOURCE IS atmRhDs (Atmosphere_Humidity_02). The nominal ambient
//  tag (atmRh / Atmosphere_Humidity) reads a constant 120 % because its
//  4-20 mA input is unwired on IP2 -- see the scaling note above.
//
//  Anti-chatter, per operator requirement ("no flapping near the
//  threshold, hold at least one round"):
//    * hysteresis band: switch up at >= 40+1.5 %, down at < 40-1.5 %;
//      inside the band the current setting is kept
//    * dwell: a change is applied only after the new condition has held
//      continuously for ONE FULL adsorption round at the current setting
//      (first write after enabling waits only AUTO_ADSORP_FIRST_CONFIRM_MS)
//
//  Deliberately NOT gated by controlEnabled: that gate marks "a human is
//  operating remotely" and postpones OTA. An always-on automation would
//  block OTA forever. The auto loop has its own cloud switch instead.
#define AUTO_ADSORP_ENABLE          0   // batch 2 -- off in batch 0
#define AUTO_ADSORP_RH_PCT          40.0f   // decision threshold, %RH (atmRhDs)
#define AUTO_ADSORP_RH_HYST         1.5f    // +/- band around the threshold
#define AUTO_ADSORP_WET_MIN         10      // humid -> saturates sooner, shorter pass
#define AUTO_ADSORP_DRY_MIN         20      // dry   -> less water per pass, run longer
#define AUTO_ADSORP_FIRST_CONFIRM_MS 60000UL // first decision after enable

// ---------------------------------------------------------------------
//  Water production from tankLevel (upright cylinder; level = height in cm)
// ---------------------------------------------------------------------
//  liters = area(cm^2) * height(cm) / 1000 ; area = pi*(D/2)^2.
//  We accumulate ONLY positive height changes (fills), ignoring the sharp
//  drops when the tank drains -- so drains never count as negative yield.
#define TANK_DIAMETER_CM   15.0f   // inner diameter (MEASURE & update)
#define WATER_EMA_ALPHA    0.2f    // level smoothing per poll (kills jitter)
#define WATER_TICK_SEC     60      // settle every N s, then count the rise
#define WATER_DEADBAND_CM  0.2f    // min rise per tick to count (noise floor)
#define WATER_RATE_TICKS   10      // rate window = TICKS*TICK_SEC (= 10 min)

// ---------------------------------------------------------------------
//  Wi-Fi reconnect backoff
// ---------------------------------------------------------------------
//  The Arduino_ConnectionHandler default is 500 ms, which spins hard enough
//  to starve the rest of loop() when the credentials are wrong -- on IP2 it
//  locked out both the USB serial rescue commands and the DFU handshake.
//  5 s is invisible on a healthy network (the first connect succeeds) and
//  leaves the board responsive when the network is the thing that is broken.
#define WIFI_RETRY_MS          5000

//  SCAN-FREE RECONNECT  (added 2026-08-27)
//
//  WiFiConnectionHandler calls the two-argument WiFi.begin(ssid, pwd), whose
//  `security` parameter defaults to ENC_TYPE_UNKNOWN. In the mbed core that
//  default takes a scan-gated path:
//
//      if (security == ENC_TYPE_UNKNOWN) {
//        scanNetworks();
//        if (isVisible(ssid)) { _security = <from the scan>; }
//        else { _currentNetworkStatus = WL_CONNECT_FAILED; return; }
//      }
//
//  So one scan that misses the beacon returns WL_CONNECT_FAILED *without
//  attempting to associate at all* -- the same status a wrong password gives,
//  and the library then prints "Connection to \"<ssid>\" failed", which reads
//  like an auth failure. It is not: nothing was tried.
//
//  Measured on IP2, 2026-08-27: dozens of consecutive failures against an AP
//  that a phone standing next to the machine held without trouble, with the
//  occasional success -- the signature of an intermittently-seen beacon, not
//  of bad credentials.
//
//  Passing the security type explicitly skips the scan and hands the work to
//  the driver's own connect(), which gets WiFi's _timeout (7 s) to complete.
//  One marginal beacon then costs a retry instead of an instant refusal.
//
//  ENC_TYPE_CCMP maps to NSAPI_SECURITY_WPA_WPA2, so it accepts either. Note
//  it does NOT cover WPA3: enum2sec() sends ENC_TYPE_WPA3 to
//  NSAPI_SECURITY_UNKNOWN. On a WPA3-only AP the scan path is actually the
//  better one, because it copies the security mode straight out of the scan
//  result -- which is why this runs as a RESCUE alongside the handler rather
//  than replacing it. Whichever path gets there first wins.
//
//  Not a substitute for fixing the radio problem. The Opta is 2.4 GHz only
//  and lives in a metal cabinet; a phone on 5 GHz outside the door proves
//  nothing about what the module can hear.
#define WIFI_FORCE_SECURITY    1
#define WIFI_FORCE_MS          60000UL   // try at most this often while down
