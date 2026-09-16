#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

// =====================================================================
//  BATCH 16: SETUP HOTSPOT. Runs on the MAIN thread inside setup(), after
//  loadWifiConfig() and BEFORE the cloud/PLC/feeder threads start.
//
//  The board opens its own WPA2 access point ("IP2-setup", 192.168.3.1,
//  DHCP served by the core), serves one small page, and stores whatever
//  SSID/password the operator types through the SAME KVStore keys the
//  Ethernet page and the USB serial console use. Then it reboots.
//
//  Why a boot-phase mode and not "always on": in mbed_opta 4.6.0
//  WiFi.getNetwork() returns the soft-AP interface whenever one exists, so
//  EVERY socket (cloud MQTT, InfluxDB, this page) would route through the
//  hotspot for as long as it is up. The hotspot is therefore opened only
//  when the board has reason to believe it cannot get online anyway, and
//  it is closed before anything else touches the network.
//
//  Closing it has a trap: WiFi.end() calls disconnect() only when the
//  status is WL_CONNECTED. In AP mode the status is WL_AP_*, so end() alone
//  just drops the pointer and the AP keeps beaconing inside the driver.
//  disconnect() FIRST (it stops the soft-AP when one exists), then end().
//
//  Triggers, any one of:
//    - BTN_USER held through boot (PORTAL_BUTTON_HOLD_MS; sampled after Ethernet bring-up)
//    - no SSID at all
//    - KVStore flag cfg_portal (set by the serial command "portal" or the
//      Ethernet page's /portal, cleared on entry)
//    - KVStore counter cfg_noassoc >= PORTAL_NOASSOC_BOOTS: incremented
//      every boot, zeroed by the main loop the first time WiFi associates.
//      Wrong credentials make the ladder reset the board every ~60 min, so
//      the door opens by itself on the fifth such boot, for a 2 min window.
//
//  Zero dynamic allocation: fixed buffers, no String. The WiFiServer and
//  its client are the core's objects, created once.
// =====================================================================

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "boot_reason.h"

#ifndef SECRET_PORTAL_PASS
//  arduino_secrets.h did not set one. A default keeps the build green, but
//  it is printed at boot as a nag: a plant board must carry its own.
#define SECRET_PORTAL_PASS    "opta-setup-2026"
#define PORTAL_PASS_IS_DEFAULT 1
#else
#define PORTAL_PASS_IS_DEFAULT 0
#endif

#define PORTAL_SSID            "IP2-setup"
#define PORTAL_TIMEOUT_MS      600000UL   // 10 min for a human trigger (button, serial/web request, no SSID)
#define PORTAL_AUTO_TIMEOUT_MS 120000UL   // 2 min when the board opened it by itself (no-association counter):
                                          // during a router outage the ladder resets hourly, so this window is
                                          // paid once per PORTAL_NOASSOC_BOOTS resets -- keep it short (review 5)
#define PORTAL_BUTTON_HOLD_MS  2000UL     // BTN_USER held this long through boot
#define PORTAL_NOASSOC_BOOTS   5          // boots without a single association -> portal
#define PORTAL_MAX_HDR_LINES   40         // a client dripping headers must not hold the loop (review C1)
#define PORTAL_KEY_FLAG        "cfg_portal"
#define PORTAL_KEY_NOASSOC     "cfg_noassoc"

static WiFiServer s_portalSrv(80);

//  ---- KVStore bookkeeping (main thread; threads are not running yet at
//  boot, and the one call from loop() takes the shared mutex) ----------
//  Under g_kvMutex like every other KVStore writer that can run beside the
//  cloud/feeder threads (review 4). Uncontended at boot; recursive, so the
//  caller in noteRunning may already hold it.
static int32_t portalKvGet(const char* key) {
  int32_t v = 0;
  if (!g_kvMutex.trylock_for(std::chrono::milliseconds(KV_LOCK_WAIT_MS))) return 0;
  if (wifiStore.begin()) { v = wifiStore.getInt(key, 0); wifiStore.end(); }
  g_kvMutex.unlock();
  return v;
}
static void portalKvPut(const char* key, int32_t v) {
  if (!g_kvMutex.trylock_for(std::chrono::milliseconds(KV_LOCK_WAIT_MS))) return;
  if (wifiStore.begin()) { wifiStore.putInt(key, v); wifiStore.end(); }
  g_kvMutex.unlock();
}

//  Serial "portal" / web /portal: open the hotspot on the next boot.
static void wifiPortalRequest() { portalKvPut(PORTAL_KEY_FLAG, 1); }   // forward-declared in serial_config.h

//  Decide at boot. Also does the per-boot counter increment, so call it
//  exactly once. Returns a short reason or nullptr.
static bool s_portalAuto = false;   // opened by the counter, not by a human: shorter window

static const char* wifiPortalWanted(const char* ssid) {
  //  Counter first: it must count this boot whatever else happens.
  int32_t noAssoc = portalKvGet(PORTAL_KEY_NOASSOC) + 1;
  portalKvPut(PORTAL_KEY_NOASSOC, noAssoc);
  //  The request flag is consumed on THIS boot whichever branch fires
  //  (review 2: a virgin board that was also armed would otherwise open a
  //  second door right after its first save).
  const bool requested = portalKvGet(PORTAL_KEY_FLAG) != 0;
  if (requested) portalKvPut(PORTAL_KEY_FLAG, 0);

  const char* why = nullptr;
  if (ssid == nullptr || ssid[0] == 0)        why = "no SSID stored";
  else if (requested)                         why = "requested (serial/web)";
  else if (noAssoc >= PORTAL_NOASSOC_BOOTS) { why = "no association for 5 boots"; s_portalAuto = true; }
  else {
    pinMode(BTN_USER, INPUT_PULLUP);          // Opta front button, LOW when pressed; pull-up so it cannot float
    if (digitalRead(BTN_USER) == LOW) {
      unsigned long t0 = millis();
      why = "USER button held";
      while (millis() - t0 < PORTAL_BUTTON_HOLD_MS) {
        if (digitalRead(BTN_USER) != LOW) { why = nullptr; break; }   // a tap, not a hold
        delay(10);
      }
    }
  }
  if (why) portalKvPut(PORTAL_KEY_NOASSOC, 0);   // any door opened resets the count: one door per N boots, not every boot
  return why;
}

//  Called from loop(): the first association zeroes the boot counter.
//  One KVStore write in the life of a boot, under the shared mutex.
inline void wifiPortalNoteRunning() {
  static bool done = false;
  if (done || WiFi.status() != WL_CONNECTED) return;
  if (!g_kvMutex.trylock_for(std::chrono::milliseconds(KV_LOCK_WAIT_MS))) return;   // busy: try again next pass
  done = true;
  portalKvPut(PORTAL_KEY_NOASSOC, 0);       // recursive mutex: the nested lock inside is fine
  g_kvMutex.unlock();
}

//  ---- the page ------------------------------------------------------
static void portalPage(WiFiClient& c, const char* curSsid, const char* msg) {
  c.print(F("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n"
            "<!DOCTYPE html><html><head><meta charset='utf-8'>"
            "<meta name='viewport' content='width=device-width,initial-scale=1'>"
            "<title>Opta WiFi setup</title></head>"
            "<body style='font-family:sans-serif;max-width:420px;margin:2em auto;padding:0 1em'>"
            "<h2>Opta gateway: WiFi setup</h2>"));
  if (msg && msg[0]) { c.print(F("<p style='color:#06c;font-size:1.1em'><b>")); c.print(msg); c.print(F("</b></p>")); }
  c.print(F("<p>Firmware <b>")); c.print(FW_VERSION);
  c.print(F("</b><br>Stored network: <b>")); c.print((curSsid && curSsid[0]) ? curSsid : "(none)");
  c.print(F("</b></p>"
            "<form method='POST' action='/save'>"
            "Network name (SSID):<br><input name='ssid' maxlength='32' style='width:100%;font-size:1.1em'><br><br>"
            "Password:<br><input name='pass' type='password' maxlength='63' style='width:100%;font-size:1.1em'><br><br>"
            "<input type='submit' value='Save and reboot' style='font-size:1.1em;padding:.4em 1em'></form>"
            "<p style='color:#666'>The board reboots and joins that network. This hotspot closes by itself "
            "after 10 minutes if nothing is saved.</p></body></html>"));
  c.flush();
}

//  One HTTP exchange. Returns true when credentials were saved (the caller
//  reboots). Uses the sketch's readHttpLine()/parseParam(), defined above
//  the include point.
static bool portalHandle(WiFiClient& c, const char* curSsid, unsigned long deadlineMs) {
  char line[256], method[8] = {0}, path[64] = {0};
  int contentLen = 0;
  if (!readHttpLine(c, line, sizeof line)) { c.stop(); return false; }
  sscanf(line, "%7s %63s", method, path);
  //  Bounded (review C1): readHttpLine restarts its own 500 ms timer per
  //  line, so a client dripping one header every 400 ms would otherwise hold
  //  this loop past the portal timeout -- with no watchdog running yet.
  int hdr = 0;
  while (hdr++ < PORTAL_MAX_HDR_LINES && (long)(millis() - deadlineMs) < 0 &&
         readHttpLine(c, line, sizeof line) && line[0])
    if (strncasecmp(line, "Content-Length:", 15) == 0) contentLen = atoi(line + 15);
  if ((long)(millis() - deadlineMs) >= 0) { c.stop(); return false; }

  if (strcmp(method, "POST") == 0 && strcmp(path, "/save") == 0) {
    char body[256] = {0};
    int n = 0;
    unsigned long t0 = millis();
    while (n < contentLen && n < (int)sizeof body - 1 && millis() - t0 < 1000)
      if (c.available()) body[n++] = (char)c.read();
    body[n] = 0;
    char ssid[33] = {0}, pass[64] = {0};
    parseParam(body, "ssid", ssid, sizeof ssid);
    parseParam(body, "pass", pass, sizeof pass);
    if (!ssid[0]) { portalPage(c, curSsid, "Empty network name -- nothing saved."); c.stop(); return false; }
    serialConfigApply(ssid, pass);
    LOG("[PORTAL] saved SSID '"); LOG(ssid); LOGLN("' -- rebooting");
    portalPage(c, ssid, "Saved. The board is rebooting and will join the new network.");
    delay(300); c.stop(); delay(300);
    return true;
  }
  portalPage(c, curSsid, nullptr);
  c.stop();
  return false;
}

//  Open the hotspot, serve until saved or timed out, close it cleanly.
//  Blocking by design: nothing else is running yet.
static void wifiPortalRun(const char* curSsid, const char* why) {
  LOG("[PORTAL] opening hotspot "); LOG(PORTAL_SSID); LOG(" ("); LOG(why); LOGLN(")");
  if (PORTAL_PASS_IS_DEFAULT) LOGLN("[PORTAL] WARNING: hotspot password is the built-in default -- set SECRET_PORTAL_PASS in arduino_secrets.h");

  if (WiFi.status() == WL_NO_MODULE) {        // review 7: disconnect()/localIP() dereference a null interface without this
    LOGLN("[PORTAL] no WiFi module -- cannot open a hotspot");
    return;
  }
  int st = WiFi.beginAP(PORTAL_SSID, SECRET_PORTAL_PASS);
  if (st != WL_AP_LISTENING) {
    LOG("[PORTAL] beginAP failed, status "); LOGLN(st);
    WiFi.disconnect(); WiFi.end();
    return;
  }
  LOG("[PORTAL] listening at "); LOGLN(WiFi.localIP());
  s_portalSrv.begin();

  const unsigned long window = s_portalAuto ? PORTAL_AUTO_TIMEOUT_MS : PORTAL_TIMEOUT_MS;
  LOG("[PORTAL] window "); LOG(window / 1000); LOGLN(" s");
  unsigned long t0 = millis(), lastBlink = 0;
  const unsigned long deadline = t0 + window;
  bool led = false, saved = false;
  int lastSt = st;
  while (!saved && (long)(millis() - deadline) < 0) {
    WiFiClient c = s_portalSrv.accept();
    if (c) saved = portalHandle(c, curSsid, deadline);
    int now = WiFi.status();
    if (now != lastSt) {
      lastSt = now;
      LOG("[PORTAL] "); LOGLN(now == WL_AP_CONNECTED ? "a device joined" : (now == WL_AP_LISTENING ? "device left, listening" : "status changed"));
    }
    serialConfigPoll();                        // the USB console keeps working meanwhile
    if (millis() - lastBlink >= 500) { lastBlink = millis(); led = !led; digitalWrite(LED_CLOUD, led ? HIGH : LOW); }
    delay(10);
  }
  digitalWrite(LED_CLOUD, LOW);

  //  Order matters (see the header comment): the listener goes first (its
  //  socket lives on the AP interface, review 3), disconnect() stops the AP,
  //  end() forgets it, and only then do sockets route via the station.
  s_portalSrv.end();
  WiFi.disconnect();
  WiFi.end();
  if (saved) {
    bootMarkIntentional("portal-save");
    delay(50);
    NVIC_SystemReset();
  }
  LOGLN("[PORTAL] timed out, hotspot closed -- normal boot");
}

//  The one call setup() makes.
inline void wifiPortalBoot(const char* curSsid) {
  pinMode(LED_CLOUD, OUTPUT);
  const char* why = wifiPortalWanted(curSsid);
  if (why) wifiPortalRun(curSsid, why);
}

#endif // WIFI_PORTAL_H
