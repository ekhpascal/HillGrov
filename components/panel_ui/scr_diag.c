#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_worker.h"
#include "pnl_poll.h"
#include "scr_diag.h"

#define TGT_N 5
#define TGT_W 150
#define TGT_H 90
#define TGT_M 12

static const char *const TGT_NAME[TGT_N] = { "top left", "top right", "bottom left", "bottom right", "centre" };
static const lv_align_t  TGT_ALIGN[TGT_N] = { LV_ALIGN_TOP_LEFT, LV_ALIGN_TOP_RIGHT, LV_ALIGN_BOTTOM_LEFT,
                                             LV_ALIGN_BOTTOM_RIGHT, LV_ALIGN_CENTER };

static lv_obj_t   *s_heap, *s_touch, *s_poll;
static lv_obj_t   *s_tgt[TGT_N], *s_tgt_lbl[TGT_N];
static uint16_t    s_hits[TGT_N];
static lv_timer_t *s_tick;

static const char *TAG = "scr_diag";
static lv_obj_t   *s_job_lbl, *s_job_btn;
static lv_timer_t *s_ui_tick;               /* 16 ms: counts LVGL cycles while a job runs */
static uint32_t    s_ticks, s_ticks_at_submit;
static uint8_t     s_job_busy;

static void ui_tick(lv_timer_t *t) { (void)t; s_ticks++; }

/* [WORKER] deliberately blocks for 3 s -- if the UI keeps ticking meanwhile,
 * the worker/mailbox split holds. */
static void job_block_run(pnl_job_t *j) {
    if (pnl_on_lvgl_task()) {
        ESP_LOGE(TAG, "blocking job ran on the LVGL task -- THE RULE is broken");
        j->irc = -1;
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
    j->irc = 0;
}

/* [LVGL] via the drain timer. */
static void job_block_done(pnl_job_t *j) {
    s_job_busy = 0;
    if (j->screen_gen != pnl_screen_gen() || !s_job_lbl) return;   /* this screen was torn down meanwhile */
    char buf[96];
    if (j->irc != 0) snprintf(buf, sizeof buf, "FAILED: the job ran on the LVGL task");
    else snprintf(buf, sizeof buf, "done after %u ms, %u UI ticks during the job",
                  (unsigned)(j->t_done_ms - j->t_submit_ms), (unsigned)(s_ticks - s_ticks_at_submit));
    lv_label_set_text(s_job_lbl, buf);
}

static void job_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_job_busy) return;
    s_ticks_at_submit = s_ticks;
    if (pnl_worker_submit(job_block_run, job_block_done, NULL, 0) != 0) {
        lv_label_set_text(s_job_lbl, "worker unavailable (not started or pool full)");
        return;
    }
    s_job_busy = 1;
    lv_label_set_text(s_job_lbl, "running... (the spinner must keep turning)");
}

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
    if (s_poll) {
        /* The LVGL pool figure every stage gate records (Global Constraints,
         * "Memory": raise CONFIG_LV_MEM_SIZE_KILOBYTES only if max_used > 75 % of
         * it). The budget is the INTERNAL pool; mon.total_size also counts Task 7's
         * PSRAM overflow pool, so it is not the denominator. */
        lv_mem_monitor_t mon;
        lv_mem_monitor(&mon);
        snprintf(buf, sizeof buf, "Poller: seq %u | LVGL pool: max used %u of %u KB internal (+%u KB PSRAM)",
                 (unsigned)pnl_poll_seq(), (unsigned)(mon.max_used / 1024), (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES,
                 (unsigned)(mon.total_size / 1024 > CONFIG_LV_MEM_SIZE_KILOBYTES
                            ? mon.total_size / 1024 - CONFIG_LV_MEM_SIZE_KILOBYTES : 0));
        lv_label_set_text(s_poll, buf);
    }
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
    s_poll = pnl_label(parent, "Poller: not started", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_poll, LV_ALIGN_TOP_MID, 0, 136);

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

    lv_obj_t *spin = lv_spinner_create(parent);
    lv_obj_set_size(spin, 72, 72);
    lv_obj_align(spin, LV_ALIGN_LEFT_MID, 190, 0);

    s_job_btn = lv_button_create(parent);
    lv_obj_t *jl = lv_label_create(s_job_btn);
    lv_label_set_text(jl, "Blocking job (3 s)");
    lv_obj_align(s_job_btn, LV_ALIGN_RIGHT_MID, -170, -30);
    lv_obj_add_event_cb(s_job_btn, job_btn_cb, LV_EVENT_CLICKED, NULL);

    s_job_lbl = pnl_label(parent, "tap to prove the LVGL task never blocks", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_set_width(s_job_lbl, 300);
    lv_obj_align(s_job_lbl, LV_ALIGN_RIGHT_MID, -120, 40);

    s_ui_tick = lv_timer_create(ui_tick, 16, NULL);
    s_tick = lv_timer_create(diag_tick, 200, NULL);
    diag_tick(s_tick);
}
