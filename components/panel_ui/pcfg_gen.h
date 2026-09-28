#pragma once
/* pcfg_gen.h -- the type-driven config-widget generator (pure: no LVGL, no IDF headers).
 * Every row of HG_FIELDS (zone) and HG_MFIELDS (master) becomes a pcfg_spec_t; wdg_field.c builds the
 * LVGL row from the spec, never from the table directly. Values stay in hg_field_read/hg_field_write
 * text form ("raw") everywhere -- the spec only says how to show, bound and step them. */
#include <stddef.h>
#include <stdint.h>
#include "hg_cfg.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PCFG_TABLE_ZONE = 0, PCFG_TABLE_MASTER } pcfg_table_t;
typedef enum { PCFG_K_STEPPER = 0, PCFG_K_SWITCH, PCFG_K_SEGMENTED, PCFG_K_ROLLER, PCFG_K_TEXT, PCFG_K_SECRET, PCFG_K_READONLY } pcfg_kind_t;
typedef enum { PCFG_KB_NONE = 0, PCFG_KB_NUMERIC, PCFG_KB_TEXT, PCFG_KB_TEXT_NOSPACE, PCFG_KB_HOSTNAME, PCFG_KB_HEX } pcfg_kb_t;
typedef enum { PCFG_ROLL_NONE = 0, PCFG_ROLL_HHMM, PCFG_ROLL_ENUM, PCFG_ROLL_PIN } pcfg_roll_t;
typedef enum { PCFG_FMT_DEC = 0, PCFG_FMT_HEX, PCFG_FMT_HHMM, PCFG_FMT_ENUM, PCFG_FMT_PIN, PCFG_FMT_TEXT, PCFG_FMT_BOOL, PCFG_FMT_SCALED } pcfg_fmt_t;
#define PCFG_MAX_OPTS 8
#define PCFG_OPT_LEN  16
typedef struct {
    pcfg_kind_t kind, edit_kind;   /* READONLY rows keep edit_kind for formatting */
    uint8_t     ftype, readonly;
    int32_t     min, max, step, big_step;
    int16_t     scale_div;         /* 1; 10 for LIGHT.DLI */
    int32_t     none_value;        /* 255 for PIN; -1 none */
    pcfg_roll_t roller;
    uint8_t     n_opts; char opts[PCFG_MAX_OPTS][PCFG_OPT_LEN];
    pcfg_kb_t   keyboard;
    uint8_t     min_len, max_len;
    pcfg_fmt_t  fmt;
    const char *label, *unit;      /* presentation, else key / "" */
    const char *zero_text;         /* "off" for LIGHT.DLI, NULL otherwise */
} pcfg_spec_t;

/* 0 / -1 (NULL row, unknown ftype, bad enum list or a group outside the table -> READONLY + TEXT: renders, never crashes) */
int     pcfg_spec_for(pcfg_table_t t, const hg_field_t *f, pcfg_spec_t *out);
/* Zone table only. WATER.DOSE_S: max = min(max, hw->shelf[shelf].pump_max_run_s); never widens; hw NULL -> unchanged */
void    pcfg_tighten(pcfg_spec_t *s, const hg_field_t *f, int shelf, const hg_zone_hw_t *hw_or_null);
/* hg_field_read text -> display ("06:30", "none", "12.5", "off", "0x40", "on"); 0 / -1 (unparseable: out = raw) */
int     pcfg_format(const pcfg_spec_t *s, const char *raw, char *out, size_t cap);
/* raw -> number (HHMM minutes, ENUM index, PIN 255 for NONE, BOOL 0/1); 0 / -1 (text kinds: always -1) */
int     pcfg_parse_raw(const pcfg_spec_t *s, const char *raw, int32_t *v);
/* number -> hg_field_write text; 0 / -1 (HHMM outside 0..1439, ENUM index out of range, text kinds) */
int     pcfg_raw_text(const pcfg_spec_t *s, int32_t v, char *out, size_t cap);
/* cur + dir * (big && big_step ? big_step : step), clamped to [min, max]; never wraps */
int32_t pcfg_step(const pcfg_spec_t *s, int32_t cur, int dir, int big);
/* An error path -> the row it names. 0 / -1. Shapes:
 *   "cfg.ZONECFG.NAME", "cfg.shelf[1].WATER.TARGET", "cfg.aux[0].AUX.MODE"   (merge / psvc_zone_fields_fn)
 *   "shelf[2].light.off", "shelf[0].enabled", "zonecfg.name", "aux.pulse_s"   (hg_cfg_validate; aux: idx -1)
 *   "WIFI.AP_PASS", "SYS.HOSTNAME"                                            (master)
 *   "hw.aux_pin" and anything unknown -> -1 (the caller shows a banner) */
int     pcfg_locate(pcfg_table_t t, const char *path, uint8_t *group, int *idx, const hg_field_t **row);

#ifdef __cplusplus
}
#endif
