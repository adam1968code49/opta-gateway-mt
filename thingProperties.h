#pragma once
// =====================================================================
//  Batch 0 property set. Declarations and registrations are taken from
//  opta-plc-gateway-ip2/thingProperties.h with everything not yet migrated
//  removed: no READWRITE property, no onXxxChange prototype, no valve /
//  panel / water / AI / heat-pump / USB property. Those return with their
//  batches. Names are unchanged so the same Thing and dashboard keep
//  working; properties this build does not register simply stop updating.
// =====================================================================
#include "config.h"
#include <ArduinoIoTCloud.h>
#include <Arduino_ConnectionHandler.h>
#include "arduino_secrets.h"

// ---- sensors: 28, in SENSOR_TAGS slot order ----------------------------
CloudTemperatureSensor t1HotTank;
CloudTemperatureSensor t2ColdTank;
CloudTemperatureSensor t3TopChIn;
CloudTemperatureSensor t4TopChOut;
CloudTemperatureSensor t5BotChIn;
CloudTemperatureSensor t6BotChOut;
CloudTemperatureSensor t7HotHxIn;
CloudTemperatureSensor t8HotHxOut;
CloudTemperatureSensor t9BoosterIn;
CloudTemperatureSensor t10BoosterOut;
CloudTemperatureSensor t11CondenserOut;
CloudTemperatureSensor t12HeatPumpIn;
CloudTemperatureSensor vtVaisalaTemp;
CloudFloat p1TopChamber;
CloudFloat p2BotChamber;
CloudFloat p3ChamberRedun;
CloudFloat p4Booster;
CloudFloat p5Collector;
CloudFloat p6VaisalaWvp;
CloudFloat hp1HotInlet;
CloudFloat hp1ColdInlet;
CloudFloat hp2HotInlet;
CloudFloat hp2ColdInlet;
CloudTemperatureSensor atmTemp;
CloudRelativeHumidity  atmRh;
CloudTemperatureSensor atmTempDs;
CloudRelativeHumidity  atmRhDs;
CloudFloat tankLevel;

// ---- diagnostics -------------------------------------------------------
CloudBool   plcConnected;
CloudInt    loopMs;         // main pass duration after update()
CloudInt    cloudMs;        // duration of ArduinoCloud.update()
CloudInt    eipMs;          // duration of the last eip.begin() attempt
CloudString lastError;
CloudString fwVersion;
CloudString bootReason;
CloudInt    stackFree;      // main thread stack headroom (PLC thread's is batch 3)
CloudInt    loopStallMs;    // worst stall seen across BOTH threads
CloudInt    stallWhere;     // WD_AT_* code at that maximum
CloudInt    wifiRssi;
CloudInt    heapUsed;
CloudInt    heapFree;
CloudInt    uptimeS;
#if OTA_ENABLE
CloudBool   otaPending;
#endif

#define PUB_DELTA 0.1f

void initProperties() {
  // --- sensors ---
  ArduinoCloud.addProperty(t1HotTank,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(t2ColdTank,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(t3TopChIn,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(t4TopChOut,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(t5BotChIn,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(t6BotChOut,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(t7HotHxIn,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(t8HotHxOut,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(t9BoosterIn,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(t10BoosterOut,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(t11CondenserOut, READ, ON_CHANGE);
  ArduinoCloud.addProperty(t12HeatPumpIn,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(vtVaisalaTemp,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(p1TopChamber,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(p2BotChamber,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(p3ChamberRedun, READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(p4Booster,      READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(p5Collector,    READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(p6VaisalaWvp,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hp1HotInlet,    READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hp1ColdInlet,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hp2HotInlet,    READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hp2ColdInlet,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(atmTemp,         READ, ON_CHANGE);
  ArduinoCloud.addProperty(atmTempDs,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(atmRh,           READ, ON_CHANGE);
  ArduinoCloud.addProperty(atmRhDs,         READ, ON_CHANGE);
  ArduinoCloud.addProperty(tankLevel,       READ, ON_CHANGE, NULL, PUB_DELTA);

  // --- diagnostics ---
  ArduinoCloud.addProperty(plcConnected, READ, ON_CHANGE);
  ArduinoCloud.addProperty(loopMs,       READ, 5 * SECONDS);
  ArduinoCloud.addProperty(cloudMs,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(eipMs,        READ, ON_CHANGE);
  ArduinoCloud.addProperty(lastError,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(fwVersion,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(bootReason,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(stackFree,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(loopStallMs,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(stallWhere,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(wifiRssi,     READ, 30 * SECONDS);
  ArduinoCloud.addProperty(heapUsed,     READ, 30 * SECONDS);
  ArduinoCloud.addProperty(heapFree,     READ, 30 * SECONDS);
  ArduinoCloud.addProperty(uptimeS,      READ, 30 * SECONDS);
#if OTA_ENABLE
  ArduinoCloud.addProperty(otaPending,   READ, ON_CHANGE);
#endif
}
