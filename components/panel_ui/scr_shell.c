#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_worker.h"
#include "pnl_poll.h"
#include "scr_shell.h"

static const char *TAG = "pnl_shell";

#define RAIL_W       120
#define SCREEN_W     1024
#define SCREEN_H     600
#define CB_BUDGET_US 200000   /* any single build/update callback: <= 200 ms (both idle tasks are TWDT-watched) */

typedef struct { const char *title; const pnl_screen_ops_t *ops; uint8_t rail; } pnl_reg_t;

/* The destination registry: ONE line per destination. Later tasks swap their
 * own line: HOME (Task 13), ZONE (14), ALARMS (16), CONFIG (20), SYSTEM (23),
 * PANEL (13, then 26), AUDIO (13). rail = shown with the left rail. */
static const pnl_reg_t REG[PNL_DEST_COUNT] = {
    [PNL_DEST_HOME]      = { "Home",       &PNL_SCR_DIAG,        1 },   /* Task 13: &PNL_SCR_HOME, rail 0 */
    [PNL_DEST_DASHBOARD] = { "Dashboard",  &PNL_SCR_DASHBOARD,   1 },
    [PNL_DEST_ZONE]      = { "Zone",       &PNL_SCR_PLACEHOLDER, 1 },   /* Task 14 */
    [PNL_DEST_CONFIG]    = { "Config",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 20 */
    [PNL_DEST_ALARMS]    = { "Alarms",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 16 */
    [PNL_DEST_SYSTEM]    = { "System",     &PNL_SCR_PLACEHOLDER, 1 },   /* Task 23 */
    [PNL_DEST_PANEL]     = { "Panel",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13: &PNL_SCR_DIAG; Task 26 */
    [PNL_DEST_AUDIO]     = { "Audio",      &PNL_SCR_PLACEHOLDER, 0 },   /* Task 13 */
    [PNL_DEST_DIAG]      = { "Touch test", &PNL_SCR_DIAG,        0 },
};

#define RAIL_N 6
static const pnl_dest_t   RAIL_DEST[RAIL_N] = { PNL_DEST_HOME, PNL_DEST_DASHBOARD, PNL_DEST_ZONE,
                                                PNL_DEST_CONFIG, PNL_DEST_ALARMS, PNL_DEST_SYSTEM };
static const char *const  RAIL_ICON[RAIL_N] = { LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_EYE_OPEN,
                                                LV_SYMBOL_SETTINGS, LV_SYMBOL_BELL, LV_SYMBOL_WIFI };
static const char *const  RAIL_TEXT[RAIL_N] = { "Home", "Dashboard", "Zone", "Config", "Alarms", "System" };

static lv_obj_t   *s_rail, *s_content, *s_page, *s_back;
static lv_obj_t   *s_rail_btn[RAIL_N];
static pnl_dest_t  s_cur = PNL_DEST_COUNT;
static int         s_arg;
static int         s_last_arg[PNL_DEST_COUNT];
static pnl_snap_t *s_snap;          /* PSRAM, lazily: the ONE LVGL-side copy (C11) */
static uint8_t     s_snap_warned;
static uint32_t    s_seen_seq;
static lv_timer_t *s_timer;
static pnl_dest_t  s_pend_dest;
static int         s_pend_arg;
static uint8_t     s_pend;

void pnl_label_set_if_changed(lv_obj_t *label, const char *text) {
    if (!label || !text) return;
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) lv_label_set_text(label, text);
}

void pnl_obj_show(lv_obj_t *o, int show) {
    if (!o) return;
    int hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
    if (show && hidden) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else if (!show && !hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

pnl_dest_t pnl_nav_current(void) { return s_cur; }
int        pnl_nav_arg(void)     { return s_arg; }
const char *pnl_nav_title(void)  { return s_cur < PNL_DEST_COUNT ? REG[s_cur].title : ""; }
const pnl_snap_t *pnl_shell_snap(void) { return s_snap; }

/* The snapshot copy is PSRAM (Global Constraints, "Memory"; ruling C11: no
 * static fallback in internal RAM). If PSRAM refuses, the shell keeps retrying
 * here and the screens show "Starting..." meanwhile. */
static int snap_ready(void) {
    if (s_snap) return 1;
    s_snap = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    if (!s_snap && !s_snap_warned) {
        s_snap_warned = 1;
        ESP_LOGW(TAG, "no PSRAM for the shell snapshot -- screens stay on \"Starting...\" until it is");
    }
    return s_snap != NULL;
}

static void warn_slow(const char *what, int64_t us) {
    if (us > CB_BUDGET_US)
        ESP_LOGW(TAG, "%s %s took %u ms (budget 200)", pnl_nav_title(), what, (unsigned)(us / 1000));
}

static void rail_paint(void) {
    for (int i = 0; i < RAIL_N; i++) {
        if (!s_rail_btn[i]) continue;
        int on = (RAIL_DEST[i] == s_cur);
        lv_obj_set_style_bg_color(s_rail_btn[i], lv_color_hex(on ? PNL_C_ACCENT : PNL_C_CARD), 0);
    }
}

static void back_cb(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_HOME, 0); }

static void nav_apply(void *unused) {
    (void)unused;
    s_pend = 0;
    pnl_dest_t d = s_pend_dest;
    int arg = s_pend_arg;
    if (d >= PNL_DEST_COUNT) return;
    if ((d == PNL_DEST_ZONE || d == PNL_DEST_CONFIG)) {
        if (arg == -1) arg = s_last_arg[d];
        if (arg >= 0) s_last_arg[d] = arg;
    }

    if (s_cur < PNL_DEST_COUNT && REG[s_cur].ops->teardown) REG[s_cur].ops->teardown();
    pnl_screen_gen_bump();                         /* any job still in flight for the old screen must not touch it */
    if (s_page) { lv_obj_delete(s_page); s_page = NULL; }
    if (s_back) { lv_obj_delete(s_back); s_back = NULL; }

    s_cur = d;
    s_arg = arg;
    int rail = REG[d].rail;
    pnl_obj_show(s_rail, rail);
    lv_obj_set_pos(s_content, rail ? RAIL_W : 0, 0);
    lv_obj_set_size(s_content, rail ? SCREEN_W - RAIL_W : SCREEN_W, SCREEN_H);
    rail_paint();

    s_page = lv_obj_create(s_content);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_text_color(s_page, lv_color_hex(PNL_C_TEXT), 0);

    if (snap_ready()) {
        pnl_poll_latest(s_snap);
        s_seen_seq = s_snap->seq;
    }
    int64_t t0 = esp_timer_get_time();
    REG[d].ops->build(s_page, arg);
    warn_slow("build", esp_timer_get_time() - t0);

    /* The diagnostics screen draws its own Home button (bottom centre): one
     * here would cover its top-left touch target (controller ruling C1). */
    if (!rail && d != PNL_DEST_HOME && REG[d].ops != &PNL_SCR_DIAG) {
        s_back = lv_button_create(s_content);
        lv_obj_t *l = lv_label_create(s_back);
        lv_label_set_text(l, LV_SYMBOL_LEFT " Home");
        lv_obj_align(s_back, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);
    }
}

void pnl_nav_go(pnl_dest_t d, int arg) {
    if (d >= PNL_DEST_COUNT) return;
    s_pend_dest = d;
    s_pend_arg = arg;
    if (!s_pend) {
        s_pend = 1;
        if (lv_async_call(nav_apply, NULL) != LV_RESULT_OK) s_pend = 0;
    }
}

static void rail_cb(lv_event_t *e) {
    pnl_dest_t d = (pnl_dest_t)(intptr_t)lv_event_get_user_data(e);
    pnl_nav_go(d, (d == PNL_DEST_ZONE || d == PNL_DEST_CONFIG) ? -1 : 0);
}

/* 250 ms: a new poll publish -> the open screen's update(), timed. */
static void tick_cb(lv_timer_t *t) {
    (void)t;
    if (s_cur >= PNL_DEST_COUNT || s_pend) return;
    if (pnl_poll_seq() == s_seen_seq || !snap_ready()) return;
    pnl_poll_latest(s_snap);
    s_seen_seq = s_snap->seq;
    if (!REG[s_cur].ops->update) return;
    int64_t t0 = esp_timer_get_time();
    REG[s_cur].ops->update(s_snap);
    warn_slow("update", esp_timer_get_time() - t0);
}

void pnl_shell_start(void) {
    (void)snap_ready();
    for (int i = 0; i < PNL_DEST_COUNT; i++) s_last_arg[i] = -1;

    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_rail = lv_obj_create(root);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, RAIL_W, SCREEN_H);
    lv_obj_set_pos(s_rail, 0, 0);
    lv_obj_set_style_bg_color(s_rail, lv_color_hex(PNL_C_CARD), 0);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_rail, 6, 0);
    lv_obj_set_style_pad_row(s_rail, 6, 0);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(s_rail, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < RAIL_N; i++) {
        lv_obj_t *b = lv_button_create(s_rail);
        lv_obj_set_size(b, RAIL_W - 12, 88);
        lv_obj_set_style_pad_hor(b, 4, 0);   /* the theme's ~20 px would leave "Dashboard" 68 px of a 108 px button */
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        pnl_label(b, RAIL_ICON[i], &lv_font_montserrat_28, PNL_C_TEXT);
        pnl_label(b, RAIL_TEXT[i], &lv_font_montserrat_14, PNL_C_TEXT);
        lv_obj_add_event_cb(b, rail_cb, LV_EVENT_CLICKED, (void *)(intptr_t)RAIL_DEST[i]);
        s_rail_btn[i] = b;
    }

    s_content = lv_obj_create(root);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    s_timer = lv_timer_create(tick_cb, 250, NULL);
    pnl_nav_go(PNL_DEST_HOME, 0);
}

/* ---- the shared placeholder: every destination whose task has not landed ---- */

static void ph_build(lv_obj_t *page, int arg) {
    (void)arg;
    char buf[64];
    snprintf(buf, sizeof buf, "%s: not available yet", pnl_nav_title());
    lv_obj_t *l = pnl_label(page, buf, &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_center(l);
}

const pnl_screen_ops_t PNL_SCR_PLACEHOLDER = { "Placeholder", ph_build, NULL, NULL, 0 };
