/* pcfg_gen.c -- the generator's rules, in order (plan Task 17):
 *  1 zone row in the hw plane -> READONLY (edit_kind still computed)   2 BOOL -> SWITCH
 *  3 ENUM -> SEGMENTED (<= 4 options) else ROLLER(ENUM)                 4 HHMM -> ROLLER(HHMM) 0..1439 step 1
 *  5 U8/U16 -> STEPPER, row bounds, step 1, big_step, NUMERIC keypad    6 PIN -> ROLLER(PIN), none 255
 *  7 STR16 -> TEXT, TEXT_NOSPACE, max 15 (its writer refuses spaces)    8 master secret STR -> SECRET, max 63
 *  9 other STR -> TEXT, keyboard from the presentation table. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pcfg_gen.h"
#include "pcfg_pres.h"
#include "hg_mcfg.h"

static int ci_eq(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return *a == *b;
}
static int prefix_ci(const char *s, const char *p) {
    for (; *p; s++, p++) {
        char x = *s, y = *p;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}
static int parse_long(const char *s, long *out) {
    char *end;
    if (!s || !*s) return -1;
    long v = strtol(s, &end, 10);
    if (*end != '\0') return -1;
    *out = v;
    return 0;
}
static uint8_t digits(int32_t v) {
    uint8_t d = 1;
    if (v < 0) v = -v;
    while (v >= 10) { v /= 10; d++; }
    return d;
}
static void set_unknown(pcfg_spec_t *o) {
    o->kind = o->edit_kind = PCFG_K_READONLY;
    o->readonly = 1;
    o->fmt = PCFG_FMT_TEXT;
    o->roller = PCFG_ROLL_NONE;
    o->keyboard = PCFG_KB_NONE;
    o->n_opts = 0;
}
static int split_enums(const char *enums, pcfg_spec_t *o) {
    const char *p = enums;
    int n = 0;
    if (!p || !*p) return -1;
    for (;;) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (n >= PCFG_MAX_OPTS || len == 0 || len >= PCFG_OPT_LEN) return -1;
        memcpy(o->opts[n], p, len);
        o->opts[n][len] = '\0';
        n++;
        if (!bar) break;
        p = bar + 1;
    }
    o->n_opts = (uint8_t)n;
    return 0;
}

int pcfg_spec_for(pcfg_table_t t, const hg_field_t *f, pcfg_spec_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    out->step = 1;
    out->scale_div = 1;
    out->none_value = -1;
    out->unit = "";
    out->label = (f && f->key) ? f->key : "?";
    if (!f || !f->key) { set_unknown(out); return -1; }
    int ngroups = (t == PCFG_TABLE_MASTER) ? HG_MG_COUNT : HG_G_COUNT;
    if ((t != PCFG_TABLE_ZONE && t != PCFG_TABLE_MASTER) || f->group >= ngroups) { set_unknown(out); return -1; }
    out->ftype = f->type;
    out->min = f->min;
    out->max = f->max;
    switch (f->type) {
    case HG_T_BOOL:
        out->edit_kind = PCFG_K_SWITCH; out->min = 0; out->max = 1; out->fmt = PCFG_FMT_BOOL;
        break;
    case HG_T_ENUM:
        if (split_enums(f->enums, out) != 0) { set_unknown(out); return -1; }
        out->min = 0; out->max = out->n_opts - 1; out->fmt = PCFG_FMT_ENUM;
        if (out->n_opts <= 4) out->edit_kind = PCFG_K_SEGMENTED;
        else { out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_ENUM; }
        break;
    case HG_T_HHMM:
        out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_HHMM;
        out->min = 0; out->max = 1439; out->fmt = PCFG_FMT_HHMM;
        break;
    case HG_T_U8: case HG_T_U16:
        out->edit_kind = PCFG_K_STEPPER; out->keyboard = PCFG_KB_NUMERIC;
        out->min_len = 1; out->max_len = digits(f->max);
        out->big_step = (f->max - f->min > 100) ? 10 : 0;
        out->fmt = PCFG_FMT_DEC;
        break;
    case HG_T_PIN:
        out->edit_kind = PCFG_K_ROLLER; out->roller = PCFG_ROLL_PIN;
        out->none_value = HG_NONE; out->fmt = PCFG_FMT_PIN;
        break;
    case HG_T_STR16:
        out->edit_kind = PCFG_K_TEXT; out->keyboard = PCFG_KB_TEXT_NOSPACE;
        out->min = 0; out->max = 15; out->max_len = 15; out->fmt = PCFG_FMT_TEXT;
        break;
    case HG_T_STR:
        out->edit_kind = (t == PCFG_TABLE_MASTER && hg_mcfg_is_secret(f)) ? PCFG_K_SECRET : PCFG_K_TEXT;
        out->keyboard = PCFG_KB_TEXT;
        out->max_len = (uint8_t)(f->max > 255 ? 255 : f->max);
        out->fmt = PCFG_FMT_TEXT;
        break;
    default:
        set_unknown(out);
        return -1;
    }
    const pcfg_pres_t *p = pcfg_pres_find(t, f->group, f->key);
    if (p) {
        if (p->label) out->label = p->label;
        if (p->unit) out->unit = p->unit;
        if (p->big_step) out->big_step = p->big_step;
        if (p->scale_div > 1) {
            out->scale_div = p->scale_div;
            if (out->fmt == PCFG_FMT_DEC) out->fmt = PCFG_FMT_SCALED;
        }
        if (p->fmt_hex && out->fmt == PCFG_FMT_DEC) out->fmt = PCFG_FMT_HEX;
        if (p->zero_text) out->zero_text = p->zero_text;
        if (out->edit_kind == PCFG_K_TEXT || out->edit_kind == PCFG_K_SECRET) {
            out->min_len = p->min_len;
            if (p->max_len && p->max_len < out->max_len) out->max_len = p->max_len;
            if (p->keyboard && f->type == HG_T_STR) out->keyboard = (pcfg_kb_t)p->keyboard;   /* STR16 stays NOSPACE */
        }
        if (p->readonly) out->readonly = 1;
    }
    if (t == PCFG_TABLE_ZONE && hg_group_is_hw(f->group)) out->readonly = 1;   /* rule 1 */
    out->kind = out->readonly ? PCFG_K_READONLY : out->edit_kind;
    return 0;
}

void pcfg_tighten(pcfg_spec_t *s, const hg_field_t *f, int shelf, const hg_zone_hw_t *hw_or_null) {
    if (!s || !f || !hw_or_null || shelf < 0 || shelf >= HG_MAX_SHELVES) return;
    if (f->group != HG_G_WATER || strcmp(f->key, "DOSE_S") != 0) return;
    int32_t m = hw_or_null->shelf[shelf].pump_max_run_s;
    if (m < s->min) m = s->min;
    if (m < s->max) s->max = m;
}

int pcfg_format(const pcfg_spec_t *s, const char *raw, char *out, size_t cap) {
    long v;
    if (!out || cap == 0) return -1;
    out[0] = '\0';
    if (!s || !raw) return -1;
    switch (s->fmt) {
    case PCFG_FMT_HHMM: {
        int m = hg_hhmm_parse(raw);
        if (m < 0) break;
        char t[6];
        hg_hhmm_format(m, t);
        snprintf(out, cap, "%s", t);
        return 0;
    }
    case PCFG_FMT_PIN:
        if (ci_eq(raw, "NONE")) { snprintf(out, cap, "none"); return 0; }
        if (parse_long(raw, &v) != 0) break;
        if (v == s->none_value) { snprintf(out, cap, "none"); return 0; }
        snprintf(out, cap, "%ld", v);
        return 0;
    case PCFG_FMT_HEX:
        if (parse_long(raw, &v) != 0 || v < 0) break;
        if (s->ftype == HG_T_U16) snprintf(out, cap, "0x%04lX", (unsigned long)v);
        else                      snprintf(out, cap, "0x%02lX", (unsigned long)v);
        return 0;
    case PCFG_FMT_BOOL:
        if (strcmp(raw, "1") == 0 || ci_eq(raw, "ON"))  { snprintf(out, cap, "on");  return 0; }
        if (strcmp(raw, "0") == 0 || ci_eq(raw, "OFF")) { snprintf(out, cap, "off"); return 0; }
        break;
    case PCFG_FMT_SCALED:
        if (parse_long(raw, &v) != 0) break;
        if (v == 0 && s->zero_text) { snprintf(out, cap, "%s", s->zero_text); return 0; }
        if (s->scale_div == 10) { snprintf(out, cap, "%ld.%ld", v / 10, labs(v % 10)); return 0; }
        snprintf(out, cap, "%ld", v);
        return 0;
    case PCFG_FMT_ENUM:
        if (parse_long(raw, &v) == 0 && v >= 0 && v < s->n_opts) { snprintf(out, cap, "%s", s->opts[v]); return 0; }
        snprintf(out, cap, "%s", raw);
        return 0;
    case PCFG_FMT_DEC:
    case PCFG_FMT_TEXT:
    default:
        snprintf(out, cap, "%s", raw);
        return 0;
    }
    snprintf(out, cap, "%s", raw);   /* unparseable: show it as it is */
    return -1;
}

int pcfg_parse_raw(const pcfg_spec_t *s, const char *raw, int32_t *v) {
    long x;
    if (!s || !raw || !v) return -1;
    switch (s->ftype) {
    case HG_T_HHMM: {
        int m = hg_hhmm_parse(raw);
        if (m < 0) return -1;
        *v = m;
        return 0;
    }
    case HG_T_ENUM:
        for (int i = 0; i < s->n_opts; i++) if (ci_eq(raw, s->opts[i])) { *v = i; return 0; }
        if (parse_long(raw, &x) == 0 && x >= 0 && x < s->n_opts) { *v = (int32_t)x; return 0; }
        return -1;
    case HG_T_PIN:
        if (ci_eq(raw, "NONE")) { *v = s->none_value; return 0; }
        if (parse_long(raw, &x) != 0) return -1;
        *v = (int32_t)x;
        return 0;
    case HG_T_BOOL:
        if (strcmp(raw, "1") == 0 || ci_eq(raw, "ON"))  { *v = 1; return 0; }
        if (strcmp(raw, "0") == 0 || ci_eq(raw, "OFF")) { *v = 0; return 0; }
        return -1;
    case HG_T_U8: case HG_T_U16:
        if (parse_long(raw, &x) != 0) return -1;
        *v = (int32_t)x;
        return 0;
    default:
        return -1;   /* STR16 / STR carry text, not a number */
    }
}

int pcfg_raw_text(const pcfg_spec_t *s, int32_t v, char *out, size_t cap) {
    if (!s || !out || cap == 0) return -1;
    switch (s->ftype) {
    case HG_T_HHMM: {
        if (v < 0 || v > 1439) return -1;
        char t[6];
        hg_hhmm_format((int)v, t);
        snprintf(out, cap, "%s", t);
        return 0;
    }
    case HG_T_ENUM:
        if (v < 0 || v >= s->n_opts) return -1;
        snprintf(out, cap, "%s", s->opts[v]);
        return 0;
    case HG_T_PIN:
        if (v == s->none_value) { snprintf(out, cap, "NONE"); return 0; }
        snprintf(out, cap, "%ld", (long)v);
        return 0;
    case HG_T_BOOL:
        snprintf(out, cap, "%s", v ? "1" : "0");
        return 0;
    case HG_T_U8: case HG_T_U16:
        snprintf(out, cap, "%ld", (long)v);
        return 0;
    default:
        return -1;
    }
}

int32_t pcfg_step(const pcfg_spec_t *s, int32_t cur, int dir, int big) {
    if (!s) return cur;
    int32_t d = (big && s->big_step > 0) ? s->big_step : (s->step > 0 ? s->step : 1);
    int64_t n = (int64_t)cur + (dir > 0 ? d : (dir < 0 ? -d : 0));
    if (n < s->min) n = s->min;
    if (n > s->max) n = s->max;
    return (int32_t)n;
}

static const hg_field_t *find_row(pcfg_table_t t, int g, const char *key) {
    const hg_field_t *tab = (t == PCFG_TABLE_MASTER) ? HG_MFIELDS : HG_FIELDS;
    int n = (t == PCFG_TABLE_MASTER) ? HG_MFIELD_COUNT : HG_FIELD_COUNT;
    for (int i = 0; i < n; i++) if (tab[i].group == g && ci_eq(tab[i].key, key)) return &tab[i];
    return NULL;
}

int pcfg_locate(pcfg_table_t t, const char *path, uint8_t *group, int *idx, const hg_field_t **row) {
    char buf[96];
    if (!path || !group || !idx || !row) return -1;
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    char *p = buf;
    int ix = -1, pfx = 0;                        /* pfx: 0 none, 1 "shelf[N].", 2 "aux[N]." */
    if (t == PCFG_TABLE_ZONE) {
        if (prefix_ci(p, "cfg.")) p += 4;        /* the web's cfgParseBadPath strips the same two roots */
        else if (prefix_ci(p, "hw.")) p += 3;
        if (prefix_ci(p, "shelf[")) { pfx = 1; p += 6; }
        else if (prefix_ci(p, "aux[")) { pfx = 2; p += 4; }
        if (pfx) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p || end[0] != ']' || end[1] != '.') return -1;
            ix = (int)v;
            p = end + 2;
        }
    }
    char *dot = strchr(p, '.');
    const char *gname = NULL, *key = p;
    if (dot) {
        *dot = '\0';
        gname = p;
        key = dot + 1;
        if (strchr(key, '.') || !*key) return -1;
    }
    int g = -1;
    if (t == PCFG_TABLE_MASTER) {
        if (!gname) return -1;
        for (int i = 0; i < HG_MG_COUNT; i++) if (ci_eq(gname, HG_MGROUP_NAMES[i])) g = i;
        if (g < 0) return -1;
        ix = -1;
    } else {
        if (gname) { g = hg_group_find(gname); if (g < 0) return -1; }
        else if (pfx == 1) g = HG_G_SHELF;        /* hg_cfg_validate: "shelf[0].enabled" -- SHELF keys carry no group */
        else return -1;
        int sc = hg_group_scope((uint8_t)g);
        if (sc == 0) { if (pfx) return -1; ix = -1; }
        else if (sc == 1) { if (pfx == 2 || (pfx == 1 && (ix < 0 || ix >= HG_MAX_SHELVES))) return -1; }
        else { if (pfx == 1 || (pfx == 2 && (ix < 0 || ix >= HG_MAX_AUX))) return -1; }
    }
    const hg_field_t *r = find_row(t, g, key);
    if (!r) return -1;
    *group = (uint8_t)g;
    *idx = ix;
    *row = r;
    return 0;
}
