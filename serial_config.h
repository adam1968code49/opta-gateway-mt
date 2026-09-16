// =====================================================================
//  serial_config.h  --  set the WiFi credentials over the USB serial port
//
//  Why this exists: the RJ45 web page can already store credentials, but
//  reaching it needs the PC on the gateway's static-IP machine network --
//  a chicken-and-egg problem when the board has never been on that
//  network. A USB cable always works, needs no network at all, and does
//  not mean reflashing (which would also mean rebuilding with new
//  secrets). Same KVStore keys as the web page, so the two agree.
//
//  Commands (type into any serial monitor at 115200, newline-terminated):
//      show          current SSID + whether an override is stored
//                    (the password is NEVER printed -- only its length)
//      ssid <name>   stage an SSID   (not written yet)
//      pass <secret> stage a password (not written yet)
//      save          write the staged pair to KVStore and reboot
//      clear         delete the override, fall back to arduino_secrets.h
//      help          list these
//
//  Staging then save() is deliberate: a stray keystroke in a terminal
//  should not be able to take the gateway off the network. Nothing is
//  written until "save", and save refuses an empty SSID.
//
//  SECURITY: this is a physical-access channel -- anyone who can plug in
//  a USB cable can change the network. That is the same trust level as
//  being able to reflash the board, so it adds no new exposure. The
//  password is write-only through this interface.
//
//  Include AFTER config.h (needs LOG/LOGLN and the KVStore keys).
// =====================================================================
#pragma once
#include <Arduino.h>

//  Provided by the .ino: the same KVStore instance and key names the web
//  config page uses.
static void serialConfigApply(const char* ssid, const char* pass);
static void serialConfigClear();
static bool serialConfigStored(char* ssidOut, size_t cap);

//  Provided by boot_reason.h: latch WHICH path asked for this reset, so
//  the next boot can tell an operator-driven restart from an unexplained one.
inline void bootMarkIntentional(const char* tag);

static void wifiPortalRequest();   // wifi_portal.h (batch 16), included later in the sketch

inline void serialConfigHelp() {
  LOGLN("[CFG] commands: show | ssid <name> | pass <secret> | save | clear | portal | help");
  LOGLN("[CFG]   portal = open the setup hotspot (WiFi 'IP2-setup', http://192.168.3.1/) on the next boot");
}

inline void serialConfigPoll() {
  static char    line[128];
  static uint8_t len = 0;
  static char    stagedSsid[33] = {0};
  static char    stagedPass[64] = {0};

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c != '\n') {
      if (len < sizeof(line) - 1) line[len++] = c;
      continue;                      // keep buffering until end of line
    }
    line[len] = 0;
    uint8_t n = len;
    len = 0;
    if (n == 0) continue;

    //  trim trailing spaces so "ssid foo " does not store the space
    while (n && line[n - 1] == ' ') line[--n] = 0;

    if (!strcmp(line, "help")) { serialConfigHelp(); }

    else if (!strcmp(line, "show")) {
      char stored[33] = {0};
      bool has = serialConfigStored(stored, sizeof(stored));
      LOG("[CFG] active SSID: ");   LOGLN(wifiSsid);
      LOG("[CFG] override in flash: ");
      if (has) { LOG("yes -> "); LOGLN(stored); } else { LOGLN("no (using arduino_secrets.h)"); }
      LOG("[CFG] staged: ssid='"); LOG(stagedSsid);
      LOG("' pass=");              LOG((int)strlen(stagedPass)); LOGLN(" chars");
    }

    else if (!strncmp(line, "ssid ", 5)) {
      strncpy(stagedSsid, line + 5, sizeof(stagedSsid) - 1);
      stagedSsid[sizeof(stagedSsid) - 1] = 0;
      LOG("[CFG] staged SSID: "); LOGLN(stagedSsid);
      LOGLN("[CFG] type 'save' to write it");
    }

    else if (!strncmp(line, "pass ", 5)) {
      strncpy(stagedPass, line + 5, sizeof(stagedPass) - 1);
      stagedPass[sizeof(stagedPass) - 1] = 0;
      //  never echo the secret back
      LOG("[CFG] staged password: "); LOG((int)strlen(stagedPass)); LOGLN(" chars");
      LOGLN("[CFG] type 'save' to write it");
    }

    else if (!strcmp(line, "save")) {
      if (!stagedSsid[0]) {
        LOGLN("[CFG] refusing: no SSID staged (use 'ssid <name>' first)");
      } else {
        LOG("[CFG] saving SSID '"); LOG(stagedSsid); LOGLN("' + password to flash");
        serialConfigApply(stagedSsid, stagedPass);
        LOGLN("[CFG] saved -- rebooting to reconnect");
        Serial.flush();
        delay(200);
        bootMarkIntentional("serial-save");
        NVIC_SystemReset();
      }
    }

    else if (!strcmp(line, "portal")) {          // batch 16: open the setup hotspot on the next boot
      wifiPortalRequest();
      LOGLN("[CFG] setup hotspot armed for the next boot: WiFi 'IP2-setup', page at http://192.168.3.1/ (10 min window)");
    }
    else if (!strcmp(line, "clear")) {
      serialConfigClear();
      LOGLN("[CFG] override cleared -- rebooting onto the arduino_secrets.h default");
      Serial.flush();
      delay(200);
      bootMarkIntentional("serial-clear");
      NVIC_SystemReset();
    }

#if BOOT_FAULT_TEST
    else if (!strcmp(line, "crash")) {
      //  A write through a null pointer: the shortest route to a hard fault
      //  that the optimiser cannot delete, because the destination is
      //  volatile and therefore observable.
      LOGLN("[CFG] faulting on purpose (BOOT_FAULT_TEST)");
      Serial.flush();
      delay(100);
      *(volatile uint32_t*)0 = 0xDEADBEEF;
    }
#endif

    else {
      LOG("[CFG] unknown command: "); LOGLN(line);
      serialConfigHelp();
    }
  }
}
