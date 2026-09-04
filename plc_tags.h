#ifndef PLC_TAGS_H
#define PLC_TAGS_H
#include <stdint.h>
#include "config.h"

// ---------------------------------------------------------------------
//  TAG NAMES AND DINT SCALING. Copied verbatim from
//  opta-plc-gateway-ip2/opta-plc-gateway-ip2.ino lines 85-172. These lived
//  in the sketch, not config.h; they belong with the tables that use them.
// ---------------------------------------------------------------------
//  your controller program. Control tags are EXAMPLES -- replace the right
//  side with the real writable tag names from your Studio 5000 program.
// =====================================================================
// -- sensors (read) --
#define TAG_T1   "Temp_1"
#define TAG_T2   "Temp_2"
#define TAG_T3   "Temp_3"
#define TAG_T4   "Temp_4"
#define TAG_T5   "Temp_5"
#define TAG_T6   "Temp_6"
#define TAG_T7   "Temp_7"
#define TAG_T8   "Temp_8"
#define TAG_T9   "Temp_9"
#define TAG_T10  "Temp_10"
#define TAG_T11  "Temp_11"
#define TAG_T12  "Temp_12"
#define TAG_VT   "Vaisala_Conden_Temp"

#define TAG_P1   "Press_1"
#define TAG_P2   "Press_2"
#define TAG_P3   "Press_3"
#define TAG_P4   "Press_4"
#define TAG_P5   "Press_5"
#define TAG_P6   "Vaisala_Conden_WVP"

// IP2 CHANGE: heat-pump module pressures are DINT raw counts named Raw_*,
// not the scaled REAL tags IP1 had. Read via readDint() + scaled below.
#define TAG_HP1H "Raw_Module_1_Pressure_Grey"    // DINT (IP1: Module_1_Pressure_Grey, REAL)
#define TAG_HP1C "Raw_Module_1_Pressure_White"   // DINT (IP1: Module_1_Pressure_White, REAL)
#define TAG_HP2H "Raw_Module_2_Pressure_Grey"    // DINT (IP1: Module_2_Pressure_Grey, REAL)
#define TAG_HP2C "Raw_Module_2_Pressure_White"   // DINT (IP1: Module_2_Pressure_White, REAL)

#define TAG_ATM_T    "Atmosphere_Temp"
#define TAG_ATM_RH   "Atmosphere_Humidity"
// IP2 CHANGE: these two are DINT on IP2 (they were REAL on IP1).
#define TAG_ATM_T2   "Atmosphere_Temp_02"        // DINT -> cloud atmTempDs
#define TAG_ATM_RH2  "Atmosphere_Humidity_02"    // DINT -> cloud atmRhDs
#define TAG_LEVEL    "Collection_Tank_Level"     // ALIAS -> Local:4:I.Ch05.Data (REAL)

// =====================================================================
//  IP2 DINT -> ENGINEERING UNIT SCALING
//
//  IP2 exposes 6 of the 28 sensor points as DINT instead of REAL. Whether a
//  DINT needs scaling had to be settled PER CHANNEL, so there are two flags
//  below rather than one: the ambient pair is confirmed, the heat-pump
//  pressures are not.
//
//  Verified 2026-08-17 by reading the live controller (192.168.102.21) from
//  the PC with pycomm3, independently of this gateway:
//
//    Atmosphere_Temp        (REAL) = 80.0    <- IMPOSSIBLE (80 degC ambient)
//    Atmosphere_Humidity    (REAL) = 120.0   <- IMPOSSIBLE (120 %RH)
//    Atmosphere_Temp_02     (DINT) = 29      <- plausible degC
//    Atmosphere_Humidity_02 (DINT) = 56      <- plausible %RH
//    Raw_Temp               (REAL) = 12.0  } BOTH exactly 12.0, the 4-20 mA
//    Raw_Humidity           (REAL) = 12.0  } midpoint -> those inputs float
//
//  This file used to assume "the REAL tags are the trustworthy engineering
//  values, the DINTs are raw counts needing span/offset". For the ambient
//  pair that is BACKWARDS. The DINTs already hold whole-degree engineering
//  units; it is the PLC-side scaled REALs that are garbage, because their
//  4-20 mA inputs are unwired and the PLC faithfully scales the floating
//  midpoint into 80.0 / 120.0.
//
//  Consequence for the cloud: atmTemp / atmRh (fed by the REAL tags) publish
//  80 degC / 120 %RH until those inputs get wired. atmTempDs / atmRhDs (fed
//  by the DINTs) are the trustworthy ambient readings for now.
// =====================================================================

//  Ambient pair -- CONFIRMED: the DINT already IS the engineering value.
#define IP2_ATM_SCALING_CONFIRMED 1
#define SCALE_ATM_T2         1.0f
#define OFFSET_ATM_T2        0.0f
#define SCALE_ATM_RH2        1.0f
#define OFFSET_ATM_RH2       0.0f

//  Heat-pump module pressures -- STILL UNCONFIRMED.
//  All four Raw_Module_*_Pressure_* read 0 because the heat pump is idle, and
//  at 0 you cannot separate span from offset. Identity here is a placeholder,
//  NOT a finding -- do not read the 1.0f/0.0f as agreement with the ambient
//  result above. Re-measure with the heat pump RUNNING, compare against the
//  gauge, then fill in and set the flag.
//  Cross-check against IP1 when you do: the same physical quantity must land
//  on the same engineering scale, or InfluxDB history for IP1 vs IP2 is not
//  comparable.
#define IP2_HP_PRESSURE_SCALING_CONFIRMED 0
#define SCALE_HP_PRESSURE    1.0f
#define OFFSET_HP_PRESSURE   0.0f

// ---------------------------------------------------------------------
//  The 28 sensor tags in canonical slot order. Moved here from the .ino so
//  the PLC thread and (later) the AI feature map and the USB header can
//  share one definition. The order is load-bearing: PlcSnapshot::sensor[]
//  is indexed by these slot numbers and cloud_side.h assigns Cloud*
//  properties from them in this order. DO NOT reorder or resize without
//  updating shared.h and cloud_side.h.
// ---------------------------------------------------------------------
static const char* const SENSOR_TAGS[] = {
  TAG_T1, TAG_T2, TAG_T3, TAG_T4, TAG_T5, TAG_T6, TAG_T7, TAG_T8,   //  0.. 7
  TAG_T9, TAG_T10, TAG_T11, TAG_T12, TAG_VT,                        //  8..12
  TAG_P1, TAG_P2, TAG_P3, TAG_P4, TAG_P5, TAG_P6,                   // 13..18
  TAG_HP1H, TAG_HP1C, TAG_HP2H, TAG_HP2C,                           // 19..22  DINT on IP2
  TAG_ATM_T, TAG_ATM_RH, TAG_ATM_T2, TAG_ATM_RH2, TAG_LEVEL         // 23..27  (25,26 DINT)
};
#define N_SENSORS (sizeof(SENSOR_TAGS) / sizeof(SENSOR_TAGS[0]))

// ---------------------------------------------------------------------
//  IP2 sweep is split: 22 REAL points in one batched MSP call, 6 DINT raw
//  counts read singly and scaled. Results are scattered back into their
//  canonical slots so everything downstream sees the IP1 layout.
// ---------------------------------------------------------------------
static const char* const REAL_TAGS[] = {
  TAG_T1, TAG_T2, TAG_T3, TAG_T4, TAG_T5, TAG_T6, TAG_T7, TAG_T8,
  TAG_T9, TAG_T10, TAG_T11, TAG_T12, TAG_VT,
  TAG_P1, TAG_P2, TAG_P3, TAG_P4, TAG_P5, TAG_P6,
  TAG_ATM_T, TAG_ATM_RH, TAG_LEVEL
};
static const uint8_t REAL_SLOT[] = {
  0, 1, 2, 3, 4, 5, 6, 7,
  8, 9, 10, 11, 12,
  13, 14, 15, 16, 17, 18,
  23, 24, 27
};
#define N_REAL (sizeof(REAL_TAGS) / sizeof(REAL_TAGS[0]))

static const char* const DINT_TAGS[] = {
  TAG_HP1H, TAG_HP1C, TAG_HP2H, TAG_HP2C, TAG_ATM_T2, TAG_ATM_RH2
};
static const uint8_t DINT_SLOT[]  = { 19, 20, 21, 22, 25, 26 };
static const float   DINT_SCALE[] = {
  SCALE_HP_PRESSURE, SCALE_HP_PRESSURE, SCALE_HP_PRESSURE, SCALE_HP_PRESSURE,
  SCALE_ATM_T2, SCALE_ATM_RH2
};
static const float   DINT_OFFSET[] = {
  OFFSET_HP_PRESSURE, OFFSET_HP_PRESSURE, OFFSET_HP_PRESSURE, OFFSET_HP_PRESSURE,
  OFFSET_ATM_T2, OFFSET_ATM_RH2
};
#define N_DINT (sizeof(DINT_TAGS) / sizeof(DINT_TAGS[0]))

static_assert(N_SENSORS == 28, "batch 0 snapshot assumes 28 sensor slots");
static_assert(N_REAL == 22,    "22 REAL tags on IP2");
static_assert(N_DINT == 6,     "6 DINT tags on IP2");
static_assert(N_REAL + N_DINT == N_SENSORS,
              "REAL_TAGS + DINT_TAGS must cover every SENSOR_TAGS slot exactly once");
static_assert(sizeof(REAL_SLOT) / sizeof(REAL_SLOT[0]) == N_REAL,
              "REAL_SLOT length must match REAL_TAGS");
static_assert(sizeof(DINT_SLOT) / sizeof(DINT_SLOT[0]) == N_DINT,
              "DINT_SLOT length must match DINT_TAGS");

#endif // PLC_TAGS_H
