#include <stdio.h>
#include <stdint.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_diag.h"

#define TGT_N 5
#define TGT_W 150
#define TGT_H 90
#define TGT_M 12

static const char *const TGT_NAME[TGT_N] = { "top left", "top right", "bottom left", "bottom right", "centre" };
static const lv_align_t  TGT_ALIGN[TGT_N] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
                                             LV_ALIGN_BOTTOM_RIGHT, LV_ALIGN_CENTER };

static lv_obj_t   *s_heap, *s_touch;
static lv_obj_t   *s_tgt[TGT_N], *s_tgt_lbl[TGT_N];
static uint16_t    s_hits[TGT_N];
static lv_timer_t *s_tick;

static void tgt_paint(int i) {
    char buf[40];
    snprintf(buf, sizeof buf, "%s\nhit %u", TGT_NAME[i], (unsigned)s_hits[i]);
    lv_label_set_text(s_tgt_lbl[i], buf);
    lv_obj_set_style_bg_color(s_tgt[i], lv_color_hex(s_hits[i] ? PNL_C_OK : PNL_C_CARD), 0);
}

static void tgt_cb(lv_event_t *e) {
    intptr_t i = (intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= TGT_N || !s_tgt[i]) return;
    s_hits[i]++;
    tgt_paint((int)i);
}

static void reset_cb(lv_event_t *e) {
    (void)e;
    for (int i = 0; i < TGT_N; i++) { s_hits[i] = 0; if (s_tgt[i]) tgt_paint(i); }
}

/* 200 ms, on the LVGL task: heap and touch counters. Cheap reads only. */
static void diag_tick(lv_timer_t *t) {
    (void)t;
    if (!s_heap || !s_touch) return;
    char buf[160];
    snprintf(buf, sizeof buf, "Internal heap: free %u KB, min %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_heap, buf);
    panel_hw_status_t st;
    panel_hw_status(&st);
    snprintf(buf, sizeof buf, "Touch %s | reads %u | errors %u (last %d) | points %u | last %u,%u",
             st.touch_ok ? "ok" : "UNAVAILABLE", (unsigned)st.reads, (unsigned)st.read_errs, st.last_err,
             (unsigned)st.points, (unsigned)st.last_x, (unsigned)st.last_y);
    lv_label_set_text(s_touch, buf);
}

void scr_diag_build(lv_obj_t *parent) {
    char buf[96];
    snprintf(buf, sizeof buf, "HillGrow master v%s -- panel diagnostics", esp_app_get_description()->version);
    lv_obj_t *title = pnl_label(parent, buf, &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    s_heap = pnl_label(parent, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_heap, LV_ALIGN_TOP_MID, 0, 48);
    s_touch = pnl_label(parent, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_set_width(s_touch, 560);   /* a label's default long mode wraps at its width */
    lv_obj_align(s_touch, LV_ALIGN_TOP_MID, 0, 80);

    for (int i = 0; i < TGT_N; i++) {
        s_tgt[i] = lv_button_create(parent);
        lv_obj_set_size(s_tgt[i], TGT_W, TGT_H);
        lv_obj_align(s_tgt[i], TGT_ALIGN[i], i == 4 ? 0 : (i % 2 ? -TGT_M : TGT_M), i == 4 ? 0 : (i < 2 ? TGT_M : -TGT_M));
        s_tgt_lbl[i] = lv_label_create(s_tgt[i]);
        lv_obj_center(s_tgt_lbl[i]);
        lv_obj_add_event_cb(s_tgt[i], tgt_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        tgt_paint(i);
    }

    lv_obj_t *rst = lv_button_create(parent);
    lv_obj_t *rl = lv_label_create(rst);
    lv_label_set_text(rl, "Reset targets");
    lv_obj_align(rst, LV_ALIGN_BOTTOM_MID, 0, -TGT_M);
    lv_obj_add_event_cb(rst, reset_cb, LV_EVENT_CLICKED, NULL);

    s_tick = lv_timer_create(diag_tick, 200, NULL);
    diag_tick(s_tick);
}
