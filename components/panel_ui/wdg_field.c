/* wdg_field.c -- config rows by kind (glue; LVGL task only). Row state lives in PSRAM and is freed on LV_EVENT_DELETE. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "wdg_field.h"
#include "wdg_keyboard.h"
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_palette.h"
#include "psvc_edit.h"

#define REVEAL_MS 10000u
#define MASK_TEXT "********"
#define DOT       "\xE2\x80\xA2 "     /* U+2022, carried by the built-in Montserrat */

typedef struct {
    pcfg_spec_t       spec;
    uint8_t           group;
    int               idx;
    const hg_field_t *f;
    wdg_changed_fn    cb;
    void             *ctx;
    wdg_reveal_fn     reveal;
    void             *reveal_ctx;
    char              raw[PSVC_FEDIT_TEXT_MAX];      /* hg_field_write text -- never a secret */
    lv_obj_t         *row, *name, *value, *err, *editor, *roll_b, *reveal_btn;
    lv_timer_t       *remask;
    uint8_t           revealed;
    const char       *map[PCFG_MAX_OPTS + 1];        /* SEGMENTED map: points into spec.opts */
} wrow_t;

static void wipe_label(lv_obj_t *l) { char *t = l ? lv_label_get_text(l) : NULL; if (t) pnl_zero(t, strlen(t)); }

static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, 8, 0);
    lv_obj_set_style_pad_row(o, 2, 0);
    return o;
}
static lv_obj_t *button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);                        /* a finger, not a stylus */
    lv_obj_set_style_min_width(b, 64, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_ALL, ud);
    return b;
}
static void button_text(lv_obj_t *b, const char *text) {
    if (b) lv_label_set_text(lv_obj_get_child(b, 0), text);
}

static void show_value(wrow_t *w) {
    char v[PSVC_FEDIT_TEXT_MAX + 8], t[PSVC_FEDIT_TEXT_MAX + 32];
    pcfg_format(&w->spec, w->raw, v, sizeof v);
    if (w->spec.kind == PCFG_K_TEXT && v[0] == '\0') snprintf(v, sizeof v, "(empty)");
    if (w->spec.unit && w->spec.unit[0]) snprintf(t, sizeof t, "%s %s", v, w->spec.unit);
    else snprintf(t, sizeof t, "%s", v);
    lv_label_set_text(w->value, t);
}

static void emit(wrow_t *w, const char *text) {
    wdg_field_set_error(w->row, NULL);
    wdg_field_set_dirty(w->row, 1);
    if (w->cb) w->cb(w->ctx, w->group, w->idx, w->f, text);
}
static void set_num(wrow_t *w, int32_t v) {
    if (pcfg_raw_text(&w->spec, v, w->raw, sizeof w->raw) != 0) return;
    if (w->value) show_value(w);
    emit(w, w->raw);
}

/* ---- STEPPER ---- */
static void step(lv_event_t *e, int dir, int big) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_SHORT_CLICKED && c != LV_EVENT_LONG_PRESSED_REPEAT) return;
    wrow_t *w = lv_event_get_user_data(e);
    int32_t cur;
    if (pcfg_parse_raw(&w->spec, w->raw, &cur) != 0) cur = w->spec.min;
    int32_t nv = pcfg_step(&w->spec, cur, dir, big);
    if (nv != cur) set_num(w, nv);
}
static void ev_dec(lv_event_t *e)     { step(e, -1, 0); }
static void ev_inc(lv_event_t *e)     { step(e, +1, 0); }
static void ev_dec_big(lv_event_t *e) { step(e, -1, 1); }
static void ev_inc_big(lv_event_t *e) { step(e, +1, 1); }
static void keypad_done(void *ctx, int accepted, const char *text) {
    wrow_t *w = ctx;
    if (!accepted) return;
    char *end;
    long v = strtol(text, &end, 10);
    if (end == text || *end || v < w->spec.min || v > w->spec.max) {
        char m[48];
        snprintf(m, sizeof m, "Out of range (%ld..%ld)", (long)w->spec.min, (long)w->spec.max);
        wdg_field_set_error(w->row, m);
        return;
    }
    set_num(w, (int32_t)v);
}
/* Spec ("Config generated"): numbers get steppers and only free-text fields summon the keyboard, so a plain tap
 * on the value opens nothing. The keypad is a deliberate secondary action: a long-press on the value. */
static void ev_value_long(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    wrow_t *w = lv_event_get_user_data(e);
    wdg_keyboard_open(w->spec.label, PCFG_KB_NUMERIC, w->raw, 1, w->spec.max_len, 0, keypad_done, w);
}

/* ---- SWITCH / SEGMENTED / ROLLERS ---- */
static void ev_switch(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    wrow_t *w = lv_event_get_user_data(e);
    set_num(w, lv_obj_has_state(w->editor, LV_STATE_CHECKED) ? 1 : 0);
}
static void ev_seg(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    wrow_t *w = lv_event_get_user_data(e);
    uint32_t sel = lv_buttonmatrix_get_selected_button(w->editor);
    if (sel >= w->spec.n_opts) return;                     /* LV_BUTTONMATRIX_BUTTON_NONE */
    lv_buttonmatrix_set_button_ctrl(w->editor, sel, LV_BUTTONMATRIX_CTRL_CHECKED);   /* never leave none checked */
    set_num(w, (int32_t)sel);
}
static void ev_hhmm(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    wrow_t *w = lv_event_get_user_data(e);
    set_num(w, (int32_t)lv_roller_get_selected(w->editor) * 60 + (int32_t)lv_roller_get_selected(w->roll_b));
}
static void ev_roller(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    wrow_t *w = lv_event_get_user_data(e);
    uint32_t sel = lv_roller_get_selected(w->editor);
    if (w->spec.roller == PCFG_ROLL_PIN) set_num(w, sel == 0 ? w->spec.none_value : (int32_t)sel - 1 + w->spec.min);
    else set_num(w, (int32_t)sel);
}
static void put2(char **p, int v) { *(*p)++ = (char)('0' + v / 10); *(*p)++ = (char)('0' + v % 10); }
static const char *two_digit_opts(int n) {         /* "00\n01\n..": 24 hours or 60 minutes */
    static char hh[24 * 3], mm[60 * 3];
    char *buf = (n == 24) ? hh : mm;
    if (!buf[0]) {
        char *p = buf;
        for (int i = 0; i < n; i++) { if (i) *p++ = '\n'; put2(&p, i); }
        *p = '\0';
    }
    return buf;
}
static lv_obj_t *roller(lv_obj_t *parent, const char *opts, uint32_t sel, int width, wrow_t *w, lv_event_cb_t cb) {
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);    /* copies opts */
    lv_roller_set_visible_row_count(r, 3);
    lv_obj_set_width(r, width);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    lv_obj_add_event_cb(r, cb, LV_EVENT_VALUE_CHANGED, w);
    return r;
}

/* ---- TEXT / SECRET ---- */
static void text_done(void *ctx, int accepted, const char *text) {
    wrow_t *w = ctx;
    if (!accepted) return;
    snprintf(w->raw, sizeof w->raw, "%s", text);
    show_value(w);
    emit(w, w->raw);
}
static void ev_text(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wrow_t *w = lv_event_get_user_data(e);
    wdg_keyboard_open(w->spec.label, w->spec.keyboard, w->raw, w->spec.min_len, w->spec.max_len, 0, text_done, w);
}
static void secret_done(void *ctx, int accepted, const char *text) {
    wrow_t *w = ctx;
    if (!accepted || !text[0]) return;                 /* blank = unchanged: nothing to record */
    char tmp[PSVC_FEDIT_TEXT_MAX];
    snprintf(tmp, sizeof tmp, "%s", text);
    if (w->revealed) wdg_field_set_secret_text(w->row, NULL);
    emit(w, tmp);
    pnl_zero(tmp, sizeof tmp);
}
static void ev_change(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wrow_t *w = lv_event_get_user_data(e);
    wdg_keyboard_open(w->spec.label, w->spec.keyboard, "", w->spec.min_len, w->spec.max_len, 1, secret_done, w);
}
static void ev_reveal(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    wrow_t *w = lv_event_get_user_data(e);
    if (w->revealed) { wdg_field_set_secret_text(w->row, NULL); return; }
    if (!w->reveal) return;
    char plain[PSVC_FEDIT_TEXT_MAX];
    if (w->reveal(w->reveal_ctx, w->f, plain, sizeof plain) == 0) wdg_field_set_secret_text(w->row, plain);
    pnl_zero(plain, sizeof plain);
}
static void remask_cb(lv_timer_t *t) {
    wrow_t *w = lv_timer_get_user_data(t);
    w->remask = NULL;                                  /* repeat count 1: LVGL deletes the timer after this */
    wdg_field_set_secret_text(w->row, NULL);
}

static void ev_row_delete(lv_event_t *e) {
    wrow_t *w = lv_event_get_user_data(e);
    if (wdg_keyboard_is_open()) wdg_keyboard_close();  /* its done() would get a dead ctx */
    if (w->remask) lv_timer_delete(w->remask);
    if (w->revealed) wipe_label(w->value);             /* children are still alive during LV_EVENT_DELETE */
    pnl_zero(w, sizeof *w);
    heap_caps_free(w);
}

lv_obj_t *wdg_field_create(lv_obj_t *parent, const pcfg_spec_t *s, uint8_t group, int idx, const hg_field_t *f,
                           const char *raw_text, wdg_changed_fn cb, void *ctx) {
    if (!parent || !s || !f) return NULL;
    wrow_t *w = heap_caps_calloc(1, sizeof *w, MALLOC_CAP_SPIRAM);
    if (!w) return NULL;
    w->spec = *s; w->group = group; w->idx = idx; w->f = f; w->cb = cb; w->ctx = ctx;
    if (s->kind != PCFG_K_SECRET && raw_text) snprintf(w->raw, sizeof w->raw, "%s", raw_text);

    lv_obj_t *row = lv_obj_create(parent);
    w->row = row;
    lv_obj_set_user_data(row, w);
    lv_obj_add_event_cb(row, ev_row_delete, LV_EVENT_DELETE, w);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_BORDER), 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *left = box(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(left, 1);
    w->name = lv_label_create(left);
    lv_obj_set_width(w->name, lv_pct(100));
    lv_label_set_long_mode(w->name, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(w->name, s->label);
    w->err = lv_label_create(left);
    lv_obj_set_width(w->err, lv_pct(100));
    lv_label_set_long_mode(w->err, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(w->err, lv_color_hex(PNL_C_OFFLINE_TEXT), 0);
    lv_obj_add_flag(w->err, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *right = box(row, LV_FLEX_FLOW_ROW);
    int32_t v = 0;
    int have = pcfg_parse_raw(s, w->raw, &v) == 0;
    switch (s->kind) {
    case PCFG_K_STEPPER:
        if (s->big_step > 0) button(right, LV_SYMBOL_MINUS LV_SYMBOL_MINUS, ev_dec_big, w);
        button(right, LV_SYMBOL_MINUS, ev_dec, w);
        w->value = lv_label_create(right);
        lv_obj_set_style_min_width(w->value, 120, 0);
        lv_obj_set_style_text_align(w->value, LV_TEXT_ALIGN_CENTER, 0);
        if (s->scale_div <= 1) {                       /* a keypad of raw x10 units would mislead */
            lv_obj_add_flag(w->value, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(w->value, ev_value_long, LV_EVENT_LONG_PRESSED, w);
        }
        button(right, LV_SYMBOL_PLUS, ev_inc, w);
        if (s->big_step > 0) button(right, LV_SYMBOL_PLUS LV_SYMBOL_PLUS, ev_inc_big, w);
        show_value(w);
        break;
    case PCFG_K_SWITCH:
        w->editor = lv_switch_create(right);
        if (have && v) lv_obj_add_state(w->editor, LV_STATE_CHECKED);
        lv_obj_add_event_cb(w->editor, ev_switch, LV_EVENT_VALUE_CHANGED, w);
        break;
    case PCFG_K_SEGMENTED:
        for (int i = 0; i < s->n_opts; i++) w->map[i] = w->spec.opts[i];
        w->map[s->n_opts] = "";
        w->editor = lv_buttonmatrix_create(right);
        lv_buttonmatrix_set_map(w->editor, w->map);
        lv_buttonmatrix_set_button_ctrl_all(w->editor, LV_BUTTONMATRIX_CTRL_CHECKABLE);
        lv_buttonmatrix_set_one_checked(w->editor, true);
        lv_obj_set_size(w->editor, s->n_opts * 130, 56);
        if (have && v >= 0 && v < s->n_opts) lv_buttonmatrix_set_button_ctrl(w->editor, (uint32_t)v, LV_BUTTONMATRIX_CTRL_CHECKED);
        lv_obj_add_event_cb(w->editor, ev_seg, LV_EVENT_VALUE_CHANGED, w);
        break;
    case PCFG_K_ROLLER:
        if (s->roller == PCFG_ROLL_HHMM) {
            int m = have ? (int)v : 0;
            w->editor = roller(right, two_digit_opts(24), (uint32_t)(m / 60), 90, w, ev_hhmm);
            lv_label_set_text(lv_label_create(right), ":");
            w->roll_b = roller(right, two_digit_opts(60), (uint32_t)(m % 60), 90, w, ev_hhmm);
        } else {
            char opts[PCFG_MAX_OPTS * PCFG_OPT_LEN + 64];
            char *p = opts;
            uint32_t sel = 0;
            if (s->roller == PCFG_ROLL_PIN) {
                p += snprintf(p, 8, "none");
                /* pins are 0..15 today (hg_cfg_fields.c HWSHELF); the capacity check keeps a wider row in bounds */
                for (int32_t pin = s->min; pin <= s->max && pin < 100 && (size_t)(p - opts) + 4 <= sizeof opts; pin++) {
                    *p++ = '\n';
                    if (pin >= 10) put2(&p, (int)pin); else *p++ = (char)('0' + pin);
                }
                *p = '\0';
                sel = (!have || v == s->none_value) ? 0 : (uint32_t)(v - s->min + 1);
            } else {
                for (int i = 0; i < s->n_opts; i++) {
                    size_t l = strlen(s->opts[i]);
                    if (i) *p++ = '\n';
                    memcpy(p, s->opts[i], l);
                    p += l;
                }
                *p = '\0';
                sel = have ? (uint32_t)v : 0;
            }
            w->editor = roller(right, opts, sel, 180, w, ev_roller);
        }
        break;
    case PCFG_K_TEXT:
        w->value = lv_label_create(right);
        lv_obj_add_flag(w->value, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(w->value, ev_text, LV_EVENT_CLICKED, w);
        show_value(w);
        button(right, LV_SYMBOL_EDIT, ev_text, w);
        break;
    case PCFG_K_SECRET:
        w->value = lv_label_create(right);
        lv_label_set_text(w->value, MASK_TEXT);
        w->reveal_btn = button(right, "Reveal", ev_reveal, w);
        lv_obj_add_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);   /* until wdg_field_set_reveal() */
        button(right, "Change", ev_change, w);
        break;
    case PCFG_K_READONLY:
    default:
        w->value = lv_label_create(right);
        lv_obj_set_style_text_color(w->value, lv_color_hex(PNL_C_MUTED), 0);
        show_value(w);
        break;
    }
    return row;
}

void wdg_field_set_error(lv_obj_t *row, const char *code) {
    wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
    if (!w) return;
    if (code && code[0]) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_OFFLINE), 0);
        lv_label_set_text(w->err, code);
        lv_obj_remove_flag(w->err, LV_OBJ_FLAG_HIDDEN);
        lv_obj_scroll_to_view_recursive(row, LV_ANIM_ON);
    } else {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(PNL_C_BORDER), 0);
        lv_obj_add_flag(w->err, LV_OBJ_FLAG_HIDDEN);
    }
}

void wdg_field_set_dirty(lv_obj_t *row, int dirty) {
    wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
    if (!w) return;
    if (dirty) lv_label_set_text_fmt(w->name, DOT "%s", w->spec.label);
    else lv_label_set_text(w->name, w->spec.label);
    lv_obj_set_style_text_color(w->name, lv_color_hex(dirty ? PNL_C_OK_TEXT : PNL_C_TEXT), 0);
}

void wdg_field_set_secret_text(lv_obj_t *row, const char *plain) {
    wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
    if (!w || w->spec.kind != PCFG_K_SECRET) return;
    if (w->remask) { lv_timer_delete(w->remask); w->remask = NULL; }
    wipe_label(w->value);
    if (plain) w->remask = lv_timer_create(remask_cb, REVEAL_MS, w);
    if (plain && w->remask) {                          /* no timer, no reveal: it could never re-mask */
        lv_timer_set_repeat_count(w->remask, 1);
        lv_label_set_text(w->value, plain[0] ? plain : "(empty)");
        w->revealed = 1;
        button_text(w->reveal_btn, "Hide");
    } else {
        lv_label_set_text(w->value, MASK_TEXT);
        w->revealed = 0;
        button_text(w->reveal_btn, "Reveal");
    }
}

void wdg_field_set_reveal(lv_obj_t *row, wdg_reveal_fn fn, void *ctx) {
    wrow_t *w = row ? lv_obj_get_user_data(row) : NULL;
    if (!w || w->spec.kind != PCFG_K_SECRET) return;
    w->reveal = fn;
    w->reveal_ctx = ctx;
    if (fn) lv_obj_remove_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(w->reveal_btn, LV_OBJ_FLAG_HIDDEN);
}
