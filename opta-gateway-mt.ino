// =====================================================================
//  opta-gateway-mt -- the IP2 gateway on two threads. MAIN THREAD FILE.
//
//  This file is the cloud side. It owns ArduinoCloud.update() and every
//  Cloud* property. The PLC side is plc_thread.h. The only thing they
//  share is shared.h. See README.md and the design spec it points to.
// =====================================================================
#include "config.h"

#if ENABLE_SERIAL_DEBUG
  #define LOG(...)    Serial.print(__VA_ARGS__)
  #define LOGLN(...)  Serial.println(__VA_ARGS__)
#else
  #define LOG(...)
  #define LOGLN(...)
#endif

#include <malloc.h>
#include "EtherNetIP.h"
#include <PortentaEthernet.h>
#include <WiFi.h>
#include "version.h"
#include "thingProperties.h"
#include "boot_reason.h"
#include "plc_probe.h"
#include "cloud_probe.h"
#include "stack_watch.h"
#include "wd_feeder.h"
#include "plc_tags.h"
#include "shared.h"
#include "plc_thread.h"
#include "cloud_side.h"
#include "cloud_ctrl.h"
static_assert(N_VALVE == 36 && N_STATE == 27 && N_ACTION == 15, "batch 1 sweep tables");
static_assert(PlcSnapshot::STATETEXT_CAP == 128 && PlcSnapshot::FAILTAG_CAP == 40, "batch 1 snapshot strings");
static_assert(sizeof(PlcSnapshot) < 960, "snapshot grew past 960 B");
static_assert(DESORP_TIME_MIN_MIN == 5 && ADSORP_TIME_MIN_MIN == 5, "clamp floor is 5 min for both timers");
static_assert(sizeof(TRIP_HIST_TAGS) / sizeof(TRIP_HIST_TAGS[0]) == TRIP_HIST_N, "five history tags, newest first");
static_assert(N_HP_REAL == 29 && N_HP_BOOL == 11, "batch 5 heat-pump tables");
static_assert(sizeof(PlcSnapshot().hpReal) == N_HP_REAL * sizeof(float), "batch 5 snapshot");
static_assert(WD_AT_WIFI == 14 && WD_AT_CLOUDCONN == 15 && WD_CLOUD_GIVEUP_MS == 300000, "batch 6 feeder");

// ---- the CIP client: DEFINED here, USED only by the PLC thread ----------
EthernetClient   plcTransport;
IPAddress        plcIp(PLC_IP_OCTET_0, PLC_IP_OCTET_1, PLC_IP_OCTET_2, PLC_IP_OCTET_3);
EtherNetIPClient eip(plcTransport, plcIp, PLC_ENIP_PORT);

// ---- pasted verbatim from opta-plc-gateway-ip2.ino, source lines 348-402 --
#include <Arduino_KVStore.h>

#define WIFI_KEY_SSID  "cfg_ssid"
#define WIFI_KEY_PASS  "cfg_pass"

static char    wifiSsid[33];
static char    wifiPass[64];
static KVStore wifiStore;

// Fill wifiSsid/wifiPass: KVStore override if present, else secrets default.
static void loadWifiConfig() {
  strncpy(wifiSsid, SECRET_SSID,      sizeof(wifiSsid) - 1); wifiSsid[32] = 0;
  strncpy(wifiPass, SECRET_OPTA_PASS, sizeof(wifiPass) - 1); wifiPass[63] = 0;
  if (!wifiStore.begin()) { LOGLN("[CFG] KVStore begin failed, using defaults"); return; }
  if (wifiStore.exists(WIFI_KEY_SSID)) {
    char s[33] = {0}, p[64] = {0};
    wifiStore.getString(WIFI_KEY_SSID, s, sizeof(s));
    if (wifiStore.exists(WIFI_KEY_PASS)) wifiStore.getString(WIFI_KEY_PASS, p, sizeof(p));
    if (s[0]) {
      strncpy(wifiSsid, s, sizeof(wifiSsid) - 1); wifiSsid[32] = 0;
      strncpy(wifiPass, p, sizeof(wifiPass) - 1); wifiPass[63] = 0;
      LOG("[CFG] WiFi override from KVStore: "); LOGLN(wifiSsid);
    }
  }
  wifiStore.end();
}

// ---- KVStore helpers shared with the USB serial config (serial_config.h) ----
//  Same keys the web page writes, so whichever channel is used last wins and
//  the two never disagree about where the credentials live.
static void serialConfigApply(const char* ssid, const char* pass) {
  if (!wifiStore.begin()) { LOGLN("[CFG] KVStore begin failed, not saved"); return; }
  wifiStore.putString(WIFI_KEY_SSID, ssid);
  wifiStore.putString(WIFI_KEY_PASS, pass);
  wifiStore.end();
}

static void serialConfigClear() {
  if (!wifiStore.begin()) { LOGLN("[CFG] KVStore begin failed, not cleared"); return; }
  wifiStore.remove(WIFI_KEY_SSID);
  wifiStore.remove(WIFI_KEY_PASS);
  wifiStore.end();
}

static bool serialConfigStored(char* ssidOut, size_t cap) {
  bool has = false;
  if (!wifiStore.begin()) return false;
  if (wifiStore.exists(WIFI_KEY_SSID)) {
    wifiStore.getString(WIFI_KEY_SSID, ssidOut, cap);
    has = ssidOut[0] != 0;
  }
  wifiStore.end();
  return has;
}

#include "serial_config.h"

// ---- pasted verbatim from opta-plc-gateway-ip2.ino, source lines 406-641 --
//  (the brief's stated range was 409-641; widened to start at 406 to
//  include "static EthernetServer cfgServer(80);", which 409 alone would
//  have left undeclared -- see task-9-report.md for why) -------------------
static EthernetServer cfgServer(80);

// Read one CRLF-terminated line, bounded time and length.
static bool readHttpLine(EthernetClient& c, char* buf, size_t cap) {
  size_t i = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < 500) {
    if (!c.available()) { if (!c.connected()) break; continue; }
    int ch = c.read();
    if (ch < 0) continue;
    if (ch == '\n') { if (i && buf[i - 1] == '\r') i--; buf[i] = 0; return true; }
    if (i < cap - 1) buf[i++] = (char)ch;
  }
  buf[(i < cap) ? i : cap - 1] = 0;
  return false;
}

// In-place decode of x-www-form-urlencoded ('+' and %XX).
static void urlDecode(char* s) {
  char* o = s;
  for (char* p = s; *p; p++) {
    if (*p == '+') { *o++ = ' '; continue; }
    if (*p == '%' && p[1] && p[2]) {
      int h = (p[1] >= '0' && p[1] <= '9') ? p[1] - '0' : ((p[1] | 0x20) >= 'a' && (p[1] | 0x20) <= 'f') ? (p[1] | 0x20) - 'a' + 10 : -1;
      int l = (p[2] >= '0' && p[2] <= '9') ? p[2] - '0' : ((p[2] | 0x20) >= 'a' && (p[2] | 0x20) <= 'f') ? (p[2] | 0x20) - 'a' + 10 : -1;
      if (h >= 0 && l >= 0) { *o++ = (char)(h * 16 + l); p += 2; continue; }
    }
    *o++ = *p;
  }
  *o = 0;
}

// Extract "name=value" from a form body into out (URL-decoded).
static bool parseParam(const char* body, const char* name, char* out, size_t cap) {
  size_t nlen = strlen(name);
  const char* p = body;
  while (p && *p) {
    if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
      const char* v = p + nlen + 1;
      size_t i = 0;
      while (v[i] && v[i] != '&' && i < cap - 1) { out[i] = v[i]; i++; }
      out[i] = 0;
      urlDecode(out);
      return true;
    }
    p = strchr(p, '&');
    if (p) p++;
  }
  return false;
}

static void sendConfigPage(EthernetClient& c, const char* msg) {
  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<title>Opta PLC Gateway</title></head>"
            "<body style='font-family:sans-serif;max-width:480px;margin:2em auto'>"
            "<h2>Opta PLC Gateway</h2>"));
  if (msg && msg[0]) { c.print(F("<p style='color:#06c'><b>")); c.print(msg); c.print(F("</b></p>")); }
  c.print(F("<p>WiFi SSID: <b>")); c.print(wifiSsid);
  c.print(F("</b><br>WiFi: <b>"));
  c.print(WiFi.status() == WL_CONNECTED ? F("connected") : F("NOT connected"));
  c.print(F("</b><br>PLC link: <b>"));
  c.print((bool)plcConnected ? F("connected") : F("NOT connected"));
#if USB_LOG_ENABLE
  //  This page is the ONLY console while USB logging is on: the stick owns
  //  the USB-C port, so there is no serial. Print the two things that were
  //  indistinguishable on the bench -- whether the stick was ever DETECTED
  //  (the hotplug callback fired) and whether a mount then SUCCEEDED. A
  //  "detected: no" means the host never enumerated it and no mount was
  //  ever attempted; "detected: yes" with fails climbing means the stick
  //  or its filesystem is the problem. The LED can say neither.
  c.print(F("</b><br>USB logging: <b>"));
  c.print(usbLogEjected() ? F("EJECTED -- SAFE TO REMOVE") : F("live (LED 1 solid)"));
  c.print(F("</b><br>USB stick detected: <b>"));
  c.print(usbLogPresent() ? F("yes") : F("no"));
  c.print(F("</b><br>USB mounts ok/failed: <b>"));
  c.print(usbLogOks()); c.print(F(" / ")); c.print(usbLogFails());
  c.print(F("</b><br>USB rows written: <b>"));
  c.print(usbLogRows());
  c.print(F("</b> (pending ")); c.print(usbLogPending());
  c.print(F(", dropped ")); c.print(usbLogDropped());
  c.print(F(")<br>USB file: <b>"));
  c.print(usbLogFile().length() ? usbLogFile() : String("(none yet)"));
#endif
  c.print(F("</b></p><hr><h3>Set WiFi credentials</h3>"
            "<form method='POST' action='/save'>"
            "SSID:<br><input name='ssid' maxlength='32' style='width:100%'><br>"
            "Password:<br><input name='pass' type='password' maxlength='63' style='width:100%'><br><br>"
            "<input type='submit' value='Save &amp; reboot'></form>"
            "<p><a href='/clear'>Clear override (revert to firmware default) &amp; reboot</a></p>"
#if USB_LOG_ENABLE
            "<p><a href='/log'>View the current USB log file</a>"
            " &nbsp;|&nbsp; <a href='/files'>Manage log files (view / delete)</a></p>"
            "<form method='POST' action='/eject' style='display:inline'>"
            "<input type=submit value='Eject (safe to remove)'></form> "
            "<form method='POST' action='/resume' style='display:inline'>"
            "<input type=submit value='Resume logging'></form>"
#endif
            "</body></html>"));
  c.flush();
}

// Non-blocking-ish: only does work when a browser is connected.
static void handleConfigClient() {
  EthernetClient client = cfgServer.available();
  if (!client) return;

  char line[256], method[8] = {0}, path[64] = {0};
  int contentLen = 0;
  if (!readHttpLine(client, line, sizeof(line))) { client.stop(); return; }
  sscanf(line, "%7s %63s", method, path);

  //  Split the query off the path. Without this every strcmp(path, "/log")
  //  below fails the moment a parameter is appended, which is the shape the
  //  file viewer needs (/log?f=IP2_20260901.csv).
  char* query = strchr(path, '?');
  if (query) { *query = 0; query++; } else { query = path + strlen(path); }

  while (readHttpLine(client, line, sizeof(line)) && line[0]) {
    if (strncasecmp(line, "Content-Length:", 15) == 0) contentLen = atoi(line + 15);
  }

  if (strcmp(method, "POST") == 0 && strcmp(path, "/save") == 0) {
    char body[256] = {0};
    int n = 0;
    unsigned long t0 = millis();
    while (n < contentLen && n < (int)sizeof(body) - 1 && millis() - t0 < 1000) {
      if (client.available()) body[n++] = (char)client.read();
    }
    body[n] = 0;
    char ssid[33] = {0}, pass[64] = {0};
    parseParam(body, "ssid", ssid, sizeof(ssid));
    parseParam(body, "pass", pass, sizeof(pass));
    if (ssid[0] && wifiStore.begin()) {
      wifiStore.putString(WIFI_KEY_SSID, ssid);
      wifiStore.putString(WIFI_KEY_PASS, pass);
      wifiStore.end();
      LOG("[CFG] saved WiFi override: "); LOGLN(ssid);
      sendConfigPage(client, "Saved. Rebooting -- reload this page in ~30 s.");
      delay(200); client.stop(); delay(300);
      bootMarkIntentional("web-save");
      NVIC_SystemReset();
    }
    sendConfigPage(client, "Save FAILED (empty SSID or storage error).");
    client.stop();
#if USB_LOG_ENABLE
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/files") == 0) {
    //  The file manager. A listing rather than a hardcoded "today", because
    //  the reason to come here is usually an OLD day -- and because the only
    //  safe way to offer delete is to let the board name the candidates.
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                   "Connection: close\r\n\r\n"
                   "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<title>Opta log files</title></head>"
                   "<body style='font-family:sans-serif;max-width:640px;margin:2em auto'>"
                   "<h2>USB log files</h2>"));
    usbLogListHtml(client);
    client.print(F("<p><a href='/'>back to gateway config</a></p></body></html>"));
    client.flush();
    client.stop();

  } else if (strcmp(method, "POST") == 0 &&
             (strcmp(path, "/eject") == 0 || strcmp(path, "/resume") == 0)) {
    //  POST for the same reason /rm is: /eject STOPS the logging, and a
    //  gateway silently not logging because a link scanner touched a URL is
    //  a failure nobody would think to look for.
    String msg = (strcmp(path, "/eject") == 0) ? usbLogEject() : usbLogResume();
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                   "Connection: close\r\n\r\n"
                   "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<title>Opta log files</title></head>"
                   "<body style='font-family:sans-serif;max-width:640px;margin:2em auto'>"
                   "<h2>USB log files</h2><p style='color:#06c;font-size:1.2em'><b>"));
    client.print(msg);
    client.print(F("</b></p>"));
    usbLogListHtml(client);
    client.print(F("<p><a href='/'>back to gateway config</a></p></body></html>"));
    client.flush();
    client.stop();

  } else if (strcmp(method, "POST") == 0 && strcmp(path, "/rm") == 0) {
    //  POST, never GET. A GET that deletes gets fired by link prefetch, by
    //  antivirus link scanners and by network scanners -- this gateway
    //  already has one unauthenticated GET that reboots it, and that cost a
    //  day of investigation before it was ruled out.
    char body[128] = {0};
    int n = 0;
    unsigned long t0 = millis();
    while (n < contentLen && n < (int)sizeof(body) - 1 && millis() - t0 < 1000) {
      if (client.available()) body[n++] = (char)client.read();
    }
    body[n] = 0;
    char fname[40] = {0};
    parseParam(body, "f", fname, sizeof(fname));
    String msg = usbLogDelete(String(fname));
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                   "Connection: close\r\n\r\n"
                   "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                   "<title>Opta log files</title></head>"
                   "<body style='font-family:sans-serif;max-width:640px;margin:2em auto'>"
                   "<h2>USB log files</h2><p style='color:#06c'><b>"));
    client.print(msg);
    client.print(F("</b></p>"));
    usbLogListHtml(client);
    client.print(F("<p><a href='/'>back to gateway config</a></p></body></html>"));
    client.flush();
    client.stop();

  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/log") == 0) {
    //  text/plain, not text/csv: a browser offers to download a CSV, and the
    //  point of this endpoint is to LOOK at the rows without saving a file.
    char fname[40] = {0};
    parseParam(query, "f", fname, sizeof(fname));
    client.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                   "Connection: close\r\n\r\n"));
    uint32_t n = usbLogStream(client, String(fname));
    LOG("[USB] served /log, "); LOG(n); LOGLN(" bytes");
    client.flush();
    client.stop();
#endif
  } else if (strcmp(method, "GET") == 0 && strcmp(path, "/clear") == 0) {
    if (wifiStore.begin()) {
      wifiStore.remove(WIFI_KEY_SSID);
      wifiStore.remove(WIFI_KEY_PASS);
      wifiStore.end();
    }
    LOGLN("[CFG] WiFi override cleared");
    sendConfigPage(client, "Override cleared. Rebooting with firmware defaults...");
    delay(200); client.stop(); delay(300);
    bootMarkIntentional("web-clear");
    NVIC_SystemReset();
  } else {
    sendConfigPage(client, nullptr);
    client.stop();
  }
}

// ---- pasted verbatim from opta-plc-gateway-ip2.ino, source lines 891-916 --
//  (the brief's stated range was 896-916; widened to start at 891 to
//  include the "#if WIFI_FORCE_SECURITY" guard line that 896 alone would
//  have omitted -- see task-9-report.md) -----------------------------------
#if WIFI_FORCE_SECURITY
//  Scan-free reconnect. See the SCAN-FREE RECONNECT block in config.h: the
//  handler's own path refuses instantly when one scan misses the beacon, so
//  this runs alongside it and tries the association the driver's way.
//
//  Deliberately a RESCUE, not a replacement. It cannot do WPA3 (enum2sec
//  sends that to NSAPI_SECURITY_UNKNOWN) whereas the scan path can, so both
//  are left running and whichever succeeds first wins. Rate-limited because
//  a connect attempt blocks loop() for up to WiFi's 7 s timeout, and doing
//  that back-to-back would starve the PLC polling and the serial rescue -- the
//  same starvation WIFI_RETRY_MS exists to prevent.
static void wifiRescue(unsigned long now) {
  static unsigned long lastTry = 0;

  if (WiFi.status() == WL_CONNECTED) { lastTry = 0; return; }
  if (lastTry != 0 && (now - lastTry) < WIFI_FORCE_MS) return;
  lastTry = now;

  LOGLN("[WIFI] down -- trying a scan-free connect (explicit WPA/WPA2)");
  if (WiFi.begin(wifiSsid, wifiPass, ENC_TYPE_CCMP) == WL_CONNECTED) {
    LOGLN("[WIFI] connected without a scan -- the beacon was the problem");
    lastError = "wifi recovered via scan-free connect";
  }
}
#endif

// ---- OTA gate: apply unless someone is operating the machine remotely. ---
//  A reboot costs ~30 s of telemetry; the PLC keeps running the machine.
//  The one thing worth waiting for is an in-flight control session, where
//  a reboot would drop a setpoint the operator thinks they made.
#if OTA_ENABLE
bool onOTARequestCallback() {
  bool const allow = !ctrlControlEnabled();
  otaPending = !allow;
  LOG("[OTA] apply requested -> ");
  LOGLN(allow ? "ALLOW" : "POSTPONE (controlEnabled is on)");
  return allow;
}
#endif

// ---- panel LEDs: PLC session, cloud link, fault ---------------------------
//  Fault = PLC disconnected, PLC thread stalled, or any PLC error flag.
static void panelLeds(bool plcStalled) {
  digitalWrite(LED_PLC,   (bool)plcConnected ? HIGH : LOW);
  digitalWrite(LED_CLOUD, ArduinoCloud.connected() ? HIGH : LOW);
  bool fault = !(bool)plcConnected || plcStalled
            || (bool)pressError || (bool)tempError || (bool)genError;
  digitalWrite(LED_FAULT, fault ? HIGH : LOW);
}

static StackWatch s_mainStack;

void setup() {
  bootReasonCapture();                      // FIRST: reads RCC->RSR
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 1500) {}
  bootReasonLog();
  LOG("\n[MT] opta-gateway-mt starting fw="); LOGLN(FW_VERSION);

  g_mainThreadId = osThreadGetId();

  //  Pulse counter armed before anything slow (cloud, PLC) so no pulses
  //  are missed during the rest of boot. Counting only; the PLC thread
  //  does the arithmetic.
  flowInit();
  LOGLN("[FLOW] pulse counter armed on I1 (FALLING)");

  // ---- pasted verbatim from opta-plc-gateway-ip2.ino, source lines 722-751,
  //  Ethernet bring-up + cfgServer.begin() -----------------------------------
  // ---- bring up the PLC network interface ----
#if USE_ETHERNET_FOR_PLC
  #if USE_ETH_DHCP
    LOGLN("[ETH] DHCP...");
    Ethernet.begin();
  #else
    IPAddress optaIp(OPTA_ETH_IP_OCTET_0, OPTA_ETH_IP_OCTET_1,
                     OPTA_ETH_IP_OCTET_2, OPTA_ETH_IP_OCTET_3);
    IPAddress optaMask(OPTA_ETH_MASK_0, OPTA_ETH_MASK_1,
                       OPTA_ETH_MASK_2, OPTA_ETH_MASK_3);
    // dns = 0.0.0.0, gateway = 0.0.0.0  -> wired port is a directly-connected
    // subnet only; the WiFi link keeps the default route to the internet.
    // Signature: begin(local_ip, dns, gateway, subnet).
    LOGLN("[ETH] static IP (no gateway; WiFi holds the default route)");
    Ethernet.begin(optaIp, IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), optaMask);
  #endif
  // Print the address the interface ACTUALLY came up with, plus which build
  // instance this is. Two boards flashed from one source tree get the same
  // static IP unless GATEWAY_INSTANCE differs (see config.h), and a duplicate
  // IP shows up as random session drops on BOTH boards -- so make the address
  // observable instead of assumed. Ethernet.localIP() is the interface's own
  // report, not an echo of the compile-time constant.
  LOG("[ETH] instance="); LOG(GATEWAY_INSTANCE);
  LOG(" configured="); LOG(OPTA_ETH_IP_OCTET_0); LOG('.'); LOG(OPTA_ETH_IP_OCTET_1);
  LOG('.'); LOG(OPTA_ETH_IP_OCTET_2); LOG('.'); LOG(OPTA_ETH_IP_OCTET_3);
  LOG("  actual="); LOGLN(Ethernet.localIP());
  cfgServer.begin();          // WiFi config page: http://<opta-eth-ip>/
  LOGLN("[CFG] web config page listening on port 80");
#endif

  // ---- Arduino Cloud over WiFi -------------------------------------------
  loadWifiConfig();
  static WiFiConnectionHandler cloudConn(wifiSsid, wifiPass);
  {
    TimeoutTable t = DefaultTimeoutTable;
    t.timeout.init       = WIFI_RETRY_MS;
    t.timeout.connecting = WIFI_RETRY_MS;
    cloudConn.updateTimeoutTable(t);
  }
  initProperties();
  fwVersion  = FW_VERSION;
  bootReason = bootReasonStr();
  ArduinoCloud.begin(cloudConn);
  ctrlBegin();
#if OTA_ENABLE
  ArduinoCloud.onOTARequestCb(onOTARequestCallback);
#endif
#if ENABLE_SERIAL_DEBUG
  setDebugMessageLevel(CLOUD_DEBUG_LEVEL);
  ArduinoCloud.printDebugInfo();
#endif

  pinMode(LED_PLC,   OUTPUT);
  pinMode(LED_CLOUD, OUTPUT);
  pinMode(LED_FAULT, OUTPUT);

  // ---- threads ------------------------------------------------------------
  //  The PLC thread starts BEFORE the feeder so its first beat exists when
  //  the feeder takes its first look. setup() does not call begin() on the
  //  CIP client: the PLC thread does its own first connect, through the
  //  probe, on its own stack.
  plcThreadBegin();
  wdFeederBegin();

  s_mainStack.begin();                      // LAST: paints below this frame
}

void loop() {
  wdBeatMain();
  wdWhereMain(WD_AT_WEB);
  handleConfigClient();
  serialConfigPoll();

  // ---- take the PLC snapshot, assign Cloud* once per PLC tick -------------
  static uint32_t lastSeq = 0;
  cloudSideConsume(lastSeq);
  //  Not stalled before the PLC thread has published once: stampMs is 0
  //  until then and would read as a minutes-old snapshot.
  bool plcStalled = cloudSideHasSnapshot() && cloudSideSnapshotAgeMs() > 3 * SAMPLE_INTERVAL_MS;
  if (plcStalled) {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 10000) {
      lastWarn = millis();
      char b[PlcSnapshot::LASTERR_CAP];
      snprintf(b, sizeof b, "plc thread stalled %lus", (unsigned long)(cloudSideSnapshotAgeMs() / 1000));
      lastError = String(b);
      LOGLN(b);
    }
  }

  // ---- cloud, gated only in the (WiFi up, cloud down) state ---------------
  unsigned long cloudT0 = millis();
  wdWhereMain(WD_AT_CLOUDPROBE);
  bool runCloud = cloudUpdateAllowed(WiFi.status() == WL_CONNECTED,
                                     ArduinoCloud.connected(), cloudT0);
  wdWhereMain(WD_AT_CLOUD);
  if (runCloud) ArduinoCloud.update();
  cloudMs = (int)(millis() - cloudT0);
#if WIFI_FORCE_SECURITY
  wifiRescue(millis());
#endif
  wdWhereMain(WD_AT_NONE);

  unsigned long now = millis();

  // ---- panel LEDs, 3 s, regardless of serial debug -------------------------
  static unsigned long lastLed = 0;
  if (now - lastLed >= 3000) {
    lastLed = now;
    panelLeds(plcStalled);
  }

  // ---- heartbeat, 3 s, serial only ----------------------------------------
#if ENABLE_SERIAL_DEBUG
  static unsigned long lastHb = 0;
  if (now - lastHb >= 3000) {
    lastHb = now;
    LOG("[HB] wifi=");  LOG(WiFi.status() == WL_CONNECTED ? "up" : "down");
    LOG(" cloud=");     LOG(ArduinoCloud.connected() ? "up" : "down");
    LOG(" plc=");       LOG((bool)plcConnected ? "1" : "0");
    LOG(" seqAge=");    LOG(cloudSideSnapshotAgeMs());
    LOG(" cloudMs=");   LOG((int)cloudMs);
    LOG(" eipMs=");     LOG((int)eipMs);
    LOG(" stall=");     LOG(wdStallMax()); LOG("@"); LOG(wdStallWhere());
    LOG(" mainStk=");   LOG(s_mainStack.minFree());
    LOG(" plcStk=");    LOG(cloudSidePlcStackFree());
    LOG(" t1=");        LOG((float)t1HotTank);
    LOG(" lvl=");       LOG((float)tankLevel);
    LOGLN("");
  }
#endif

  // ---- diagnostics, 30 s --------------------------------------------------
  static unsigned long lastDiag = 0;
  if (now - lastDiag >= DIAG_PUBLISH_MS) {
    lastDiag = now;
    struct mallinfo mi = mallinfo();
    heapUsed = (int)mi.uordblks;
    heapFree = (int)mi.fordblks;
    uptimeS  = (int)(now / 1000UL);
    s_mainStack.sample();
    stackFree   = (int)s_mainStack.minFree();
    loopStallMs = (int)wdStallMax();
    stallWhere  = (int)wdStallWhere();
    wifiRssi    = (int)WiFi.RSSI();
  }

  loopMs = (int)(millis() - now);
}
