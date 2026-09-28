/* wdg_keyboard.c -- modal keyboard overlay (glue; LVGL task only). */
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "wdg_keyboard.h"
#include "pnl_input.h"
#include "pnl_palette.h"

#define KB_TEXT_CAP  194     /* >= CMD_LINE_MAX + 2: the zone console opens with max_len CMD_LINE_MAX (192) so a 192nd
                                character is accepted here and refused VISIBLY by the console ("Line too long"); every other
                                caller asks for far less (63-character passwords at most) and keeps its own max_len */
#define KB_REVEAL_MS 10000u  /* a revealed password re-masks by itself (Global Constraints, "Secrets"; ruling C4) */

static struct {
    lv_obj_t      *overlay, *ta, *kb, *ok, *hint, *eye_lbl;
    lv_timer_t    *remask;   /* armed only while a masked entry is shown in clear */
    pcfg_kb_t      kind;
    uint8_t        min_len, max_len, masked;
    wdg_kb_done_fn done;
    void          *ctx;
} s_kb;

static const char *class_hint(pcfg_kb_t kb) {
    switch (kb) {
    case PCFG_KB_NUMERIC:      return "digits only";
    case PCFG_KB_TEXT_NOSPACE: return "no spaces";
    case PCFG_KB_HOSTNAME:     return "a-z, 0-9 and - only";
    case PCFG_KB_HEX:          return "hex digits and : only";
    default:                   return "printable characters";
    }
}

/* LVGL frees without zeroing: overwrite the textarea's own buffers in place first. In password mode
 * lv_textarea_get_text() returns the plain-text buffer (pwd_tmp), not the bullets (LVGL 9.5.0). */
static void ta_wipe(lv_obj_t *ta) {
    char *t = (char *)lv_textarea_get_text(ta);
    if (t) pnl_zero(t, strlen(t));
    char *l = lv_label_get_text(lv_textarea_get_label(ta));
    if (l) pnl_zero(l, strlen(l));
    lv_textarea_set_text(ta, "");
}

static int text_valid(void) {
    return pnl_text_ok(s_kb.kind, lv_textarea_get_text(s_kb.ta), s_kb.min_len, s_kb.max_len);
}
static void refresh_ok(void) {
    int ok = text_valid();
    lv_obj_set_state(s_kb.ok, LV_STATE_DISABLED, !ok);
    lv_obj_set_style_text_color(s_kb.hint, lv_color_hex(PNL_C_MUTED), 0);
}

/* The eye (masked keyboards only). masked 1 hides the text and disarms the timer; 0 shows it in clear
 * for KB_REVEAL_MS, after which remask_cb hides it again. wdg_keyboard_close() disarms it too. */
static void remask_cb(lv_timer_t *t);
static void set_masked(int masked) {
    if (s_kb.remask) { lv_timer_delete(s_kb.remask); s_kb.remask = NULL; }
    if (!masked) {
        s_kb.remask = lv_timer_create(remask_cb, KB_REVEAL_MS, NULL);
        if (!s_kb.remask) masked = 1;                /* no timer, no reveal: it could never re-mask */
        else lv_timer_set_repeat_count(s_kb.remask, 1);
    }
    lv_textarea_set_password_mode(s_kb.ta, masked ? true : false);
    lv_label_set_text(s_kb.eye_lbl, masked ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
}
static void remask_cb(lv_timer_t *t) {
    (void)t;
    s_kb.remask = NULL;                          /* repeat count 1: LVGL deletes the timer after this */
    if (s_kb.overlay) set_masked(1);
}

static void finish(int accepted) {
    if (!s_kb.overlay) return;
    wdg_kb_done_fn done = s_kb.done;
    void *ctx = s_kb.ctx;
    char text[KB_TEXT_CAP];
    snprintf(text, sizeof text, "%s", accepted ? lv_textarea_get_text(s_kb.ta) : "");
    wdg_keyboard_close();
    if (done) done(ctx, accepted, text);
    pnl_zero(text, sizeof text);
}

static void ev_ta(lv_event_t *e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_INSERT) {
        const char *ins = lv_event_get_param(e);
        for (const char *p = ins; p && *p; p++)
            if (!pnl_kb_accepts(s_kb.kind, *p)) { lv_textarea_set_insert_replace(s_kb.ta, ""); return; }
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        refresh_ok();
    } else if (c == LV_EVENT_READY) {            /* the keyboard's OK key, or Enter in one-line mode */
        if (text_valid()) finish(1);
        else lv_obj_set_style_text_color(s_kb.hint, lv_color_hex(PNL_C_OFFLINE_TEXT), 0);
    } else if (c == LV_EVENT_CANCEL) {           /* the keyboard's close key */
        finish(0);
    }
}
static void ev_ok(lv_event_t *e)     { (void)e; if (text_valid()) finish(1); }
static void ev_cancel(lv_event_t *e) { (void)e; finish(0); }
static void ev_eye(lv_event_t *e)    { (void)e; set_masked(!lv_textarea_get_password_mode(s_kb.ta)); }

static lv_obj_t *kb_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label_out) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 56);
    lv_obj_set_style_min_width(b, 96, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    if (label_out) *label_out = l;
    return b;
}

void wdg_keyboard_open(const char *title, pcfg_kb_t kb, const char *initial, uint8_t min_len, uint8_t max_len,
                       int masked, wdg_kb_done_fn done, void *ctx) {
    if (s_kb.overlay) wdg_keyboard_close();
    if (max_len == 0 || max_len >= KB_TEXT_CAP) max_len = KB_TEXT_CAP - 1;
    s_kb.kind = kb; s_kb.min_len = min_len; s_kb.max_len = max_len;
    s_kb.masked = masked ? 1 : 0; s_kb.done = done; s_kb.ctx = ctx;

    s_kb.overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_kb.overlay);
    lv_obj_set_size(s_kb.overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_kb.overlay, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_bg_opa(s_kb.overlay, LV_OPA_90, 0);
    lv_obj_add_flag(s_kb.overlay, LV_OBJ_FLAG_CLICKABLE);      /* modal: nothing reaches the screen below */

    lv_obj_t *card = lv_obj_create(s_kb.overlay);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_label_set_text(t, title ? title : "");

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    s_kb.ta = lv_textarea_create(row);
    lv_obj_set_flex_grow(s_kb.ta, 1);
    lv_textarea_set_one_line(s_kb.ta, true);
    lv_textarea_set_max_length(s_kb.ta, max_len);
    lv_textarea_set_password_mode(s_kb.ta, s_kb.masked);
    lv_textarea_set_text(s_kb.ta, initial ? initial : "");
    lv_obj_add_event_cb(s_kb.ta, ev_ta, LV_EVENT_ALL, NULL);
    if (s_kb.masked) kb_button(row, LV_SYMBOL_EYE_OPEN, ev_eye, &s_kb.eye_lbl);
    kb_button(row, "Cancel", ev_cancel, NULL);
    s_kb.ok = kb_button(row, "OK", ev_ok, NULL);

    s_kb.hint = lv_label_create(card);
    char h[96];
    snprintf(h, sizeof h, "%u to %u characters, %s", (unsigned)min_len, (unsigned)max_len, class_hint(kb));
    lv_label_set_text(s_kb.hint, h);

    s_kb.kb = lv_keyboard_create(s_kb.overlay);
    lv_obj_set_size(s_kb.kb, lv_pct(100), lv_pct(52));
    lv_obj_align(s_kb.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_kb.kb, kb == PCFG_KB_NUMERIC ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb.kb, s_kb.ta);
    refresh_ok();
}

void wdg_keyboard_close(void) {
    if (!s_kb.overlay) return;
    if (s_kb.remask) lv_timer_delete(s_kb.remask);   /* closing ends any reveal: the text goes with the overlay */
    ta_wipe(s_kb.ta);
    lv_obj_add_flag(s_kb.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(s_kb.overlay);           /* safe from inside the keyboard's own event */
    memset(&s_kb, 0, sizeof s_kb);               /* pointers and bounds only: the text was wiped above */
}

int wdg_keyboard_is_open(void) { return s_kb.overlay != NULL; }
