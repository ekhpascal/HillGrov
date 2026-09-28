/* pcfg_pres.c -- one entry per field row of HG_FIELDS and HG_MFIELDS (test_pcfg_gen pins the coverage). */
#include <string.h>
#include "pcfg_pres.h"
#include "hg_mcfg.h"

#define ZP(g, k, lbl, u) .table = PCFG_TABLE_ZONE,   .group = HG_G_##g,  .key = k, .label = lbl, .unit = u
#define MP(g, k, lbl, u) .table = PCFG_TABLE_MASTER, .group = HG_MG_##g, .key = k, .label = lbl, .unit = u

const pcfg_pres_t PCFG_PRES[] = {
    /* ZONECFG */
    { ZP(ZONECFG, "NAME",             "Zone name", ""), .min_len = 1 },          /* validator: non-empty */
    { ZP(ZONECFG, "LINKLOSS_S",       "Link-loss timeout", "s"), .big_step = 10 },
    /* SHELF */
    { ZP(SHELF,   "CROP",             "Crop", "") },
    { ZP(SHELF,   "ENABLED",          "Shelf enabled", "") },
    { ZP(SHELF,   "PROFILE",          "Profile id (profiles not implemented yet)", "") },   /* D11 */
    /* LIGHT */
    { ZP(LIGHT,   "ON",               "Lights on", "") },
    { ZP(LIGHT,   "OFF",              "Lights off", "") },
    { ZP(LIGHT,   "WHITE",            "White", "%") },
    { ZP(LIGHT,   "RED",              "Red", "%") },
    { ZP(LIGHT,   "RAMP_MIN",         "Ramp", "min") },
    { ZP(LIGHT,   "DLI",              "Daily light target", "mol/m2/d"), .scale_div = 10, .zero_text = "off" },
    /* WATER */
    { ZP(WATER,   "MODE",             "Watering", "") },
    { ZP(WATER,   "TARGET",           "Target moisture", "%") },
    { ZP(WATER,   "HYST",             "Hysteresis", "%") },
    { ZP(WATER,   "SETTLE_MIN",       "Settle time", "min") },
    { ZP(WATER,   "DOSE_S",           "Dose", "s"), .big_step = 10 },
    { ZP(WATER,   "INTERVAL_MIN",     "Minimum interval", "min"), .big_step = 10 },
    { ZP(WATER,   "MAX_DOSES",        "Max doses", "/day") },
    { ZP(WATER,   "DIFF_MAX",         "Max sensor difference", "%") },
    { ZP(WATER,   "WIN_START",        "Window start", "") },
    { ZP(WATER,   "WIN_END",          "Window end", "") },
    /* FAN */
    { ZP(FAN,     "MODE",             "Fan", "") },
    { ZP(FAN,     "ON_MIN",           "On time", "min") },
    { ZP(FAN,     "PERIOD_MIN",       "Period", "min"), .big_step = 10 },
    /* VIB */
    { ZP(VIB,     "MODE",             "Pollination", "") },
    { ZP(VIB,     "INTENSITY",        "Intensity", "%") },
    { ZP(VIB,     "PULSE_S",          "Pulse", "s") },
    { ZP(VIB,     "INTERVAL_MIN",     "Interval", "min"), .big_step = 10 },
    { ZP(VIB,     "START",            "Start", "") },
    { ZP(VIB,     "END",              "End", "") },
    /* AUX */
    { ZP(AUX,     "MODE",             "Aux output", "") },
    { ZP(AUX,     "PULSE_S",          "Pulse", "s") },
    { ZP(AUX,     "INTERVAL_MIN",     "Interval", "min"), .big_step = 10 },
    { ZP(AUX,     "START",            "Start", "") },
    { ZP(AUX,     "END",              "End", "") },
    /* HW -- read-only hardware plane */
    { ZP(HW,      "SHELVES",          "Shelves", "") },
    { ZP(HW,      "PCA_ADDR",         "PCA9685 I2C address", ""), .fmt_hex = 1 },
    { ZP(HW,      "PCF_ADDR",         "PCF8575 I2C address", ""), .fmt_hex = 1 },
    { ZP(HW,      "SOIL_BACKEND",     "Soil sensor backend", "") },
    { ZP(HW,      "PCF_ACTLOW",       "PCF8575 active-low mask", ""), .fmt_hex = 1 },
    { ZP(HW,      "PCA_HZ",           "PWM frequency", "Hz") },
    /* HWSHELF -- read-only */
    { ZP(HWSHELF, "LED_W",            "White LED channel", "") },
    { ZP(HWSHELF, "LED_R",            "Red LED channel", "") },
    { ZP(HWSHELF, "PUMP",             "Pump pin", "") },
    { ZP(HWSHELF, "FAN",              "Fan pin", "") },
    { ZP(HWSHELF, "SOIL_A",           "Soil sensor A channel", "") },
    { ZP(HWSHELF, "SOIL_B",           "Soil sensor B channel", "") },
    { ZP(HWSHELF, "VIB",              "Vibrator channel", "") },
    { ZP(HWSHELF, "LED_MAX_W",        "White LED cap", "%") },
    { ZP(HWSHELF, "LED_MAX_R",        "Red LED cap", "%") },
    { ZP(HWSHELF, "PUMP_MAX_RUN_S",   "Pump max run", "s") },
    { ZP(HWSHELF, "PUMP_MAX_DAILY_S", "Pump max per day", "s") },
    /* CAL -- read-only */
    { ZP(CAL,     "DRY_A",            "Soil A dry", "mV") },
    { ZP(CAL,     "DRY_B",            "Soil B dry", "mV") },
    { ZP(CAL,     "WET_A",            "Soil A wet", "mV") },
    { ZP(CAL,     "WET_B",            "Soil B wet", "mV") },
    { ZP(CAL,     "MIN_OK",           "Plausible minimum", "mV") },
    { ZP(CAL,     "MAX_OK",           "Plausible maximum", "mV") },
    /* master (zone 0) -- WEB has no rows */
    { MP(WIFI,    "STA_SSID",         "Wi-Fi network (SSID)", "") },
    { MP(WIFI,    "STA_PASS",         "Wi-Fi password", ""), .max_len = 63 },           /* D12: validator max */
    { MP(WIFI,    "AP_SSID",          "Access point name", ""), .min_len = 1 },
    { MP(WIFI,    "AP_PASS",          "Access point password", ""), .max_len = 63 },    /* D12 */
    { MP(TIME,    "TZ",               "Time zone (POSIX TZ)", ""), .min_len = 1, .keyboard = PCFG_KB_TEXT_NOSPACE },
    { MP(TIME,    "NTP",              "NTP server", ""), .min_len = 1, .keyboard = PCFG_KB_TEXT_NOSPACE },
    { MP(SYS,     "HOSTNAME",         "Hostname", ""), .min_len = 1, .keyboard = PCFG_KB_HOSTNAME },
};
#undef ZP
#undef MP
const int PCFG_PRES_COUNT = (int)(sizeof PCFG_PRES / sizeof PCFG_PRES[0]);

const pcfg_pres_t *pcfg_pres_find(pcfg_table_t t, uint8_t group, const char *key) {
    if (!key) return NULL;
    for (int i = 0; i < PCFG_PRES_COUNT; i++)
        if (PCFG_PRES[i].table == (uint8_t)t && PCFG_PRES[i].group == group && strcmp(PCFG_PRES[i].key, key) == 0)
            return &PCFG_PRES[i];
    return NULL;
}
