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
//  the PLC thread and (later) the USB header can
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

//  Named slots that code other than the sweep/TAKE lists reads directly.
//  flow_watch.h checks these against the tables at start-up (strcmp) and
//  disables itself if they drift, so a reorder cannot silently watch the
//  wrong tag.
#define SSLOT_LEVEL   27      // SENSOR_TAGS[27] == TAG_LEVEL  (Collection_Tank_Level)

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

// ---------------------------------------------------------------------
//  BATCH 1: control tags, cycle timers, valve sweep, state sweep.
//  Control/timer names are verbatim from opta-plc-gateway-ip2.ino 178-221;
//  the valve/state name macros live in config.h (no #if guard).
// ---------------------------------------------------------------------
#define TAG_START_BUTTON    "Start_Button"
#define TAG_STOP_BUTTON     "Stop_Button"
#define TAG_ADSORP_TIME     "Timer_3.PRE"        // DINT, ms
#define TAG_ADSORP_ACC      "Timer_3.ACC"        // DINT, ms elapsed
#define MS_PER_MIN          60000L
#define ADSORP_TIME_MIN_MIN 5                    // lower clamp (minutes)
#define ADSORP_TIME_MAX_MIN 60                   // upper clamp (minutes)
#define TAG_DESORP_PRE_T6   "Timer_6[3].PRE"     // top chamber
#define TAG_DESORP_PRE_T11  "Timer_11[3].PRE"    // bottom chamber
#define TAG_DESORP_ACC_T6   "Timer_6[3].ACC"
#define TAG_DESORP_ACC_T11  "Timer_11[3].ACC"
#define DESORP_TIME_MIN_MIN 5                    // 2026-09-05: both clamps 5-60 (Adam)
#define DESORP_TIME_MAX_MIN 60

//  Valve / pump / position sweep. Slot order is a contract with
//  cloud_side.h's TAKE_V* list; verbatim from the old VALVE_TAGS.
static const char* const VALVE_TAGS[] = {
  TAG_V_S1,   TAG_V_S5,   TAG_V_S6,   TAG_V_S7,   TAG_V_S10,      //  0.. 4
  TAG_V_V1A,  TAG_V_V1B,  TAG_V_V2A1, TAG_V_V2A2, TAG_V_V2B,      //  5.. 9
  TAG_P_SCROLL, TAG_P_COND, TAG_P_HOTW, TAG_P_COLDW,              // 10..13
  TAG_P_BVFDRUN,                                                  // 14
  TAG_POS_S2, TAG_POS_S3, TAG_POS_S4, TAG_POS_S8, TAG_POS_S9,     // 15..19
  TAG_POS_V10, TAG_POS_V11,                                       // 20..21
  TAG_DISPLAY_WATERVOL, TAG_CUMUL_WATERVOL,                       // 22..23  read-back only in batch 1
  TAG_PRESS_ERROR, TAG_TEMP_ERROR, TAG_GEN_ERROR,                 // 24..26
  TAG_ST_STAGE1, TAG_ST_STAGE2, TAG_ST_AUXHEAT,                   // 27..29
  TAG_ST_INDOORCIRC, TAG_ST_INDOORFLOW, TAG_ST_OUTDOORFLOW,       // 30..32
  TAG_ST_LOCKOUT, TAG_ST_PHASEFAULT, TAG_ST_BACNETCTL             // 33..35
};
#define N_VALVE (sizeof(VALVE_TAGS) / sizeof(VALVE_TAGS[0]))
#define VSLOT_P_COND  11      // VALVE_TAGS[11] == TAG_P_COND  (Lefoo condensate pump)

//  State sweep: 15 Action bits then 12 button/state/fan/door bits.
//  plcActionWord bit N = Action_(N+1); plcStateWord bit order is FIXED
//  (config.h comment): Start, Stop, Reset, Purge, State_1, State_2, Fan1,
//  Fan2, TopA, TopB, BotA, BotB.
static const char* const STATE_TAGS[] = {
  TAG_ACTION_1,  TAG_ACTION_2,  TAG_ACTION_3,  TAG_ACTION_4,  TAG_ACTION_5,
  TAG_ACTION_6,  TAG_ACTION_7,  TAG_ACTION_8,  TAG_ACTION_9,  TAG_ACTION_10,
  TAG_ACTION_11, TAG_ACTION_12, TAG_ACTION_13, TAG_ACTION_14, TAG_ACTION_15,
  TAG_START_BUTTON, TAG_STOP_BUTTON, TAG_RESET_BUTTON, TAG_PURGE_BUTTON,
  TAG_STATE_1, TAG_STATE_2, TAG_FAN1, TAG_FAN2,
  TAG_IND_TOPA, TAG_IND_TOPB, TAG_IND_BOTA, TAG_IND_BOTB
};
#define N_STATE  (sizeof(STATE_TAGS) / sizeof(STATE_TAGS[0]))
#define N_ACTION 15

static_assert(N_VALVE == 36,  "cloud_side.h TAKE_V* list covers 36 valve slots");
static_assert(N_STATE == 27,  "15 actions + 12 state bits");
static_assert(N_ACTION < N_STATE, "action bits come first");

//  Trip history ring in the PLC, newest first (the HMI reads them in this
//  order). Five separate REAL tags, not an array -- the controls engineer's
//  toolchain does not take REAL arrays. Names live in config.h.
static const char* const TRIP_HIST_TAGS[TRIP_HIST_N] = {
  TAG_TRIP_HIST1, TAG_TRIP_HIST2, TAG_TRIP_HIST3, TAG_TRIP_HIST4, TAG_TRIP_HIST5
};

// ---------------------------------------------------------------------
//  BATCH 5: heat pump, THROUGH THE PLC. Names verbatim from the old
//  firmware (opta-plc-gateway-ip2.ino 237-296). The PLC drives the unit
//  (460ESBM/BACnet today, Modbus later); the gateway only reads and writes
//  these tags.
// ---------------------------------------------------------------------
//  PLC staging logic
#define TAG_HP_LOOPTEMP     "HP_LoopTemp_PV"
#define TAG_HP_ACT1         "HP_Stage1_Activation"
#define TAG_HP_ACT2         "HP_Stage2_Activation"
#define TAG_HP_CALL1        "HP_Stage1_Call"
#define TAG_HP_CALL2        "HP_Stage2_Call"
#define TAG_HP_SAT1         "HP_Stage1_Satisfied"
#define TAG_HP_SAT2         "HP_Stage2_Satisfied"
#define TAG_HP_Y1           "HP_Y1A_Cmd"
#define TAG_HP_Y2           "HP_Y2A_Cmd"
#define TAG_HP_REVERSE      "HP_Reverse_Cmd"
#define TAG_HP_COOLREQ      "HP_Mode_CoolRequest"
#define TAG_HP_TEMPVALID    "HP_TempValid"
#define TAG_HP_ENABLE       "HP_Enable"
#define TAG_HP_MANUAL       "HP_Manual_Override"
//  Unit status via the PLC's HP_In UDT (from the 460ESBM)
#define TAG_HP_DISCH_T      "HP_In.Disch_Temp"
#define TAG_HP_SUCT_P       "HP_In.LPS1_Suction"
#define TAG_HP_DISCH_P      "HP_In.HPS1_Discharge"
#define TAG_HP_EVAP_T       "HP_In.Evap1_Temp"
#define TAG_HP_COND_T       "HP_In.Cond1_Temp"
#define TAG_HP_SUCTLINE_T   "HP_In.SuctionLine1_Temp"
#define TAG_HP_SUPERHEAT    "HP_In.Superheat1"
#define TAG_HP_EEV_POS      "HP_In.EEV1_Position"
#define TAG_HP_COMP_I       "HP_In.Comp1_Current"
#define TAG_HP_OD_IN        "HP_In.Outdoor_IN"
#define TAG_HP_OD_OUT       "HP_In.Outdoor_OUT"
#define TAG_HP_ID_IN        "HP_In.Indoor_IN"
#define TAG_HP_OPMODE       "HP_In.Operation_Mode"
#define TAG_HP_LIMITS       "HP_In.Limits_Word"
#define TAG_HP_ALARM1       "HP_In.PermAlarms1"
#define TAG_HP_ALARM2       "HP_In.PermAlarms2"
#define TAG_HP_BOARDFLT     "HP_In.Board_Faults"
#define TAG_HP_SENSORFLT    "HP_In.Sensor_Faults"
//  Setpoints (read back only in batch 5; edited in Studio 5000)
#define TAG_HP_HTG_SP1      "HP_Htg_SP_S1"
#define TAG_HP_HTG_SP2      "HP_Htg_SP_S2"
#define TAG_HP_HTG_D1       "HP_Htg_Delta_S1"
#define TAG_HP_HTG_D2       "HP_Htg_Delta_S2"
#define TAG_HP_CLG_SP1      "HP_Clg_SP_S1"
#define TAG_HP_CLG_SP2      "HP_Clg_SP_S2"
#define TAG_HP_CLG_D1       "HP_Clg_Delta_S1"
#define TAG_HP_CLG_D2       "HP_Clg_Delta_S2"
#define TAG_HP_HIGHLIMIT    "HP_Htg_HighLimit"

//  Slot order is a contract with cloud_side.h's TAKE_H* list.
static const char* const HP_REAL_TAGS[] = {
  TAG_HP_LOOPTEMP, TAG_HP_ACT1, TAG_HP_ACT2,                          //  0.. 2  PLC-computed
  TAG_HP_DISCH_T, TAG_HP_SUCT_P, TAG_HP_DISCH_P,                      //  3.. 5  HP_In analog
  TAG_HP_EVAP_T, TAG_HP_COND_T, TAG_HP_SUCTLINE_T,                    //  6.. 8
  TAG_HP_SUPERHEAT, TAG_HP_EEV_POS, TAG_HP_COMP_I,                    //  9..11
  TAG_HP_OD_IN, TAG_HP_OD_OUT, TAG_HP_ID_IN,                          // 12..14
  TAG_HP_OPMODE, TAG_HP_LIMITS, TAG_HP_ALARM1,                        // 15..17  words as REAL
  TAG_HP_ALARM2, TAG_HP_BOARDFLT, TAG_HP_SENSORFLT,                   // 18..20
  TAG_HP_HTG_SP1, TAG_HP_HTG_SP2, TAG_HP_HTG_D1, TAG_HP_HTG_D2,       // 21..24  setpoints
  TAG_HP_CLG_SP1, TAG_HP_CLG_SP2, TAG_HP_CLG_D1, TAG_HP_CLG_D2        // 25..28
};
#define N_HP_REAL (sizeof(HP_REAL_TAGS) / sizeof(HP_REAL_TAGS[0]))
//  Stale detection watches only the HP_In analog slots: PLC-computed
//  values (0..2) move on setpoint edits with the link dead, and the words
//  (15..20) are legitimately constant for hours.
#define HP_STALE_FIRST 3
#define HP_STALE_LAST  14

static const char* const HP_BOOL_TAGS[] = {
  TAG_HP_CALL1, TAG_HP_CALL2, TAG_HP_SAT1, TAG_HP_SAT2,               //  0.. 3
  TAG_HP_Y1, TAG_HP_Y2, TAG_HP_REVERSE, TAG_HP_COOLREQ,               //  4.. 7
  TAG_HP_TEMPVALID, TAG_HP_ENABLE, TAG_HP_MANUAL                      //  8..10
};
#define N_HP_BOOL (sizeof(HP_BOOL_TAGS) / sizeof(HP_BOOL_TAGS[0]))

static_assert(N_HP_REAL == 29, "cloud_side.h TAKE_HR/HI list covers 29 heat-pump REAL slots");
static_assert(N_HP_BOOL == 11, "cloud_side.h TAKE_HB list covers 11 heat-pump BOOL slots");
static_assert(HP_STALE_LAST < N_HP_REAL, "stale window inside the REAL table");

#endif // PLC_TAGS_H
