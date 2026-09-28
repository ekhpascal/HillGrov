#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "alarm_mgr.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_poll.h"
#include "scr_shell.h"

/* The web's alarms page (app.js:896-911). No acknowledge or clear: the web
 * writes nothing here either. */

static am_snapshot_t *s_am;          /* PSRAM, this screen's copy; freed on teardown */
static uint32_t       s_am_seen;
static uint8_t        s_have;        /* s_am holds at least one published snapshot */
static lv_obj_t      *s_act, *s_act_empty, *s_ev, *s_ev_empty, *s_oom;

/* lv_table redraws the whole table on any cell write: write only on change. */
static void cell_set(lv_obj_t *t, uint32_t r, uint32_t c, const char *txt) {
    const char *cur = lv_table_get_cell_value(t, r, c);
    if (!cur || strcmp(cur, txt) != 0) lv_table_set_cell_value(t, r, c, txt);
}

static void age_text(const pnl_snap_t *sn, uint32_t stamp, char *b, size_t cap) {
    if (!sn || !sn->started) { snprintf(b, cap, "--"); return; }   /* HG.alarmAgo: no uptime yet -> a dash */
    pnl_fmt_age(sn->st.uptime_s, stamp, b, cap);
}

/* Only when the poller published a new alarm snapshot. */
static void lists_render(void) {
    pnl_label_set_if_changed(s_act_empty, "No active alarms.");
    pnl_label_set_if_changed(s_ev_empty, "No events.");
    pnl_obj_show(s_act_empty, s_am->n_active == 0);
    pnl_obj_show(s_act, s_am->n_active > 0);
    if (s_am->n_active > 0) {
        if (lv_table_get_row_count(s_act) != (uint32_t)s_am->n_active) lv_table_set_row_count(s_act, (uint32_t)s_am->n_active);
        for (int i = 0; i < s_am->n_active; i++) {
            cell_set(s_act, (uint32_t)i, 0, s_am->active[i].key);
            cell_set(s_act, (uint32_t)i, 1, s_am->active[i].text);
        }
    }
    pnl_obj_show(s_ev_empty, s_am->n_events == 0);
    pnl_obj_show(s_ev, s_am->n_events > 0);
    if (s_am->n_events > 0) {
        if (lv_table_get_row_count(s_ev) != (uint32_t)s_am->n_events) lv_table_set_row_count(s_ev, (uint32_t)s_am->n_events);
        for (int i = 0; i < s_am->n_events; i++) cell_set(s_ev, (uint32_t)i, 1, s_am->events[i].text);
    }
}

/* Every poll: the ages move even when the lists do not. */
static void ages_render(const pnl_snap_t *sn) {
    char b[24];
    for (int i = 0; i < s_am->n_active; i++) {
        age_text(sn, s_am->active[i].since_s, b, sizeof b);
        cell_set(s_act, (uint32_t)i, 2, b);
    }
    for (int i = 0; i < s_am->n_events; i++) {
        age_text(sn, s_am->events[i].at_s, b, sizeof b);
        cell_set(s_ev, (uint32_t)i, 0, b);
    }
}

static void alarms_update(const pnl_snap_t *sn) {
    if (!s_am || !s_act) return;
    uint32_t seq = pnl_poll_alarms_seq();
    if (seq == 0) return;   /* nothing published yet: the web's "Loading..." stays, never a false "No active alarms." */
    if (!s_have || seq != s_am_seen) {
        pnl_poll_alarms(s_am);
        s_am_seen = seq;
        s_have = 1;
        lists_render();
    }
    ages_render(sn);
}

/* Read-only: not clickable, so a press scrolls the page instead of
 * highlighting a cell (as the zone view's tables). */
static lv_obj_t *table(lv_obj_t *parent, const int32_t *widths, uint32_t cols) {
    lv_obj_t *t = lv_table_create(parent);
    lv_table_set_column_count(t, cols);
    lv_table_set_row_count(t, 1);
    for (uint32_t c = 0; c < cols; c++) lv_table_set_column_width(t, c, widths[c]);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t, LV_OBJ_FLAG_HIDDEN);
    return t;
}

static void alarms_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    pnl_label(page, "Alarms", &lv_font_montserrat_28, PNL_C_TEXT);

    s_am = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    if (!s_am) {
        s_oom = pnl_label(page, "Out of memory -- alarms unavailable. Try again shortly.", &lv_font_montserrat_20, PNL_C_WARN_TEXT);
        return;
    }
    s_have = 0;

    static const int32_t ACT_W[3] = { 140, 560, 160 };
    static const int32_t EV_W[2]  = { 160, 700 };
    pnl_label(page, "Active", &lv_font_montserrat_20, PNL_C_MUTED);
    s_act_empty = pnl_label(page, "Loading...", &lv_font_montserrat_20, PNL_C_MUTED);
    s_act = table(page, ACT_W, 3);
    pnl_label(page, "History", &lv_font_montserrat_20, PNL_C_MUTED);
    s_ev_empty = pnl_label(page, "Loading...", &lv_font_montserrat_20, PNL_C_MUTED);
    s_ev = table(page, EV_W, 2);

    alarms_update(pnl_shell_snap());
}

static void alarms_teardown(void) {
    if (s_am) { heap_caps_free(s_am); s_am = NULL; }
    s_act = s_act_empty = s_ev = s_ev_empty = s_oom = NULL;
    s_have = 0;
    s_am_seen = 0;
}

const pnl_screen_ops_t PNL_SCR_ALARMS = { "Alarms", alarms_build, alarms_update, alarms_teardown, 1 };
