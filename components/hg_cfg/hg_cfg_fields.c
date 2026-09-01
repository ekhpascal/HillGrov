#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "hg_cfg.h"

const char *const HG_GROUP_NAMES[HG_G_COUNT] =
    { "ZONECFG", "SHELF", "LIGHT", "WATER", "FAN", "VIB", "AUX", "HW", "HWSHELF", "CAL" };

#define F(g, k, off, t, lo, hi, e) { HG_G_##g, k, (uint16_t)(off), HG_T_##t, lo, hi, e }
const hg_field_t HG_FIELDS[] = {
    F(ZONECFG, "NAME",        offsetof(hg_zone_cfg_t, name), STR16, 0, 15, NULL),
    F(ZONECFG, "LINKLOSS_S",  6,  U16, 10, 600, NULL),
    F(SHELF,   "CROP",        0,  STR16, 0, 15, NULL),
    F(SHELF,   "ENABLED",     16, BOOL, 0, 1, NULL),
    F(SHELF,   "PROFILE",     17, U8, 0, 16, NULL),
    F(LIGHT,   "ON",          0,  HHMM, 0, 1439, NULL),
    F(LIGHT,   "OFF",         2,  HHMM, 0, 1439, NULL),
    F(LIGHT,   "WHITE",       4,  U8, 0, 100, NULL),
    F(LIGHT,   "RED",         5,  U8, 0, 100, NULL),
    F(LIGHT,   "RAMP_MIN",    6,  U8, 0, 120, NULL),
    F(LIGHT,   "DLI",         8,  U16, 0, 1000, NULL),
    F(WATER,   "MODE",        0,  ENUM, 0, 1, "OFF|AUTO"),
    F(WATER,   "TARGET",      1,  U8, 0, 100, NULL),
    F(WATER,   "HYST",        2,  U8, 1, 30, NULL),
    F(WATER,   "SETTLE_MIN",  3,  U8, 1, 60, NULL),
    F(WATER,   "DOSE_S",      4,  U16, 1, 300, NULL),
    F(WATER,   "INTERVAL_MIN",6,  U16, 10, 1440, NULL),
    F(WATER,   "MAX_DOSES",   8,  U8, 0, 24, NULL),
    F(WATER,   "DIFF_MAX",    9,  U8, 5, 50, NULL),
    F(WATER,   "WIN_START",   10, HHMM, 0, 1439, NULL),
    F(WATER,   "WIN_END",     12, HHMM, 0, 1439, NULL),
    F(FAN,     "MODE",        0,  ENUM, 0, 3, "OFF|ON|LIGHT|CYCLE"),
    F(FAN,     "ON_MIN",      1,  U8, 0, 60, NULL),
    F(FAN,     "PERIOD_MIN",  2,  U16, 0, 1440, NULL),
    F(VIB,     "MODE",        0,  ENUM, 0, 1, "OFF|PULSE"),
    F(VIB,     "INTENSITY",   1,  U8, 20, 100, NULL),
    F(VIB,     "PULSE_S",     2,  U8, 1, 30, NULL),
    F(VIB,     "INTERVAL_MIN",4,  U16, 5, 1440, NULL),
    F(VIB,     "START",       6,  HHMM, 0, 1439, NULL),
    F(VIB,     "END",         8,  HHMM, 0, 1439, NULL),
    F(AUX,     "MODE",        0,  ENUM, 0, 1, "OFF|PULSE"),
    F(AUX,     "PULSE_S",     1,  U8, 1, 30, NULL),
    F(AUX,     "INTERVAL_MIN",2,  U16, 5, 1440, NULL),
    F(AUX,     "START",       4,  HHMM, 0, 1439, NULL),
    F(AUX,     "END",         6,  HHMM, 0, 1439, NULL),
    F(HW,      "SHELVES",     0,  U8, 1, 4, NULL),
    F(HW,      "PCA_ADDR",    1,  U8, 0, 127, NULL),
    F(HW,      "PCF_ADDR",    2,  U8, 0, 127, NULL),
    F(HW,      "SOIL_BACKEND",3,  ENUM, 0, 1, "INTERNAL|ADS1115"),
    F(HW,      "PCF_ACTLOW",  4,  U16, 0, 65535, NULL),
    F(HW,      "PCA_HZ",      6,  U16, 200, 1500, NULL),
    F(HWSHELF, "LED_W",       0,  PIN, 0, 15, NULL),
    F(HWSHELF, "LED_R",       1,  PIN, 0, 15, NULL),
    F(HWSHELF, "PUMP",        2,  PIN, 0, 15, NULL),
    F(HWSHELF, "FAN",         3,  PIN, 0, 15, NULL),
    F(HWSHELF, "SOIL_A",      4,  PIN, 0, 7, NULL),
    F(HWSHELF, "SOIL_B",      5,  PIN, 0, 7, NULL),
    F(HWSHELF, "VIB",         24, PIN, 0, 15, NULL),
    F(HWSHELF, "LED_MAX_W",   6,  U8, 0, 100, NULL),
    F(HWSHELF, "LED_MAX_R",   7,  U8, 0, 100, NULL),
    F(HWSHELF, "PUMP_MAX_RUN_S",   20, U16, 1, 300, NULL),
    F(HWSHELF, "PUMP_MAX_DAILY_S", 22, U16, 1, 3600, NULL),
    F(CAL,     "DRY_A",       8,  U16, 0, 3300, NULL),
    F(CAL,     "DRY_B",       10, U16, 0, 3300, NULL),
    F(CAL,     "WET_A",       12, U16, 0, 3300, NULL),
    F(CAL,     "WET_B",       14, U16, 0, 3300, NULL),
    F(CAL,     "MIN_OK",      16, U16, 0, 3300, NULL),
    F(CAL,     "MAX_OK",      18, U16, 0, 3300, NULL),
};
#undef F
const int HG_FIELD_COUNT = (int)(sizeof HG_FIELDS / sizeof HG_FIELDS[0]);

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

int hg_group_find(const char *name) {
    for (int g = 0; g < HG_G_COUNT; g++)
        if (ci_eq(name, HG_GROUP_NAMES[g])) return g;
    return -1;
}

int hg_group_scope(uint8_t group) {
    if (group == HG_G_ZONECFG || group == HG_G_HW) return 0;
    if (group == HG_G_AUX) return 2;
    return 1;
}

static void *group_base(uint8_t group, int idx, const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg) {
    int scope = hg_group_scope(group);
    if (scope == 1 && (idx < 0 || idx >= HG_MAX_SHELVES)) return NULL;
    if (scope == 2 && (idx < 0 || idx >= HG_MAX_AUX)) return NULL;
    switch (group) {
    case HG_G_ZONECFG: return (void *)cfg;
    case HG_G_SHELF:   return (void *)&cfg->shelf[idx];
    case HG_G_LIGHT:   return (void *)&cfg->shelf[idx].light;
    case HG_G_WATER:   return (void *)&cfg->shelf[idx].water;
    case HG_G_FAN:     return (void *)&cfg->shelf[idx].fan;
    case HG_G_VIB:     return (void *)&cfg->shelf[idx].vib;
    case HG_G_AUX:     return (void *)&cfg->aux[idx];
    case HG_G_HW:      return (void *)hw;
    case HG_G_HWSHELF: return (void *)&hw->shelf[idx];
    case HG_G_CAL:     return (void *)&hw->shelf[idx];
    default:           return NULL;
    }
}

static const hg_field_t *find_row(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && ci_eq(HG_FIELDS[i].key, key)) return &HG_FIELDS[i];
    return NULL;
}

static int parse_int(const char *s, long *out) {
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

int hg_hhmm_parse(const char *s) {
    const char *colon = strchr(s, ':');
    long v;
    if (!colon) {
        if (parse_int(s, &v) != 0 || v < 0 || v > 1439) return -1;
        return (int)v;
    }
    char hbuf[4];
    size_t hl = (size_t)(colon - s);
    if (hl < 1 || hl > 2 || strlen(colon + 1) != 2) return -1;
    memcpy(hbuf, s, hl); hbuf[hl] = '\0';
    long h, m;
    if (parse_int(hbuf, &h) != 0 || parse_int(colon + 1, &m) != 0) return -1;
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return (int)(h * 60 + m);
}

void hg_hhmm_format(int minutes, char out[6]) {
    snprintf(out, 6, "%02d:%02d", minutes / 60, minutes % 60);
}

static int enum_parse(const char *enums, const char *val, long *out) {
    long idx = 0;
    const char *p = enums;
    while (*p) {
        const char *bar = strchr(p, '|');
        size_t n = bar ? (size_t)(bar - p) : strlen(p);
        char name[16];
        if (n < sizeof name) {
            memcpy(name, p, n); name[n] = '\0';
            if (ci_eq(name, val)) { *out = idx; return 0; }
        }
        if (!bar) break;
        p = bar + 1; idx++;
    }
    if (parse_int(val, out) == 0 && *out >= 0 && *out <= idx) return 0; /* numeric form */
    return -1;
}

static const char *enum_name(const char *enums, long idx, char *buf, size_t cap) {
    const char *p = enums;
    while (idx-- > 0) { p = strchr(p, '|'); if (!p) return "?"; p++; }
    const char *bar = strchr(p, '|');
    size_t n = bar ? (size_t)(bar - p) : strlen(p);
    if (n >= cap) n = cap - 1;
    memcpy(buf, p, n); buf[n] = '\0';
    return buf;
}

static int str_ok(const char *s) {
    size_t n = strlen(s);
    if (n > 15) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] <= 0x20 || s[i] > 0x7E) return 0;
    return 1;
}

int hg_field_set_text(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, uint8_t group, int idx,
                      const char *key, const char *val) {
    const hg_field_t *f = find_row(group, key);
    if (!f) return -3;
    uint8_t *base = (uint8_t *)group_base(group, idx, hw, cfg);
    if (!base) return -4;
    uint8_t *dst = base + f->offset;
    long v;
    switch (f->type) {
    case HG_T_STR16:
        if (!str_ok(val)) return -1;
        memset(dst, 0, 16);
        strcpy((char *)dst, val);
        return 0;
    case HG_T_BOOL:
        if (ci_eq(val, "ON") || ci_eq(val, "ENABLE") || ci_eq(val, "1")) v = 1;
        else if (ci_eq(val, "OFF") || ci_eq(val, "DISABLE") || ci_eq(val, "0")) v = 0;
        else return -1;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_ENUM:
        if (enum_parse(f->enums, val, &v) != 0) return -1;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_HHMM: {
        int m = hg_hhmm_parse(val);
        if (m < 0) return -1;
        uint16_t u = (uint16_t)m;
        memcpy(dst, &u, 2);
        return 0;
    }
    case HG_T_PIN:
        if (ci_eq(val, "NONE")) { *dst = HG_NONE; return 0; }
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_U8:
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        *dst = (uint8_t)v;
        return 0;
    case HG_T_U16: {
        if (parse_int(val, &v) != 0) return -1;
        if (v < f->min || v > f->max) return -2;
        uint16_t u = (uint16_t)v;
        memcpy(dst, &u, 2);
        return 0;
    }
    default:
        return -3;
    }
}

int hg_field_get_text(const hg_zone_hw_t *hw, const hg_zone_cfg_t *cfg, uint8_t group, int idx,
                      const char *key, char *out, size_t cap) {
    const hg_field_t *f = find_row(group, key);
    if (!f) return -3;
    const uint8_t *base = (const uint8_t *)group_base(group, idx, hw, cfg);
    if (!base) return -4;
    const uint8_t *src = base + f->offset;
    uint16_t u16;
    switch (f->type) {
    case HG_T_STR16: snprintf(out, cap, "%s", (const char *)src); return 0;
    case HG_T_BOOL:  snprintf(out, cap, "%u", *src); return 0;
    case HG_T_ENUM: { char b[16]; snprintf(out, cap, "%s", enum_name(f->enums, *src, b, sizeof b)); return 0; }
    case HG_T_HHMM: { char tmp[6]; memcpy(&u16, src, 2); hg_hhmm_format(u16, tmp); snprintf(out, cap, "%s", tmp); return 0; }
    case HG_T_PIN:
        if (*src == HG_NONE) snprintf(out, cap, "NONE");
        else snprintf(out, cap, "%u", *src);
        return 0;
    case HG_T_U8:    snprintf(out, cap, "%u", *src); return 0;
    case HG_T_U16:   memcpy(&u16, src, 2); snprintf(out, cap, "%u", u16); return 0;
    default:         return -3;
    }
}
