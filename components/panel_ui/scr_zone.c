#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"
#include "scr_zone.h"

/* The web's zone page (app.js:801-834): 14 rows, the shelf table, "Configure
 * this zone". Read-only; Task 22 adds the console and Replace board below. */

#define ZROWS 14
static const char *const ROW_NAME[ZROWS] = {
    "MAC", "Health", "Firmware", "Config gen", "Hops", "Link", "Link stale", "Config sync",
    "Last heard", "Uptime", "Heap", "Resets", "Faults", "Mode" };
static const char *const SHELF_HEAD[7] = { "#", "Soil A", "Soil B", "White", "Red", "Out", "Pump" };

static lv_obj_t *s_sel, *s_title, *s_notfound, *s_body, *s_rows, *s_shelves, *s_noshelf, *s_extra;
static lv_obj_t *s_sel_btn[HG_MAX_ZONES], *s_sel_lbl[HG_MAX_ZONES];
static uint8_t   s_sel_id[HG_MAX_ZONES];
static int       s_nsel = -1;
static uint8_t   s_zone;          /* shown now; 0 = none */
static uint8_t   s_last_zone;     /* survives teardown: "-1 = last viewed" */

lv_obj_t *scr_zone_extra_area(void) { return s_zone ? s_extra : NULL; }
uint8_t   scr_zone_current(void)    { return s_zone; }

/* Node ids are untrusted indices: a slot counts only when it is used and its
 * id is the slot's own (psvc_state_fill: slot = id-1). */
static int slot_valid(const psvc_state_t *st, int i) {
    return st->node[i].used && st->node[i].id == (uint8_t)(i + 1);
}

static const hg_node_t *find_node(const psvc_state_t *st, uint8_t id) {
    if (id < 1 || id > HG_MAX_ZONES || !slot_valid(st, id - 1)) return NULL;
    return &st->node[id - 1];
}

static uint8_t first_zone(const pnl_snap_t *sn) {
    if (!sn || !sn->started) return 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (slot_valid(&sn->st, i)) return (uint8_t)(i + 1);
    return 0;
}

static void sel_cb(lv_event_t *e) {
    uint8_t id = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (id && id != s_zone) pnl_nav_go(PNL_DEST_ZONE, id);
}

static void cfg_cb(lv_event_t *e) {
    (void)e;
    if (s_zone) pnl_nav_go(PNL_DEST_CONFIG, s_zone);
}

/* lv_table redraws the whole table on any cell write: write only on change. */
static void cell_set(lv_obj_t *t, uint32_t r, uint32_t c, const char *txt) {
    const char *cur = lv_table_get_cell_value(t, r, c);
    if (!cur || strcmp(cur, txt) != 0) lv_table_set_cell_value(t, r, c, txt);
}

static void selector_update(const psvc_state_t *st) {
    uint8_t ids[HG_MAX_ZONES];
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (slot_valid(st, i)) ids[n++] = (uint8_t)(i + 1);
    if (n != s_nsel || memcmp(ids, s_sel_id, (size_t)n) != 0) {
        lv_obj_clean(s_sel);
        memset(s_sel_btn, 0, sizeof s_sel_btn);
        memset(s_sel_lbl, 0, sizeof s_sel_lbl);
        s_nsel = n;
        memcpy(s_sel_id, ids, (size_t)n);
        for (int k = 0; k < n; k++) {
            lv_obj_t *b = lv_button_create(s_sel);
            s_sel_lbl[k] = pnl_label(b, "", &lv_font_montserrat_20, PNL_C_TEXT);
            lv_obj_add_event_cb(b, sel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[k]);
            lv_obj_set_style_bg_color(b, lv_color_hex(ids[k] == s_zone ? PNL_C_ACCENT : PNL_C_CARD), 0);
            s_sel_btn[k] = b;
        }
    }
    for (int k = 0; k < s_nsel; k++) {   /* a rename shows without a rebuild */
        char name[17];
        pnl_zone_name(&st->node[s_sel_id[k] - 1], name);
        pnl_label_set_if_changed(s_sel_lbl[k], name);
    }
}

static void zone_update(const pnl_snap_t *sn) {
    if (!s_body) return;
    if (!sn || !sn->started) {   /* NULL = no shell copy yet: same as "not started" */
        pnl_label_set_if_changed(s_title, "Starting...");
        pnl_obj_show(s_notfound, 0);
        pnl_obj_show(s_body, 0);
        return;
    }
    if (!s_zone) {   /* opened before the first poll or before any enrolment: rebuild onto the first zone */
        uint8_t z = first_zone(sn);
        if (z) { pnl_nav_go(PNL_DEST_ZONE, z); return; }
    }
    const psvc_state_t *st = &sn->st;
    selector_update(st);

    const hg_node_t *n = find_node(st, s_zone);
    char b[64];
    if (!n) {
        if (s_zone) snprintf(b, sizeof b, "Zone %u not found.", (unsigned)s_zone);
        else snprintf(b, sizeof b, "No zones enrolled.");
        pnl_label_set_if_changed(s_notfound, b);
        pnl_obj_show(s_notfound, 1);
        pnl_obj_show(s_body, 0);
        pnl_label_set_if_changed(s_title, "Zone");
        return;
    }
    pnl_obj_show(s_notfound, 0);
    pnl_obj_show(s_body, 1);

    char name[17];
    pnl_zone_name(n, name);
    snprintf(b, sizeof b, "Zone %u -- %s", (unsigned)n->id, name);
    pnl_label_set_if_changed(s_title, b);

    /* state_snap.c ss_node(): the same field in the same row as /api/state */
    char v[ZROWS][32];
    snprintf(v[0], sizeof v[0], "%02x:%02x:%02x:%02x:%02x:%02x",
             n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    snprintf(v[1], sizeof v[1], "%s", state_snap_health_name(n->health));
    snprintf(v[2], sizeof v[2], "%u.%u.%u", (unsigned)n->hb.fw_maj, (unsigned)n->hb.fw_min, (unsigned)n->hb.fw_patch);
    snprintf(v[3], sizeof v[3], "%u", (unsigned)n->hb.cfg_gen);
    snprintf(v[4], sizeof v[4], "%u", (unsigned)n->hops);
    snprintf(v[5], sizeof v[5], "%u", (unsigned)n->link_flags);
    snprintf(v[6], sizeof v[6], "%s", n->health == NODE_H_OFFLINE ? "yes" : "no");
    snprintf(v[7], sizeof v[7], "%s", st->cfg_sync_failed[n->id - 1] ? "FAILED" : "OK");
    snprintf(v[8], sizeof v[8], "%us ago", (unsigned)((uint32_t)(st->now_ms - n->last_hb_ms) / 1000u));
    snprintf(v[9], sizeof v[9], "%us", (unsigned)n->hb.uptime_s);
    snprintf(v[10], sizeof v[10], "%u KB", (unsigned)n->hb.min_free_heap_kb);
    snprintf(v[11], sizeof v[11], "%u", (unsigned)n->hb.reset_reason);
    snprintf(v[12], sizeof v[12], "0x%" PRIx64, (uint64_t)n->hb.active_faults);
    snprintf(v[13], sizeof v[13], "%u", (unsigned)n->hb.mode);
    for (uint32_t r = 0; r < ZROWS; r++) cell_set(s_rows, r, 1, v[r]);

    int ns = n->hb.n_shelves > 4 ? 4 : n->hb.n_shelves;
    pnl_obj_show(s_shelves, ns > 0);
    pnl_obj_show(s_noshelf, ns == 0);
    if (ns > 0) {
        if (lv_table_get_row_count(s_shelves) != (uint32_t)(ns + 1)) lv_table_set_row_count(s_shelves, (uint32_t)(ns + 1));
        for (int i = 0; i < ns; i++) {
            const hg_hb_shelf_t *s = &n->hb.shelf[i];
            uint32_t r = (uint32_t)(i + 1);
            snprintf(b, sizeof b, "%d", i);                          cell_set(s_shelves, r, 0, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->pct_a);         cell_set(s_shelves, r, 1, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->pct_b);         cell_set(s_shelves, r, 2, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->white);         cell_set(s_shelves, r, 3, b);
            snprintf(b, sizeof b, "%u", (unsigned)s->red);           cell_set(s_shelves, r, 4, b);
            cell_set(s_shelves, r, 5, s->out_flags ? "on" : "off");
            snprintf(b, sizeof b, "%us", (unsigned)s->pump_today_s); cell_set(s_shelves, r, 6, b);
        }
    }
}

/* Read-only tables: not clickable, so a press scrolls the page instead of
 * highlighting a cell. */
static lv_obj_t *table(lv_obj_t *parent, uint32_t cols, uint32_t rows) {
    lv_obj_t *t = lv_table_create(parent);
    lv_table_set_column_count(t, cols);
    lv_table_set_row_count(t, rows);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    return t;
}

static void zone_build(lv_obj_t *page, int arg) {
    const pnl_snap_t *sn = pnl_shell_snap();
    uint8_t z = 0;
    if (arg >= 1 && arg <= HG_MAX_ZONES) z = (uint8_t)arg;
    else if (s_last_zone) z = s_last_zone;
    else z = first_zone(sn);   /* 0 while the shell copy is NULL or not started: update() retries */
    s_zone = z;
    if (z) s_last_zone = z;

    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);

    s_sel = lv_obj_create(page);
    lv_obj_remove_style_all(s_sel);
    lv_obj_set_size(s_sel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_sel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_sel, 8, 0);
    lv_obj_set_style_pad_row(s_sel, 8, 0);

    s_title = pnl_label(page, "Zone", &lv_font_montserrat_28, PNL_C_TEXT);
    s_notfound = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_add_flag(s_notfound, LV_OBJ_FLAG_HIDDEN);

    s_body = lv_obj_create(page);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 10, 0);

    lv_obj_t *cfg = lv_button_create(s_body);
    pnl_label(cfg, "Configure this zone " LV_SYMBOL_RIGHT, &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_add_event_cb(cfg, cfg_cb, LV_EVENT_CLICKED, NULL);

    s_rows = table(s_body, 2, ZROWS);
    lv_table_set_column_width(s_rows, 0, 220);
    lv_table_set_column_width(s_rows, 1, 420);
    for (uint32_t r = 0; r < ZROWS; r++) {
        lv_table_set_cell_value(s_rows, r, 0, ROW_NAME[r]);
        lv_table_set_cell_value(s_rows, r, 1, "");
    }

    pnl_label(s_body, "Shelves", &lv_font_montserrat_28, PNL_C_TEXT);
    s_shelves = table(s_body, 7, 1);
    for (uint32_t c = 0; c < 7; c++) {
        lv_table_set_column_width(s_shelves, c, c == 0 ? 60 : 110);
        lv_table_set_cell_value(s_shelves, 0, c, SHELF_HEAD[c]);
    }
    s_noshelf = pnl_label(s_body, "No shelf telemetry.", &lv_font_montserrat_20, PNL_C_MUTED);

    s_extra = lv_obj_create(s_body);   /* Task 22: console + Replace board */
    lv_obj_remove_style_all(s_extra);
    lv_obj_set_size(s_extra, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_extra, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_extra, 10, 0);

    zone_update(sn);
}

static void zone_teardown(void) {
    s_sel = s_title = s_notfound = s_body = s_rows = s_shelves = s_noshelf = s_extra = NULL;
    memset(s_sel_btn, 0, sizeof s_sel_btn);
    memset(s_sel_lbl, 0, sizeof s_sel_lbl);
    s_nsel = -1;
    s_zone = 0;
}

const pnl_screen_ops_t PNL_SCR_ZONE = { "Zone", zone_build, zone_update, zone_teardown, 1 };
