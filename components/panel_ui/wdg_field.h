#pragma once
/* wdg_field.h -- one config row per pcfg_kind_t (glue; LVGL task only). The row owns a copy of its spec and
 * the current raw text (hg_field_write form); it reports every change through wdg_changed_fn. */
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"
#include "hg_cfg.h"
#include "pcfg_gen.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wdg_changed_fn)(void *ctx, uint8_t group, int idx, const hg_field_t *f, const char *raw_text);
lv_obj_t *wdg_field_create(lv_obj_t *parent, const pcfg_spec_t *s, uint8_t group, int idx, const hg_field_t *f,
                           const char *raw_text, wdg_changed_fn cb, void *ctx);
    /* one row: label + unit + editor by kind:
       STEPPER [-] value [+] with [--]/[++] when big_step, long-press repeat on the buttons. A tap on the value does
         nothing (numbers are stepped, spec "Config generated"); a LONG-PRESS on the value is the secondary route to a
         NUMERIC keypad (not for scaled rows);
       SWITCH lv_switch; SEGMENTED lv_buttonmatrix one-checked; ROLLER two-column HH/MM, or enum / pin roller;
       TEXT value -> keyboard; SECRET "********" (fixed 8) + [Reveal] (10 s) + [Change] (keyboard starts empty; blank = unchanged);
       READONLY formatted value, muted (the owner shows "hardware plane -- set at the zone console" once per group).
       NULL on bad args or no memory. */
void wdg_field_set_error(lv_obj_t *row, const char *code);   /* outline PNL_C_OFFLINE + code text; NULL clears */
void wdg_field_set_dirty(lv_obj_t *row, int dirty);          /* U+2022 bullet + space marker before the label */
void wdg_field_set_secret_text(lv_obj_t *row, const char *plain);   /* the revealed value (NULL re-masks and wipes) */
typedef int (*wdg_reveal_fn)(void *ctx, const hg_field_t *f, char *out, size_t cap);   /* 0 filled / -1 */
void wdg_field_set_reveal(lv_obj_t *row, wdg_reveal_fn fn, void *ctx);                  /* SECRET rows: shows [Reveal] */

#ifdef __cplusplus
}
#endif
