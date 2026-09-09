#ifndef CLOUD_LADDER_H
#define CLOUD_LADDER_H

// =====================================================================
//  CLOUD THREAD ONLY. What to do, and when, while WiFi is up and the
//  cloud stays down. Cheapest first, and the expensive rung -- a whole-
//  board reset -- only when the evidence says a reset can help.
//
//  2026-09-09 03:44 the cloud dropped for 18 min with WiFi up; batch 6.1's
//  flat 15 min gate reset the board and it reconnected at once. Nobody can
//  say from that whether the uplink was down (a reset changes nothing) or
//  our own stack was wedged (a reset was the cure), because the marker
//  carried no evidence. This file replaces the flat gate with a ladder
//  and writes the evidence into the marker.
//
//  Why WiFi.disconnect() and not ArduinoCloud.disconnect(): the library's
//  disconnect() clears its private _auto_reconnect and parks the state
//  machine in Disconnected for good; there is no public connect(). Dropping
//  the association instead makes the cloud library fall back to ConnectPhy
//  and redo DNS/NTP/TLS with _auto_reconnect intact. That IS "restart only
//  the cloud task", through the only door the libraries leave open.
//  Threads, heap, the PLC session and the flow-pulse ledger are untouched.
//
//  Who brings WiFi back afterwards -- two roads, both pre-existing:
//  in a WIFI_FORCE_SECURITY build wifiRescue() runs in the same pass, sees
//  status != WL_CONNECTED and does a scan-free WiFi.begin() at once (its
//  lastTry was cleared while WiFi was up, so no 60 s throttle); that is
//  the road that usually wins, and the WiFiConnectionHandler then never
//  even notices the drop. If that begin() fails, the handler's own
//  check() sees WL_DISCONNECTED and runs end() + begin() itself. Either
//  way the association, the DHCP lease and the TLS session are new.
// =====================================================================

#include <Arduino.h>
#include <mbed.h>
#include <math.h>
#include <stdio.h>
#include <WiFi.h>
#include "config.h"
#include "shared.h"
#include "wd_feeder.h"
#include "cloud_probe.h"
#include "cloud_ctrl.h"        // ctrlSyncSeen()/ctrlSyncMs() for the no-SYNC guard

#define CLOUD_LADDER_WIFI_SETTLE_MS     120000UL   // no rung acts until WiFi has been up this long
#define CLOUD_LADDER_REASSOC1_MS        300000UL   // 5 min offline  -> first WiFi.disconnect()
#define CLOUD_LADDER_REASSOC2_MS        900000UL   // 15 min         -> second
#define CLOUD_LADDER_RESET_EVIDENCE_MS  1200000UL  // 20 min         -> reset, only if the knock landed recently
#define CLOUD_LADDER_PROBE_WINDOW_MS    600000UL   // "recently" = a probe success within this
#define CLOUD_LADDER_RESET_CEILING_MS   3600000UL  // 60 min         -> reset regardless (the probe itself may be wrong)
#define CLOUD_LADDER_LEDGER_GRACE_MS    300000UL   // a reset rung waits for waterOwedL == 0 at most this long
#define CLOUD_LADDER_NOSYNC_MS          360000UL   // attached this long with no SYNC -> drop the association once
#define CLOUD_LADDER_GW_PING_MS         1000UL     // gateway ping timeout when writing the marker

static unsigned long s_ldOfflineSince  = 0;   // first pass that saw cloud down; 0 = cloud up
static unsigned long s_ldWifiUpSince   = 0;   // first pass that saw WiFi up;   0 = WiFi down
static unsigned long s_ldResetArmedAt  = 0;   // a reset rung's condition first held here; 0 = not armed
static uint8_t       s_ldReassocDone   = 0;   // re-associations this episode (0..2)
static uint32_t      s_ldReassocTotal  = 0;   // since boot, for [HB]
static uint32_t      s_ldOks0 = 0, s_ldFails0 = 0, s_ldFailOpens0 = 0;   // probe counters when the episode began
static unsigned long s_ldCloudUpSince  = 0;   // first pass that saw connected() true; 0 = down
static bool          s_ldNoSyncKicked  = false;  // one kick per connection
static uint32_t      s_ldNoSyncKicks   = 0;   // since boot, for [HB]
static bool          s_ldSyncWasSeen   = false;  // ctrlSyncSeen() last pass: a true->false edge restarts the 6 min
static bool          s_ldGwPingDone    = false;  // one boot-time gateway ping, so the instrument is proven to speak

//  One ICMP echo to the router. Returns the RTT in ms, negative on failure.
//  Bounded by CLOUD_LADDER_GW_PING_MS; caller sets the where-code.
static int cloudLadderGwPing() {
  return WiFi.ping(WiFi.gatewayIP(), 255, CLOUD_LADDER_GW_PING_MS);
}

inline uint32_t cloudReassocs()    { return s_ldReassocTotal; }
inline uint32_t cloudNoSyncKicks() { return s_ldNoSyncKicks; }
inline uint32_t cloudOfflineMin(unsigned long now) {
  return s_ldOfflineSince ? (uint32_t)((now - s_ldOfflineSince) / 60000UL) : 0;
}

//  Drop the association. wifiRescue() (same pass) or the connection
//  handler (next check()) brings it back -- the same two roads every
//  ordinary WiFi drop already takes; see the header.
static void cloudLadderReassoc(unsigned long offMin) {
  wdWhereCloud(WD_AT_WIFI);
  LOG("[CLOUD] offline "); LOG(offMin); LOGLN(" min with WiFi up: re-associating (WiFi.disconnect)");
  WiFi.disconnect();
  s_ldReassocDone++;
  s_ldReassocTotal++;
}

//  CLOUD THREAD, every pass, after update() has returned. wifiUp/cloudUp
//  are read fresh by the caller after update(), not the pre-update copies.
static void cloudLadderTick(bool wifiUp, bool cloudUp, unsigned long now, float owedL) {
  SHARED_ASSERT_ON_CLOUD();

  if (!wifiUp)                    s_ldWifiUpSince = 0;
  else if (s_ldWifiUpSince == 0)  s_ldWifiUpSince = now;

  if (cloudUp) {
    if (s_ldCloudUpSince == 0) { s_ldCloudUpSince = now; s_ldNoSyncKicked = false; }
    if (s_ldOfflineSince != 0) {
      LOG("[CLOUD] back after "); LOG((now - s_ldOfflineSince) / 60000UL);
      LOG(" min: p="); LOG(cloudProbeOks() - s_ldOks0); LOG("/"); LOG(cloudProbeFails() - s_ldFails0);
      LOG(" fo="); LOG(cloudFailOpens() - s_ldFailOpens0); LOG(" wr="); LOGLN(s_ldReassocDone);
    }
    s_ldOfflineSince = 0; s_ldResetArmedAt = 0; s_ldReassocDone = 0;

    //  Prove the gateway-ping instrument once per boot, on the serial
    //  log, so a `gw=fail` in a marker months from now can be trusted.
    if (!s_ldGwPingDone) {
      s_ldGwPingDone = true;
      wdWhereCloud(WD_AT_WIFI);
      int rtt = cloudLadderGwPing();
      LOG("[CLOUD] gateway "); LOG(WiFi.gatewayIP()); LOG(" ping ");
      if (rtt >= 0) { LOG(rtt); LOGLN(" ms"); } else LOGLN("FAILED (router does not answer ICMP?)");
    }

    //  The library can also drop s_syncSeen while MQTT stays up (a thing
    //  detach/re-attach fires DISCONNECT without a connection loss). That
    //  edge starts a new 6 min, otherwise the guard would fire at once and
    //  log a "connected N min without SYNC" that never happened.
    bool syncNow = ctrlSyncSeen();
    if (s_ldSyncWasSeen && !syncNow) s_ldCloudUpSince = now;
    s_ldSyncWasSeen = syncNow;

    //  Attached but never synced. Properties go out at QoS 0, so this is
    //  the one application-level witness the device has: the cloud's
    //  last-values reply never arrived. The library's own give-up is 30 s
    //  x 10; past that, drop the association once and let it start over.
    if (!syncNow && !s_ldNoSyncKicked && now - s_ldCloudUpSince >= CLOUD_LADDER_NOSYNC_MS) {
      s_ldNoSyncKicked = true;
      s_ldNoSyncKicks++;
      wdWhereCloud(WD_AT_WIFI);
      LOG("[CLOUD] connected "); LOG((now - s_ldCloudUpSince) / 60000UL);
      LOGLN(" min without SYNC: re-associating (WiFi.disconnect)");
      WiFi.disconnect();
    }
    return;
  }
  s_ldCloudUpSince = 0;
  s_ldSyncWasSeen  = false;
  if (s_ldOfflineSince == 0) {                 // episode begins: baseline the evidence counters
    s_ldOfflineSince = now;
    s_ldOks0 = cloudProbeOks(); s_ldFails0 = cloudProbeFails(); s_ldFailOpens0 = cloudFailOpens();
    return;
  }

  unsigned long off = now - s_ldOfflineSince;

  //  Nothing acts on a radio that is down or has only just come up: a
  //  reboot cannot bring an AP back, and a fresh association deserves the
  //  library's own attempt first. ONE exception: the 60 min ceiling stays
  //  reachable once we have taken a re-association ourselves -- if our own
  //  WiFi.disconnect() left the radio down, the settle gate would otherwise
  //  hide every rung forever, with the feeder happily fed and no marker
  //  (review finding I-1). An AP that is simply off, with no re-association
  //  taken, still never resets, as before.
  bool wifiSettled = wifiUp && now - s_ldWifiUpSince >= CLOUD_LADDER_WIFI_SETTLE_MS;
  bool ceiling     = off >= CLOUD_LADDER_RESET_CEILING_MS && (wifiSettled || s_ldReassocDone > 0);
  if (!wifiSettled && !ceiling) return;

  if (wifiSettled) {
    if (s_ldReassocDone == 0 && off >= CLOUD_LADDER_REASSOC1_MS) { cloudLadderReassoc(off / 60000UL); return; }
    if (s_ldReassocDone == 1 && off >= CLOUD_LADDER_REASSOC2_MS) { cloudLadderReassoc(off / 60000UL); return; }
  }

  //  Reset rungs. Evidence rung: the knock reached the broker's port lately,
  //  so the uplink is fine and the fault is on our side -- the one case a
  //  reset is known to cure (04:02). Ceiling rung: no conditions, because
  //  the knock itself could be what is broken.
  unsigned long lastOk = cloudProbeLastOkMs();
  bool upstreamSeen = lastOk != 0 && now - lastOk < CLOUD_LADDER_PROBE_WINDOW_MS;
  bool resetDue = (wifiSettled && off >= CLOUD_LADDER_RESET_EVIDENCE_MS && upstreamSeen) || ceiling;
  if (!resetDue) { s_ldResetArmedAt = 0; return; }
  if (s_ldResetArmedAt == 0) s_ldResetArmedAt = now;
  //  Wait for the water ledger to be flat (an in-flight batch would be
  //  lost for good), but not forever. 10 mL tolerance: float residue, not
  //  water -- one discharge is 500-1100 mL.
  if (fabsf(owedL) >= 0.01f && now - s_ldResetArmedAt < CLOUD_LADDER_LEDGER_GRACE_MS) return;

  //  Is the router itself answering? Distinguishes "the uplink is down"
  //  (gw=ok, p=0) from "the router is wedged" (gw=fail): the second one
  //  is fixed by power-cycling the router, not by anything on this board.
  const char* gw = "na";
  if (WiFi.status() == WL_CONNECTED) {
    wdWhereCloud(WD_AT_WIFI);
    gw = cloudLadderGwPing() >= 0 ? "ok" : "fail";
  }
  char tag[48];                                // bootMarkIntentional's own buffer is 48
  snprintf(tag, sizeof tag, "offline %lum p=%lu/%lu fo=%lu wr=%u gw=%s",   // <= 43 chars
           off / 60000UL,
           (unsigned long)(cloudProbeOks() - s_ldOks0), (unsigned long)(cloudProbeFails() - s_ldFails0),
           (unsigned long)(cloudFailOpens() - s_ldFailOpens0), (unsigned)s_ldReassocDone, gw);
  //  No LOG here: the marker is the record, and a blocked serial port must
  //  not delay the reset.
  bootMarkIntentional(tag);
  delay(50);
  NVIC_SystemReset();
}

#endif // CLOUD_LADDER_H
