#include <stdio.h>
#include <string.h>
#include "hg_json.h"
#include "node_mgr.h"
#include "psvc_zcfg.h"

psvc_rc_t psvc_zone_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw, uint32_t *gen, int *hw_present) {
    if (zone < 1 || zone > HG_MAX_ZONES || !cfg) return PSVC_E_ZONE_UNKNOWN;
    uint32_t g = 0;
    int hp = 0;
    if (node_mgr_cfg_get(zone, cfg, hw, &g, &hp) != 0) return PSVC_E_NO_CACHE;
    if (hw && !hp) memset(hw, 0, sizeof *hw);
    if (gen) *gen = g;
    if (hw_present) *hw_present = hp;
    return PSVC_OK;
}

int psvc_zone_cfg_busy(uint8_t zone) { return node_mgr_cfg_busy(zone); }

/* node_mgr_cfg_get treats the HW plane as best-effort -- when it is absent or
 * its envelope will not unwrap it zeroes the struct and returns 0, signalling
 * the absence through hw_present. Handing hg_cfg_validate that all-zero
 * profile rejected EVERY save in the first seconds after a zone reboot (the
 * default dose_s=20 compared against pump_max_run_s=0). The first fix keyed on
 * hw_gen, which is structurally always 0 (the HW plane carries no generation
 * on the wire), so it passed NULL on every save and silently disabled the pump
 * limits -- a permanent false accept on a flood guard. Presence comes from the
 * cache's own validity, never a gen. (Moved verbatim from http_api_cfg.c's
 * cfg_put_zone, panel plan Task 5.) */
psvc_rc_t psvc_zone_cfg_edit(uint8_t zone, psvc_zcfg_fn fn, void *ctx,
                             char *err, size_t errcap, char *warn, size_t warncap) {
    char err_scratch[2], warn_scratch[2];
    if (!err || errcap == 0)   { err = err_scratch;   errcap = sizeof err_scratch; }
    if (!warn || warncap == 0) { warn = warn_scratch; warncap = sizeof warn_scratch; }
    err[0] = '\0';
    warn[0] = '\0';
    if (zone < 1 || zone > HG_MAX_ZONES) return PSVC_E_ZONE_UNKNOWN;
    if (!fn) return PSVC_E_INTERNAL;
    if (node_mgr_cfg_busy(zone)) return PSVC_E_BUSY;

    hg_zone_cfg_t cfg;
    hg_zone_hw_t  hw;
    int           hw_present = 0;
    if (node_mgr_cfg_get(zone, &cfg, &hw, NULL, &hw_present) != 0) return PSVC_E_NO_CACHE;
    const hg_zone_hw_t *hwp = hw_present ? &hw : NULL;

    int frc = fn(&cfg, hwp, ctx, err, errcap, warn, warncap);
    if (frc == PSVC_EDIT_BAD_JSON)      return PSVC_E_BAD_JSON;
    if (frc == PSVC_EDIT_INVALID_FIELD) return PSVC_E_INVALID_FIELD;
    if (frc == PSVC_EDIT_VALIDATION)    return PSVC_E_VALIDATION;
    if (frc != 0)                       return PSVC_E_INTERNAL;

    if (hg_cfg_validate(&cfg, hwp, err, errcap) != 0) return PSVC_E_VALIDATION;

    switch (node_mgr_cfg_set(zone, &cfg)) {
    case 0:  return PSVC_OK;
    case -1: return PSVC_E_ZONE_UNKNOWN;
    case -2: return PSVC_E_BUSY;
    case -3: return PSVC_E_ZONE_NOT_ONLINE;
    default: return PSVC_E_INTERNAL;
    }
}

int psvc_zone_json_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                      char *err, size_t errcap, char *warn, size_t warncap) {
    int rc = hg_json_merge_cfg(hw_or_null, cfg, (const char *)ctx, err, errcap, warn, warncap);
    switch (rc) {
    case 0:  return 0;
    case -1: return PSVC_EDIT_BAD_JSON;
    case -2: return PSVC_EDIT_INVALID_FIELD;
    default: return PSVC_EDIT_VALIDATION;
    }
}

static int is_hw_group(uint8_t g) { return g == HG_G_HW || g == HG_G_HWSHELF || g == HG_G_CAL; }

static void cfg_path(char *err, size_t errcap, const hg_field_t *f, int idx) {
    if (!err || !errcap) return;
    int scope = hg_group_scope(f->group);
    if (scope == 1)      snprintf(err, errcap, "cfg.shelf[%d].%s.%s", idx, HG_GROUP_NAMES[f->group], f->key);
    else if (scope == 2) snprintf(err, errcap, "cfg.aux[%d].%s.%s", idx, HG_GROUP_NAMES[f->group], f->key);
    else                 snprintf(err, errcap, "cfg.%s.%s", HG_GROUP_NAMES[f->group], f->key);
}

int psvc_zone_fields_fn(hg_zone_cfg_t *cfg, const hg_zone_hw_t *hw_or_null, void *ctx,
                        char *err, size_t errcap, char *warn, size_t warncap) {
    (void)hw_or_null;   /* the validator after this fn is what uses hw -- a field write never does */
    (void)warn;
    (void)warncap;
    const psvc_fedits_t *set = (const psvc_fedits_t *)ctx;
    if (!set || (set->n > 0 && !set->e)) return PSVC_EDIT_INVALID_FIELD;
    for (int i = 0; i < set->n; i++) {
        const psvc_fedit_t *e = &set->e[i];
        const hg_field_t *f = e->f;
        if (!f || f->group >= HG_G_COUNT) {
            if (err && errcap) snprintf(err, errcap, "cfg.?");
            return PSVC_EDIT_INVALID_FIELD;
        }
        if (is_hw_group(f->group)) {
            if (err && errcap) snprintf(err, errcap, "hw.%s.%s", HG_GROUP_NAMES[f->group], f->key);
            return PSVC_EDIT_INVALID_FIELD;
        }
        int idx = hg_group_scope(f->group) == 0 ? -1 : e->idx;
        void *base = hg_field_base(f->group, idx, NULL, cfg);
        char text[PSVC_FEDIT_TEXT_MAX];
        memcpy(text, e->text, sizeof text);
        text[sizeof text - 1] = '\0';
        if (!base || hg_field_write(f, base, text) != 0) {
            cfg_path(err, errcap, f, idx);
            return PSVC_EDIT_INVALID_FIELD;
        }
    }
    return 0;
}
