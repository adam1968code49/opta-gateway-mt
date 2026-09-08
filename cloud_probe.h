#ifndef CLOUD_PROBE_H
#define CLOUD_PROBE_H

// =====================================================================
//  KNOCK ON THE BROKER BEFORE ArduinoCloud.update() TRIES TO RECONNECT.
//
//  THE PROBLEM IT SOLVES
//
//  Six restarts overnight 2026-09-03/04 (n=29..34), every 2-4 hours, and
//  every one had stallWhere=2: the loop was stuck in ArduinoCloud.update()
//  when it died. Inside update(), the reconnect path ends in
//  _mqttClient.connect(), a TLS connect on a MbedClient socket -- the
//  same socket that is only made non-blocking AFTER connect returns, the
//  same disease as eip.begin(). Against a broker that does not answer it
//  blocks for the full TCP retry, past the feeder's 60 s give-up, and
//  the watchdog resets the board. The PLC probe cannot see this path; it
//  is the other half.
//
//  It is NOT only weak WiFi. Two of the six restarts had steady -70 dBm
//  beforehand. The hop that drops is sometimes the internet side -- the
//  path to iot.arduino.cc -- with the local link fine. That matters
//  because it means a strong Starlink signal does not make this go away:
//  a satellite hand-off drops the same hop. So the probe tests the thing
//  that actually blocks -- can the broker's port be reached -- and not the
//  RSSI, which is a proxy that was wrong a third of the time.
//
//  THE GATE IS NARROW ON PURPOSE
//
//  update() is not only the reconnect. When the cloud IS connected it
//  publishes properties, services MQTT keepalive, runs OTA. Skipping it
//  then would silence the gateway. And when WiFi itself is DOWN, update()
//  is the only thing that re-associates the radio; skipping it then wedges
//  the board offline for good. So the probe gates exactly one state:
//
//      WiFi associated  AND  cloud not connected
//
//  which is the state whose next update() is the unbounded TLS connect.
//  In every other state update() runs unconditionally, as before.
//
//  RATE-LIMITED. While disconnected the loop spins at ~100 ms; a TCP knock
//  on iot.arduino.cc ten times a second is abuse. One knock per
//  CLOUD_PROBE_PERIOD_MS; between knocks the disconnected update() is
//  skipped, which costs nothing -- with the broker unreachable it could
//  only have blocked.
//
//  DNS. The broker is a hostname. gethostbyname() is bounded by mbed's
//  DNS config (5 s wait x 3 attempts worst case) and iot.arduino.cc sits
//  in mbed's own DNS cache after the library's first connect, so it is
//  normally instant. The result is cached here too, so DNS runs once, not
//  per knock. A DNS failure is treated as "not reachable" -- fail CLOSED.
//  DNS down almost always means the internet is down, which means the
//  broker is unreachable, which means entering update() would only stall.
//  Failing open there would defeat the whole gate at the one moment it is
//  needed.
// =====================================================================

#include <WiFi.h>
#include "netsocket/TCPSocket.h"
#include "netsocket/SocketAddress.h"

#ifndef CLOUD_PROBE_HOST
#define CLOUD_PROBE_HOST       "iot.arduino.cc"
#endif
//  8885 = DEFAULT_BROKER_PORT_SECURE_AUTH: the port ArduinoIoTCloudTCP
//  picks for a board with a secure element, which the Opta has
//  (ArduinoIoTCloudTCP.cpp line 122 resolves PORT_AUTO to it).
#ifndef CLOUD_PROBE_PORT
#define CLOUD_PROBE_PORT       8885
#endif
#ifndef CLOUD_PROBE_TIMEOUT_MS
#define CLOUD_PROBE_TIMEOUT_MS 500
#endif
#ifndef CLOUD_PROBE_PERIOD_MS
#define CLOUD_PROBE_PERIOD_MS  5000
#endif
#define CLOUD_DNS_NEG_CACHE_MS 60000   // after a failed lookup, do not ask again for this long
//  The broker name is a load balancer whose addresses rotate. An address
//  cached forever turned into a knock that could never succeed once its
//  node went away (2026-09-08: 90 minutes offline with WiFi up). So the
//  address expires, a run of refusals forces a fresh lookup, and after a
//  minute of refusals update() is let through anyway so the library can
//  resolve and connect by itself -- on its own thread now, a long connect
//  costs nobody else anything.
#define CLOUD_DNS_TTL_MS           600000   // re-resolve at least this often
#define CLOUD_PROBE_FAIL_RERESOLVE 3        // ...and after this many consecutive refusals
#define CLOUD_FAILOPEN_AFTER_MS    60000    // refused for this long -> let update() try anyway
#define CLOUD_FAILOPEN_PERIOD_MS   30000    // ...this often while still refused

static SocketAddress  s_brokerAddr;
static bool           s_brokerResolved = false;
static unsigned long  s_brokerResolvedMs = 0;
static uint32_t       s_probeFailStreak = 0;
static unsigned long  s_probeFailSince  = 0;   // 0 = not currently refused
static unsigned long  s_lastFailOpen    = 0;
static uint32_t       s_cloudFailOpens  = 0;
static unsigned long  s_lastCloudProbe = 0;
static bool           s_lastProbeOk    = false;
static uint32_t       s_cloudProbeOks  = 0;
static uint32_t       s_cloudProbeFails = 0;

inline uint32_t cloudProbeOks()   { return s_cloudProbeOks; }
inline uint32_t cloudProbeFails() { return s_cloudProbeFails; }
inline uint32_t cloudFailOpens()  { return s_cloudFailOpens; }

static unsigned long s_dnsFailMs = 0;   // 0 = no recent failure

//  One bounded knock. False on DNS failure, refuse, timeout, or setup
//  error -- see the fail-closed note above.
static bool cloudProbeOnce() {
  NetworkInterface* net = WiFi.getNetwork();
  if (net == nullptr) return false;

  //  Expire the cached address: by age, or after a run of refusals that
  //  says the node behind it is gone.
  if (s_brokerResolved &&
      (millis() - s_brokerResolvedMs >= CLOUD_DNS_TTL_MS || s_probeFailStreak >= CLOUD_PROBE_FAIL_RERESOLVE)) {
    s_brokerResolved = false;
    s_probeFailStreak = 0;
  }
  if (!s_brokerResolved) {
    //  A failed lookup costs mbed's 5 s x 3 retries. Remember the failure
    //  and answer "unreachable" from memory for a minute instead of
    //  paying it again every knock while the uplink is down.
    if (s_dnsFailMs != 0 && millis() - s_dnsFailMs < CLOUD_DNS_NEG_CACHE_MS) return false;
    SocketAddress a;
    if (net->gethostbyname(CLOUD_PROBE_HOST, &a) != NSAPI_ERROR_OK) { s_dnsFailMs = millis(); return false; }
    s_dnsFailMs = 0;
    a.set_port(CLOUD_PROBE_PORT);
    s_brokerAddr = a;
    s_brokerResolved = true;
    s_brokerResolvedMs = millis();
  }

  TCPSocket sock;
  if (sock.open(net) != NSAPI_ERROR_OK) return false;
  sock.set_timeout(CLOUD_PROBE_TIMEOUT_MS);      // BEFORE connect -- the bound
  nsapi_error_t rc = sock.connect(s_brokerAddr);
  sock.close();
  bool ok = (rc == NSAPI_ERROR_OK || rc == NSAPI_ERROR_IS_CONNECTED);
  if (ok) s_probeFailStreak = 0; else s_probeFailStreak++;
  return ok;
}

//  The gate. Returns true when ArduinoCloud.update() should run this pass.
//  Only the (WiFi up, cloud down) state is ever gated; the caller passes
//  the two flags so this file does not have to know how they are read.
static bool cloudUpdateAllowed(bool wifiUp, bool cloudUp, unsigned long now) {
  if (cloudUp) { s_probeFailSince = 0; s_probeFailStreak = 0; return true; }   // connected: publish, keepalive, OTA -- never skip
  if (!wifiUp)  return true;   // radio down: update() is what re-associates it

  //  WiFi up, cloud down: the next update() is the TLS connect. Knock,
  //  rate-limited; reuse the last verdict between knocks.
  if (s_lastCloudProbe == 0 || now - s_lastCloudProbe >= CLOUD_PROBE_PERIOD_MS) {
    s_lastCloudProbe = now;
    s_lastProbeOk = cloudProbeOnce();
    if (s_lastProbeOk) { s_cloudProbeOks++;   s_probeFailSince = 0; }
    else               { s_cloudProbeFails++; if (s_probeFailSince == 0) s_probeFailSince = now; }
  }
  if (s_lastProbeOk) return true;
  //  Fail-open: refused for a minute straight, so the knock itself may be
  //  the thing that is wrong (stale address, a port the path filters).
  //  Let the library try with its own lookup, every 30 s, until it lands.
  if (s_probeFailSince != 0 && now - s_probeFailSince >= CLOUD_FAILOPEN_AFTER_MS
      && now - s_lastFailOpen >= CLOUD_FAILOPEN_PERIOD_MS) {
    s_lastFailOpen = now;
    s_cloudFailOpens++;
    return true;
  }
  return false;
}

#endif // CLOUD_PROBE_H
