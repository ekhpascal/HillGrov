#include <stdio.h>
#include <string.h>
#include "hg_cfg.h"

/* err path: "<scopePrefix><group>.<key>" lowercased, e.g. "shelf[2].water.dose_s" */
static int fail(char *err, size_t n, int shelf, uint8_t group, const char *key) {
    char lg[12], lk[24];
    size_t i;
    for (i = 0; HG_GROUP_NAMES[group][i] && i < 11; i++) {
        char c = HG_GROUP_NAMES[group][i];
        lg[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    lg[i] = '\0';
    for (i = 0; key[i] && i < 23; i++) {
        char c = key[i];
        lk[i] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    }
    lk[i] = '\0';
    if (shelf >= 0) {
        if (group == HG_G_SHELF) {
            snprintf(err, n, "shelf[%d].%s", shelf, lk);
        } else {
            snprintf(err, n, "shelf[%d].%s.%s", shelf, lg, lk);
        }
    } else if (group == HG_G_ZONECFG || group == HG_G_HW) snprintf(err, n, "%s.%s", lg, lk);
    else snprintf(err, n, "%s.%s", lg, lk);
    return -1;
}

static int str16_ok(const char *s) {
    for (int i = 0; i < 16; i++) {
        if (s[i] == '\0') return 1;
        if (s[i] < 0x20 || s[i] > 0x7E) return 0;
    }
    return 0;
}

static long field_raw(const void *base, const hg_field_t *f) {
    const uint8_t *p = (const uint8_t *)base + f->offset;
    if (f->type == HG_T_U16 || f->type == HG_T_HHMM) {
        uint16_t u; memcpy(&u, p, 2); return (long)u;
    }
    return (long)*p;
}

/* range-check every table row of `group` against `base`; PIN allows HG_NONE */
static int check_group_rows(const void *base, uint8_t group, int shelf, char *err, size_t n) {
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        if (f->group != group || f->type == HG_T_STR16) continue;
        long v = field_raw(base, f);
        if (f->type == HG_T_PIN && v == HG_NONE) continue;
        long hi = (f->type == HG_T_HHMM) ? 1439 : f->max;
        long lo = (f->type == HG_T_HHMM) ? 0 : f->min;
        if (v < lo || v > hi) return fail(err, n, shelf, group, f->key);
    }
    return 0;
}

static int mark_dup(uint8_t used[16], uint8_t pin) {
    if (pin == HG_NONE) return 0;
    if (pin > 15 || used[pin]) return -1;
    used[pin] = 1;
    return 0;
}

int hg_hw_validate(const hg_zone_hw_t *hw, char *err, size_t n) {
    if (check_group_rows(hw, HG_G_HW, -1, err, n) != 0) return -1;
    uint8_t pca[16] = {0}, pcf[16] = {0}, soil[8] = {0};
    for (int s = 0; s < hw->shelf_count && s < HG_MAX_SHELVES; s++) {
        const hg_shelf_hw_t *sh = &hw->shelf[s];
        if (check_group_rows(sh, HG_G_HWSHELF, s, err, n) != 0) return -1;
        if (check_group_rows(sh, HG_G_CAL, s, err, n) != 0) return -1;
        if (mark_dup(pca, sh->led_ch[0]) != 0) return fail(err, n, s, HG_G_HWSHELF, "LED_W");
        if (mark_dup(pca, sh->led_ch[1]) != 0) return fail(err, n, s, HG_G_HWSHELF, "LED_R");
        if (mark_dup(pca, sh->vib_ch) != 0) return fail(err, n, s, HG_G_HWSHELF, "VIB");
        if (mark_dup(pcf, sh->pump_pin) != 0) return fail(err, n, s, HG_G_HWSHELF, "PUMP");
        if (mark_dup(pcf, sh->fan_pin) != 0) return fail(err, n, s, HG_G_HWSHELF, "FAN");
        if (sh->soil_ch[0] != HG_NONE && (sh->soil_ch[0] > 7 || soil[sh->soil_ch[0]]++))
            return fail(err, n, s, HG_G_HWSHELF, "SOIL_A");
        if (sh->soil_ch[1] != HG_NONE && (sh->soil_ch[1] > 7 || soil[sh->soil_ch[1]]++))
            return fail(err, n, s, HG_G_HWSHELF, "SOIL_B");
        if (sh->soil_min_ok_mv >= sh->soil_max_ok_mv) return fail(err, n, s, HG_G_CAL, "MIN_OK");
        for (int p = 0; p < 2; p++)
            if (sh->soil_dry_mv[p] <= sh->soil_wet_mv[p])
                return fail(err, n, s, HG_G_CAL, p == 0 ? "DRY_A" : "DRY_B");
    }
    for (int a = 0; a < HG_MAX_AUX; a++)
        if (hw->aux[a].type != 0 && mark_dup(pcf, hw->aux[a].pin) != 0)
            return fail(err, n, -1, HG_G_HW, "AUX_PIN");
    return 0;
}

int hg_cfg_validate(const hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw, char *err, size_t n) {
    if (!str16_ok(cfg->name) || cfg->name[0] == '\0') return fail(err, n, -1, HG_G_ZONECFG, "NAME");
    if (check_group_rows(cfg, HG_G_ZONECFG, -1, err, n) != 0) return -1;
    for (int a = 0; a < HG_MAX_AUX; a++)
        if (cfg->aux[a].mode != 0 && check_group_rows(&cfg->aux[a], HG_G_AUX, -1, err, n) != 0) return -1;
    for (int s = 0; s < HG_MAX_SHELVES; s++) {
        const hg_shelf_cfg_t *sc = &cfg->shelf[s];
        if (!str16_ok(sc->crop)) return fail(err, n, s, HG_G_SHELF, "CROP");
        if (check_group_rows(sc, HG_G_SHELF, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->light, HG_G_LIGHT, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->water, HG_G_WATER, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->fan, HG_G_FAN, s, err, n) != 0) return -1;
        if (check_group_rows(&sc->vib, HG_G_VIB, s, err, n) != 0) return -1;
        if (sc->light.on_min == sc->light.off_min) return fail(err, n, s, HG_G_LIGHT, "OFF");
        if (hw) {
            if (sc->enabled && s >= hw->shelf_count) return fail(err, n, s, HG_G_SHELF, "ENABLED");
            const hg_shelf_hw_t *sh = &hw->shelf[s];
            if (sc->water.dose_s > sh->pump_max_run_s) return fail(err, n, s, HG_G_WATER, "DOSE_S");
            if ((uint32_t)sc->water.max_doses_day * sc->water.dose_s > sh->pump_max_daily_s)
                return fail(err, n, s, HG_G_WATER, "MAX_DOSES");
        }
    }
    return 0;
}
