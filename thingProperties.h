#pragma once
// =====================================================================
//  Batch 1 property set: the 43 of batch 0 plus valves / pumps /
//  positions, fault flags, heat-pump St_* status bits, PLC state words,
//  cycle timers, and the eight READWRITE controls (seven machine controls and flowResetTotal) with their callbacks.
//  Declarations and registrations are verbatim from
//  opta-plc-gateway-ip2/thingProperties.h so the same Thing and dashboard
//  keep working. Still absent (later batches): water accounting writes,
//  heat-pump control and BACnet status, USB. (NanoEdge AI dropped 2026-09-08.)
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

// ---- valves / pumps / positions, in VALVE_TAGS slot order 0..21 ---------
CloudBool   valveS1;          // pneumatic, discrete
CloudBool   valveS5;
CloudBool   valveS6;          // top chamber -> turbo
CloudBool   valveS7;          // bottom chamber -> turbo
CloudBool   valveS10;
CloudBool   valveV1A;         // water, discrete: top chamber inlet
CloudBool   valveV1B;         //                  bottom chamber inlet
CloudBool   valveV2A1;        //                  chamber outlet -> hot hx
CloudBool   valveV2A2;
CloudBool   valveV2B;
CloudBool   pumpScroll;       // Edwards scroll (roughing) pump
CloudBool   pumpCond;         // Lefoo condensate pump
CloudBool   pumpHotWater;     // Grundfos
CloudBool   pumpColdWater;    // AMT
CloudBool   boosterVfdRun;    // booster VFD run command
CloudFloat  posS2;            // proportional valve positions (0/45/50)
CloudFloat  posS3;
CloudFloat  posS4;            // collection chamber vent
CloudFloat  posS8;
CloudFloat  posS9;
CloudFloat  posV10;           // top chamber vent
CloudFloat  posV11;           // bottom chamber vent

// ---- water totals read back from the PLC (slots 22, 23) -----------------
CloudFloat  hmiWaterTotal;          // RO: hourly odometer the HMI shows
CloudFloat  cumulativeWaterVolume;  // RO: lifetime total as stored in the PLC

// ---- PLC fault flags (slots 24..26) --------------------------------------
CloudBool   pressError;             // RO: Press_Error from the PLC
CloudBool   tempError;              // RO: Temp_Error from the PLC
CloudBool   genError;               // RO: Gen_Error from the PLC

// ---- heat pump's own status over BACnet (slots 27..35) -------------------
CloudBool   stStage1;               // compressor stage 1 running
CloudBool   stStage2;               // compressor stage 2 running
CloudBool   stAuxHeat;              // auxiliary electric heat on
CloudBool   stIndoorCirc;           // indoor circulator running
CloudBool   stIndoorFlow;           // indoor flow proved
CloudBool   stOutdoorFlow;          // outdoor flow proved
CloudBool   stLockout;              // unit locked out
CloudBool   stPhaseFault;           // phase fault
CloudBool   stBACnetControl;        // unit is under BACnet control

// ---- valve-sweep health ---------------------------------------------------
CloudInt    plcReadFails;           // RO: failing tags in the status sweep
CloudString plcFailTag;             // RO: first failing tag, "ok" when none

// ---- PLC state words and cycle timers -----------------------------------
CloudInt    plcActionWord;    // bit N = Action_(N+1)
CloudInt    plcStateWord;     // buttons / states / fans / door indicators
CloudString plcStateText;
CloudInt    adsorpElapsedS;    // Timer_3.ACC
CloudInt    desorpElapsedT6S;  // Timer_6[3].ACC   (top chamber)
CloudInt    desorpElapsedT11S; // Timer_11[3].ACC  (bottom chamber)

// ---- water accounting (batch 2), computed on the PLC thread --------------
CloudFloat  flowRate;               // RO: L/min, from pulse frequency (F = 38*Q)
CloudFloat  flowTotal;              // RO: cumulative liters through the sensor
CloudFloat  flowBatch;              // RO: liters of THIS discharge (see config.h)
CloudBool   flowResetTotal;         // RW momentary: reset the cumulative total
CloudBool   plcFlowWriteOk;
CloudFloat  displayWaterVolume;     // RO: LIVE lifetime total (not the HMI's)
CloudFloat  tripWaterVolume;        // RO: LIVE trip total
CloudFloat  waterOwedL;             // RO: liters awaiting commit to the PLC
CloudBool   hmiWriteOk;             // RO: last hourly HMI update succeeded
CloudInt    hmiWriteCount;          // RO: hourly HMI writes performed
CloudInt    plcTotalRestores;       // RO: lifetime-total restores performed

// ---------------- Heat pump: PLC staging status (read-only) --------------
CloudTemperatureSensor hpLoopTemp;    // HP_LoopTemp_PV, from HP_In.Indoor_OUT
CloudFloat hpStage1ActivT;            // computed stage-1 turn-on temperature
CloudFloat hpStage2ActivT;            // computed stage-2 turn-on temperature
CloudBool  hpStage1Call;              // stage 1 demand
CloudBool  hpStage2Call;              // stage 2 demand
CloudBool  hpStage1Sat;               // stage 1 reached setpoint
CloudBool  hpStage2Sat;               // stage 2 reached setpoint
CloudBool  hpY1Cmd;                   // stage 1 command driving the output
CloudBool  hpY2Cmd;                   // stage 2 command driving the output
CloudBool  hpReverseCmd;              // reversing valve command
CloudBool  hpCoolRequest;             // 0 = heating, 1 = cooling
CloudBool  hpTempValid;               // loop-temp sanity check passed
CloudBool  hpEnableSt;                // HP_Enable readback (see hpEnable RW)
CloudBool  hpManualOvrSt;             // HP_Manual_Override readback
// ---------------- Heat pump: unit status via HP_In (read-only) -----------
CloudTemperatureSensor hpDischTemp;   // compressor discharge temperature
CloudFloat hpSuctionPress;            // LPS1_Suction
CloudFloat hpDischPress;              // HPS1_Discharge
CloudTemperatureSensor hpEvapTemp;    // evaporator
CloudTemperatureSensor hpCondTemp;    // condenser
CloudTemperatureSensor hpSuctionLineT;// suction line
CloudFloat hpSuperheat;               // Superheat1
CloudFloat hpEevPosition;             // EEV1_Position
CloudFloat hpCompCurrent;             // Comp1_Current
CloudTemperatureSensor hpOutdoorIn;   // outdoor loop in
CloudTemperatureSensor hpOutdoorOut;  // outdoor loop out
CloudTemperatureSensor hpIndoorIn;    // indoor loop in
CloudInt   hpOperationMode;           // Operation_Mode
CloudInt   hpLimitsWord;
CloudInt   hpPermAlarms1;
CloudInt   hpPermAlarms2;
CloudInt   hpBoardFaults;
CloudInt   hpSensorFaults;
CloudBool  hpDataStale;               // HP_In frozen for HP_STALE_TIMEOUT_MS
CloudInt   hpDataAgeS;
// ---------------- Heat pump: setpoints, READ-ONLY read-back in batch 5 ----
//  Edited in Studio 5000. Registered READ on purpose: a dashboard slider
//  moves nothing and snaps back on the next 6 s read-back.
CloudFloat hpHtgSp1;
CloudFloat hpHtgSp2;
CloudFloat hpHtgDelta1;
CloudFloat hpHtgDelta2;
CloudFloat hpClgSp1;
CloudFloat hpClgSp2;
CloudFloat hpClgDelta1;
CloudFloat hpClgDelta2;
// ---------------- Heat pump: the two switches (dashboard -> PLC) ---------
CloudBool  hpEnable;                  // -> HP_Enable
CloudBool  hpModeCool;                // -> HP_Mode_CoolRequest (0 heat, 1 cool)

// ---- controls (dashboard -> PLC), all behind controlEnabled --------------
CloudBool  controlEnabled;     // master gate for ALL writes to the PLC
CloudBool  systemRun;          // ON -> Start_Button = true (start auto cycle)
CloudBool  stopButton;         // -> Stop_Button
CloudBool  resetButton;        // -> Reset_Button
CloudBool  purgeButton;        // -> Purge_Button
CloudInt   adsorpTimeMs;       // adsorption time in MINUTES (dashboard); x60000 -> Timer_3.PRE ms. clamp 5-60
CloudInt   desorpTimeMs;       // desorption time in MINUTES (dashboard); x60000 -> Timer_6[3].PRE ms. clamp 5-60

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

// ---- callbacks, defined in cloud_ctrl.h (main thread) --------------------
void onControlEnabledChange();
void onSystemRunChange();
void onStopButtonChange();
void onResetButtonChange();
void onPurgeButtonChange();
void onAdsorpTimeMsChange();
void onDesorpTimeMsChange();
void onFlowResetTotalChange();
void onHpEnableChange();
void onHpModeCoolChange();

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

  // --- valves / pumps / positions ---
  ArduinoCloud.addProperty(valveS1,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveS5,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveS6,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveS7,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveS10,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveV1A,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveV1B,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveV2A1, READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveV2A2, READ, ON_CHANGE);
  ArduinoCloud.addProperty(valveV2B,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(pumpScroll,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(pumpCond,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(pumpHotWater,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(pumpColdWater, READ, ON_CHANGE);
  ArduinoCloud.addProperty(boosterVfdRun, READ, ON_CHANGE);
  ArduinoCloud.addProperty(posS2,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posS3,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posS4,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posS8,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posS9,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posV10, READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(posV11, READ, ON_CHANGE, NULL, PUB_DELTA);

  // --- water totals read back ---
  ArduinoCloud.addProperty(cumulativeWaterVolume, READ, 30 * SECONDS);
  ArduinoCloud.addProperty(hmiWaterTotal,         READ, ON_CHANGE, NULL, PUB_DELTA);

  // --- fault flags, sweep health, heat-pump St_* ---
  ArduinoCloud.addProperty(pressError,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(tempError,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(genError,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcReadFails, READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcFailTag,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(stStage1,        READ, ON_CHANGE);
  ArduinoCloud.addProperty(stStage2,        READ, ON_CHANGE);
  ArduinoCloud.addProperty(stAuxHeat,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(stIndoorCirc,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(stIndoorFlow,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(stOutdoorFlow,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(stLockout,       READ, ON_CHANGE);
  ArduinoCloud.addProperty(stPhaseFault,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(stBACnetControl, READ, ON_CHANGE);

  // --- PLC state words and timers ---
  ArduinoCloud.addProperty(adsorpElapsedS,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(desorpElapsedT6S,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(desorpElapsedT11S, READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcActionWord,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcStateWord,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcStateText,      READ, ON_CHANGE);

  // --- water accounting ---
  ArduinoCloud.addProperty(flowRate,           READ, 5 * SECONDS);
  ArduinoCloud.addProperty(flowTotal,          READ, 10 * SECONDS);
  ArduinoCloud.addProperty(flowBatch,          READ, 10 * SECONDS);
  ArduinoCloud.addProperty(flowResetTotal,     READWRITE, ON_CHANGE, onFlowResetTotalChange);
  ArduinoCloud.addProperty(plcFlowWriteOk,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(displayWaterVolume, READ, 10 * SECONDS);
  ArduinoCloud.addProperty(tripWaterVolume,    READ, 10 * SECONDS);
  ArduinoCloud.addProperty(waterOwedL,         READ, 30 * SECONDS);
  ArduinoCloud.addProperty(hmiWriteOk,         READ, ON_CHANGE);
  ArduinoCloud.addProperty(hmiWriteCount,      READ, ON_CHANGE);
  ArduinoCloud.addProperty(plcTotalRestores,   READ, ON_CHANGE);

  // --- heat pump: PLC staging status ---
  ArduinoCloud.addProperty(hpLoopTemp,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpStage1ActivT, READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpStage2ActivT, READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpStage1Call,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpStage2Call,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpStage1Sat,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpStage2Sat,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpY1Cmd,        READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpY2Cmd,        READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpReverseCmd,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpCoolRequest,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpTempValid,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpEnableSt,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpManualOvrSt,  READ, ON_CHANGE);
  // --- heat pump: unit status via HP_In ---
  ArduinoCloud.addProperty(hpDischTemp,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpSuctionPress, READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpDischPress,   READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpEvapTemp,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpCondTemp,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpSuctionLineT, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpSuperheat,    READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpEevPosition,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpCompCurrent,  READ, ON_CHANGE, NULL, PUB_DELTA);
  ArduinoCloud.addProperty(hpOutdoorIn,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpOutdoorOut,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpIndoorIn,     READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpOperationMode, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpLimitsWord,   READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpPermAlarms1,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpPermAlarms2,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpBoardFaults,  READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpSensorFaults, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpDataStale,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpDataAgeS,     READ, 30 * SECONDS);
  // --- heat pump: setpoints, read-only read-back ---
  ArduinoCloud.addProperty(hpHtgSp1,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpHtgSp2,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpHtgDelta1, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpHtgDelta2, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpClgSp1,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpClgSp2,    READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpClgDelta1, READ, ON_CHANGE);
  ArduinoCloud.addProperty(hpClgDelta2, READ, ON_CHANGE);
  // --- heat pump: the two switches, behind controlEnabled ---
  ArduinoCloud.addProperty(hpEnable,   READWRITE, ON_CHANGE, onHpEnableChange);
  ArduinoCloud.addProperty(hpModeCool, READWRITE, ON_CHANGE, onHpModeCoolChange);

  // --- controls (dashboard -> PLC), all behind controlEnabled ---
  ArduinoCloud.addProperty(controlEnabled, READWRITE, ON_CHANGE, onControlEnabledChange);
  ArduinoCloud.addProperty(systemRun,      READWRITE, ON_CHANGE, onSystemRunChange);
  ArduinoCloud.addProperty(stopButton,     READWRITE, ON_CHANGE, onStopButtonChange);
  ArduinoCloud.addProperty(resetButton,    READWRITE, ON_CHANGE, onResetButtonChange);
  ArduinoCloud.addProperty(purgeButton,    READWRITE, ON_CHANGE, onPurgeButtonChange);
  ArduinoCloud.addProperty(adsorpTimeMs,   READWRITE, ON_CHANGE, onAdsorpTimeMsChange);
  ArduinoCloud.addProperty(desorpTimeMs,   READWRITE, ON_CHANGE, onDesorpTimeMsChange);

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
