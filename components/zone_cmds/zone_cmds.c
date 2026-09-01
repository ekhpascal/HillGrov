#include <stdio.h>
#include <string.h>
#include "cmd_core.h"
#include "hg_cfg.h"
#include "hg_model.h"
#include "zone_cmds.h"

/* ---- shared edit plumbing: hg_field_set_text() run inside hg_model_edit() ---- */

struct edit_arg { uint8_t group; int idx; const char *key; const char *value; int rc; };

static int ci_starts(const char *s, const char *pfx) {
    for (; *pfx; s++, pfx++) {
        char a = *s, b = *pfx;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* HW/HWSHELF map keys change the pin/address map -> restart pending; the
 * safety-cap and calibration keys persist as hw but apply live. */
static uint32_t hw_mask_for(uint8_t group, const char *key) {
    if (group == HG_G_CAL) return HG_CH_HW_LIVE;
    if (group == HG_G_HWSHELF && (ci_starts(key, "LED_MAX_") || ci_starts(key, "PUMP_MAX_")))
        return HG_CH_HW_LIVE;
    if (group == HG_G_HW || group == HG_G_HWSHELF) return HG_CH_HW;
    return HG_CH_CFG;
}

static uint32_t do_edit(hg_zone_hw_t *hw, hg_zone_cfg_t *cfg, void *argp) {
    struct edit_arg *a = argp;
    a->rc = hg_field_set_text(hw, cfg, a->group, a->idx, a->key, a->value);
    if (a->rc != 0) return 0;                 /* rejected by set_text: no change, handler maps rc */
    return hw_mask_for(a->group, a->key);
}

static int field_err_token(int rc, char *r, int l) {
    switch (rc) {
    case -1: return cmd_err(r, l, "BAD_ARGS");
    case -2: case -4: return cmd_err(r, l, "OUT_OF_RANGE");
    case -3: return cmd_err(r, l, "INVALID_FIELD");
    default: return cmd_err(r, l, "INTERNAL");
    }
}

static const hg_field_t *find_field(uint8_t group, const char *key) {
    for (int i = 0; i < HG_FIELD_COUNT; i++)
        if (HG_FIELDS[i].group == group && cmd_ci_eq(HG_FIELDS[i].key, key)) return &HG_FIELDS[i];
    return NULL;
}

/* group name [idx] KEY <canonical-value>, idx 1-based when the group is scoped */
static int set_and_reply(uint8_t group, int idx, int scoped, const char *key, const char *value,
                          char *r, int l) {
    struct edit_arg ea = { group, idx, key, value, 0 };
    char path[48] = "";
    int mrc = hg_model_edit(do_edit, &ea, path, sizeof path);
    if (mrc == -1) return cmd_err(r, l, "BUSY");
    if (mrc == -2) { cmd_err(r, l, "INVALID_FIELD"); cmd_linef(r, l, "  Field : %s", path); return -1; }
    if (ea.rc != 0) return field_err_token(ea.rc, r, l);

    const hg_field_t *f = find_field(group, key);
    hg_zone_hw_t hw; hg_zone_cfg_t cfg;
    hg_model_snapshot_hw(&hw);
    hg_model_snapshot_cfg(&cfg, NULL);
    char val[24];
    hg_field_get_text(&hw, &cfg, group, idx, f->key, val, sizeof val);
    if (scoped) return cmd_okf(r, l, "%s %d %s %s", HG_GROUP_NAMES[group], idx + 1, f->key, val);
    return cmd_okf(r, l, "%s %s %s", HG_GROUP_NAMES[group], f->key, val);
}

/* Title-cases a canonical (upper-case, '_'-separated) field key for the GET dump. */
static void key_title(const char *key, char *out, size_t cap) {
    size_t i = 0;
    for (; key[i] && i + 1 < cap; i++) {
        char c = key[i];
        out[i] = (i == 0) ? (char)((c >= 'a' && c <= 'z') ? c - 32 : c)
                           : (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[i] = '\0';
}

/* ---- generic field grammar: SET|GET <GROUP> [idx] <key> <value> ---- */

static int h_field_get(cmd_req_t *q, char *r, int l) {
    int group = hg_group_find(q->e->noun1);
    if (group < 0) return cmd_err(r, l, "INTERNAL");
    int scoped = hg_group_scope((uint8_t)group) != 0;
    int idx = scoped ? (int)q->val[0] - 1 : 0;

    hg_zone_hw_t hw; hg_zone_cfg_t cfg;
    hg_model_snapshot_hw(&hw);
    hg_model_snapshot_cfg(&cfg, NULL);
    if (scoped) cmd_okf(r, l, "%s %d", HG_GROUP_NAMES[group], idx + 1);
    else        cmd_okf(r, l, "%s", HG_GROUP_NAMES[group]);

    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        if (f->group != (uint8_t)group) continue;
        char val[24], title[24];
        hg_field_get_text(&hw, &cfg, (uint8_t)group, idx, f->key, val, sizeof val);
        key_title(f->key, title, sizeof title);
        cmd_linef(r, l, "  %s : %s", title, val);
    }
    return 0;
}

static int h_field_set(cmd_req_t *q, char *r, int l) {
    int group = hg_group_find(q->e->noun1);
    if (group < 0) return cmd_err(r, l, "INTERNAL");
    int scoped = hg_group_scope((uint8_t)group) != 0;
    if (!scoped) return set_and_reply((uint8_t)group, 0, 0, q->tok[0], q->tok[1], r, l);
    int idx = (int)q->val[0] - 1;
    return set_and_reply((uint8_t)group, idx, 1, q->tok[1], q->tok[2], r, l);
}

static int h_field(cmd_req_t *q, char *r, int l) {
    return q->verb == CMDV_GET ? h_field_get(q, r, l) : h_field_set(q, r, l);
}

/* ---- SET SHELVES <n> -- alias for SET HW SHELVES ---- */

static int h_shelves(cmd_req_t *q, char *r, int l) {
    char valtxt[8];
    snprintf(valtxt, sizeof valtxt, "%d", (int)q->val[0]);
    return set_and_reply(HG_G_HW, 0, 0, "SHELVES", valtxt, r, l);
}

/* ---- SET CAL <s> <A|B> <DRY|WET> <mv> / GET CAL <s> -- two enums build the key ---- */

static const char *const CAL_SIDE[]  = { "A", "B" };
static const char *const CAL_POINT[] = { "DRY", "WET" };

static int h_cal_set(cmd_req_t *q, char *r, int l) {
    int idx = (int)q->val[0] - 1;
    const char *side = CAL_SIDE[q->val[1]];
    const char *point = CAL_POINT[q->val[2]];
    char key[8];
    snprintf(key, sizeof key, "%s_%s", point, side);   /* DRY_A, WET_B, ... */

    struct edit_arg ea = { HG_G_CAL, idx, key, q->tok[3], 0 };
    char path[48] = "";
    int mrc = hg_model_edit(do_edit, &ea, path, sizeof path);
    if (mrc == -1) return cmd_err(r, l, "BUSY");
    if (mrc == -2) { cmd_err(r, l, "INVALID_FIELD"); cmd_linef(r, l, "  Field : %s", path); return -1; }
    if (ea.rc != 0) return field_err_token(ea.rc, r, l);

    hg_zone_hw_t hw; hg_zone_cfg_t cfg;
    hg_model_snapshot_hw(&hw);
    hg_model_snapshot_cfg(&cfg, NULL);
    char val[24];
    hg_field_get_text(&hw, &cfg, HG_G_CAL, idx, key, val, sizeof val);
    return cmd_okf(r, l, "CAL %d %s %s %s", idx + 1, side, point, val);
}

static int h_cal(cmd_req_t *q, char *r, int l) {
    return q->verb == CMDV_GET ? h_field_get(q, r, l) : h_cal_set(q, r, l);
}

/* ---- GET CONFIG -- replayable dump of every field that differs from default ---- */

static int cal_dump_line(int idx, const char *key, const char *val, char *r, int l) {
    if      (cmd_ci_eq(key, "DRY_A")) return cmd_linef(r, l, "  SET CAL %d A DRY %s", idx + 1, val);
    else if (cmd_ci_eq(key, "DRY_B")) return cmd_linef(r, l, "  SET CAL %d B DRY %s", idx + 1, val);
    else if (cmd_ci_eq(key, "WET_A")) return cmd_linef(r, l, "  SET CAL %d A WET %s", idx + 1, val);
    else if (cmd_ci_eq(key, "WET_B")) return cmd_linef(r, l, "  SET CAL %d B WET %s", idx + 1, val);
    return cmd_linef(r, l, "  SET CAL %d %s %s", idx + 1, key, val); /* MIN_OK/MAX_OK: no SET row */
}

static void dump_line(uint8_t group, int idx, const char *key, const char *val, char *r, int l) {
    if (group == HG_G_CAL) { cal_dump_line(idx, key, val, r, l); return; }
    if (hg_group_scope(group) == 0) cmd_linef(r, l, "  SET %s %s %s", HG_GROUP_NAMES[group], key, val);
    else cmd_linef(r, l, "  SET %s %d %s %s", HG_GROUP_NAMES[group], idx + 1, key, val);
}

static int h_config_dump(cmd_req_t *q, char *r, int l) {
    (void)q;
    hg_zone_hw_t hw, dhw; hg_zone_cfg_t cfg, dcfg;
    hg_model_snapshot_hw(&hw);
    hg_model_snapshot_cfg(&cfg, NULL);
    hg_defaults_hw(&dhw);
    hg_defaults_cfg(&dcfg);

    int n = 0;
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        int scope = hg_group_scope(f->group);
        int hi = (scope == 1) ? HG_MAX_SHELVES - 1 : (scope == 2) ? HG_MAX_AUX - 1 : 0;
        for (int idx = 0; idx <= hi; idx++) {
            char a[24], b[24];
            hg_field_get_text(&hw, &cfg, f->group, idx, f->key, a, sizeof a);
            hg_field_get_text(&dhw, &dcfg, f->group, idx, f->key, b, sizeof b);
            if (strcmp(a, b) != 0) n++;
        }
    }
    cmd_okf(r, l, "CONFIG %d", n);
    for (int i = 0; i < HG_FIELD_COUNT; i++) {
        const hg_field_t *f = &HG_FIELDS[i];
        int scope = hg_group_scope(f->group);
        int hi = (scope == 1) ? HG_MAX_SHELVES - 1 : (scope == 2) ? HG_MAX_AUX - 1 : 0;
        for (int idx = 0; idx <= hi; idx++) {
            char a[24], b[24];
            hg_field_get_text(&hw, &cfg, f->group, idx, f->key, a, sizeof a);
            hg_field_get_text(&dhw, &dcfg, f->group, idx, f->key, b, sizeof b);
            if (strcmp(a, b) == 0) continue;
            dump_line(f->group, idx, f->key, a, r, l);
        }
    }
    return 0;
}

/* ---- row table ---- */

static const cmd_arg_t A_KV[] = {
    { "key",   ARG_STR, 0, 20, NULL },
    { "value", ARG_STR, 0, 20, NULL },
};
static const cmd_arg_t A_SHELF_KV[] = {
    { "shelf", ARG_INT, 1, HG_MAX_SHELVES, NULL },
    { "key",   ARG_STR, 0, 20, NULL },
    { "value", ARG_STR, 0, 20, NULL },
};
static const cmd_arg_t A_AUX_KV[] = {
    { "aux",   ARG_INT, 1, HG_MAX_AUX, NULL },
    { "key",   ARG_STR, 0, 20, NULL },
    { "value", ARG_STR, 0, 20, NULL },
};
static const cmd_arg_t A_SHELVES[] = {
    { "shelves", ARG_INT, 1, HG_MAX_SHELVES, NULL },
};
static const cmd_arg_t A_CAL[] = {
    { "shelf", ARG_INT, 1, HG_MAX_SHELVES, NULL },
    { "side",  ARG_ENUM, 0, 0, "A|B" },
    { "point", ARG_ENUM, 0, 0, "DRY|WET" },
    { "mv",    ARG_INT, 0, 3300, NULL },
};

const cmd_entry_t ZONE_CMD_ROWS[] = {
  { CMDV_SET|CMDV_GET, CMD_AREA_CONFIG, "ZONECFG", NULL, A_KV,       0, 2, 2, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "SHELF",   NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "LIGHT",   NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "WATER",   NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "FAN",     NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "VIB",     NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_SHELF,  "AUX",     NULL, A_AUX_KV,   1, 3, 3, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET,          CMD_AREA_CONFIG, "HW",      NULL, A_KV,       0, 2, 2, CMDF_ZONE|CMDF_UNLOCK, h_field,       NULL },
  { CMDV_GET,          CMD_AREA_CONFIG, "HW",      NULL, NULL,       0, 0, 0, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET,          CMD_AREA_CONFIG, "HWSHELF", NULL, A_SHELF_KV, 1, 3, 3, CMDF_ZONE|CMDF_UNLOCK, h_field,       NULL },
  { CMDV_GET,          CMD_AREA_CONFIG, "HWSHELF", NULL, A_SHELF_KV, 1, 1, 1, CMDF_ZONE,             h_field,       NULL },
  { CMDV_SET,          CMD_AREA_CONFIG, "SHELVES", NULL, A_SHELVES,  0, 1, 1, CMDF_ZONE|CMDF_UNLOCK, h_shelves,     NULL },
  { CMDV_SET|CMDV_GET, CMD_AREA_CONFIG, "CAL",     NULL, A_CAL,      1, 4, 4, CMDF_ZONE,             h_cal,         NULL },
  { CMDV_GET,          CMD_AREA_CONFIG, "CONFIG",  NULL, NULL,       0, 0, 0, CMDF_ZONE,             h_config_dump, NULL },
};
const int ZONE_CMD_ROWS_N = (int)(sizeof ZONE_CMD_ROWS / sizeof ZONE_CMD_ROWS[0]);
