#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "scr_shell.h"

/* The web dashboard (app.js:758-799), in its order: ring banner, default-
 * password banner, master card, degraded console slots, one card per ENROLLED
 * node. Built once; update() only rewrites what changed. */

typedef struct {
    lv_obj_t *card, *name, *health, *sub, *soil, *light, *pump;
    uint8_t   id;
    int       health_seen;
} dash_card_t;

static lv_obj_t   *s_start, *s_ring, *s_def, *s_def_lbl, *s_master, *s_mtitle, *s_time, *s_sta, *s_ap, *s_heap;
static lv_obj_t   *s_quar, *s_grid;
static dash_card_t s_card[HG_MAX_ZONES];
static int         s_ring_seen = -1;

static void card_cb(lv_event_t *e) {
    dash_card_t *c = (dash_card_t *)lv_event_get_user_data(e);
    if (c && c->id) pnl_nav_go(PNL_DEST_ZONE, c->id);
}

static void def_cb(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_SYSTEM, 0); }

/* A transparent, non-clickable flex row: taps fall through to the card. */
static lv_obj_t *row(lv_obj_t *parent) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static void badge_style(lv_obj_t *l) {
    lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(l, 4, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
}

static void dash_update(const pnl_snap_t *sn);

static void dash_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_row(page, 12, 0);

    s_start = pnl_label(page, "Starting...", &lv_font_montserrat_28, PNL_C_MUTED);

    s_ring = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_set_width(s_ring, LV_PCT(100));
    badge_style(s_ring);
    lv_obj_set_style_pad_all(s_ring, 10, 0);

    s_def = lv_obj_create(page);
    pnl_theme_card(s_def);
    lv_obj_set_size(s_def, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(s_def, lv_color_hex(PNL_C_WARN), 0);
    lv_obj_set_flex_flow(s_def, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_def, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_def, LV_OBJ_FLAG_SCROLLABLE);
    s_def_lbl = pnl_label(s_def, "", &lv_font_montserrat_20, PNL_C_WARN_TEXT);
    lv_obj_set_width(s_def_lbl, 620);
    lv_obj_t *b = lv_button_create(s_def);
    pnl_label(b, "Open System", &lv_font_montserrat_20, PNL_C_TEXT);
    lv_obj_add_event_cb(b, def_cb, LV_EVENT_CLICKED, NULL);

    s_master = lv_obj_create(page);
    pnl_theme_card(s_master);
    lv_obj_set_size(s_master, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_master, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_master, 6, 0);
    lv_obj_remove_flag(s_master, LV_OBJ_FLAG_SCROLLABLE);
    s_mtitle = pnl_label(s_master, "", &lv_font_montserrat_28, PNL_C_TEXT);
    s_time   = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_sta    = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_ap     = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_TEXT);
    s_heap   = pnl_label(s_master, "", &lv_font_montserrat_20, PNL_C_MUTED);

    s_quar = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_WARN_TEXT);

    s_grid = lv_obj_create(page);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_grid, 12, 0);
    lv_obj_set_style_pad_column(s_grid, 12, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < HG_MAX_ZONES; i++) {
        dash_card_t *c = &s_card[i];
        memset(c, 0, sizeof *c);
        c->health_seen = -1;
        c->card = lv_obj_create(s_grid);
        pnl_theme_card(c->card);
        lv_obj_set_size(c->card, 280, 150);
        lv_obj_set_flex_flow(c->card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c->card, 6, 0);
        lv_obj_remove_flag(c->card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(c->card, card_cb, LV_EVENT_CLICKED, c);
        lv_obj_t *head = row(c->card);
        c->name   = pnl_label(head, "", &lv_font_montserrat_28, PNL_C_TEXT);
        c->health = pnl_label(head, "", &lv_font_montserrat_14, PNL_C_TEXT);
        badge_style(c->health);
        c->sub    = pnl_label(c->card, "", &lv_font_montserrat_14, PNL_C_MUTED);
        lv_obj_set_width(c->sub, LV_PCT(100));   /* wraps: "fw 1.2.3  stale | last seen 12345s ago" overflows 256 px */
        lv_obj_t *tiles = row(c->card);
        c->soil   = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        c->light  = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        c->pump   = pnl_label(tiles, "", &lv_font_montserrat_20, PNL_C_TEXT);
        lv_obj_add_flag(c->card, LV_OBJ_FLAG_HIDDEN);
    }
    dash_update(pnl_shell_snap());
}

static void dash_update(const pnl_snap_t *sn) {
    if (!s_grid) return;
    int up = (sn && sn->started) ? 1 : 0;   /* NULL = no shell copy yet: same as "not started" */
    pnl_obj_show(s_start, !up);
    pnl_obj_show(s_ring, up);
    pnl_obj_show(s_master, up);
    pnl_obj_show(s_grid, up);
    if (!up) { pnl_obj_show(s_def, 0); pnl_obj_show(s_quar, 0); return; }

    const psvc_state_t *st = &sn->st;
    char buf[160], t[96];

    const char *rs = state_snap_ring_state_name(st->ring.state);
    if (st->ring.blame[0]) snprintf(buf, sizeof buf, "Ring: %s -- %.47s", rs, st->ring.blame);
    else snprintf(buf, sizeof buf, "Ring: %s", rs);
    pnl_label_set_if_changed(s_ring, buf);
    if ((int)st->ring.state != s_ring_seen) {
        s_ring_seen = (int)st->ring.state;
        uint32_t bg = st->ring.state == RING_ST_OK ? PNL_C_OK : st->ring.state == RING_ST_OPEN ? PNL_C_OFFLINE : PNL_C_CARD;
        lv_obj_set_style_bg_color(s_ring, lv_color_hex(bg), 0);
    }

    if (st->web_default || st->ap_default) {
        snprintf(buf, sizeof buf, "Factory default password still in use (%s) -- change it in System.",
                 st->web_default && st->ap_default ? "web, Wi-Fi AP" : st->web_default ? "web" : "Wi-Fi AP");
        pnl_label_set_if_changed(s_def_lbl, buf);
        pnl_obj_show(s_def, 1);
    } else {
        pnl_obj_show(s_def, 0);
    }

    snprintf(buf, sizeof buf, "Master  v%s", st->version);
    pnl_label_set_if_changed(s_mtitle, buf);
    pnl_fmt_master_time(st->time, st->time_src, st->utc_offset_s, st->time_is_set, t, sizeof t);
    snprintf(buf, sizeof buf, "Time: %s", t);
    pnl_label_set_if_changed(s_time, buf);
    pnl_fmt_sta(&st->wifi, t, sizeof t);
    snprintf(buf, sizeof buf, "STA: %s", t);
    pnl_label_set_if_changed(s_sta, buf);
    pnl_fmt_ap(&st->wifi, t, sizeof t);
    snprintf(buf, sizeof buf, "AP: %s", t);
    pnl_label_set_if_changed(s_ap, buf);
    snprintf(buf, sizeof buf, "Heap min %u KB", (unsigned)st->heap_min_kb);
    pnl_label_set_if_changed(s_heap, buf);

    if (st->web_cmd_quarantined > 0) {
        snprintf(buf, sizeof buf, "%u web console slot(s) degraded", (unsigned)st->web_cmd_quarantined);
        pnl_label_set_if_changed(s_quar, buf);
        pnl_obj_show(s_quar, 1);
    } else {
        pnl_obj_show(s_quar, 0);
    }

    int k = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &st->node[i];
        if (!n->used) continue;
        dash_card_t *c = &s_card[k++];
        c->id = n->id;
        char name[17];
        pnl_zone_name(n, name);
        pnl_label_set_if_changed(c->name, name);
        pnl_label_set_if_changed(c->health, state_snap_health_name(n->health));
        if ((int)n->health != c->health_seen) {
            c->health_seen = (int)n->health;
            lv_obj_set_style_bg_color(c->health, pnl_health_color(n->health), 0);
        }
        snprintf(buf, sizeof buf, "fw %u.%u.%u%s | last seen %us ago",
                 (unsigned)n->hb.fw_maj, (unsigned)n->hb.fw_min, (unsigned)n->hb.fw_patch,
                 n->health == NODE_H_OFFLINE ? "  stale" : "",
                 (unsigned)((st->now_ms - n->last_hb_ms) / 1000u));
        pnl_label_set_if_changed(c->sub, buf);
        pnl_readings_t r;
        pnl_node_readings(n, &r);
        pnl_fmt_reading(&r, 0, t, sizeof t); snprintf(buf, sizeof buf, "Soil %s", t);  pnl_label_set_if_changed(c->soil, buf);
        pnl_fmt_reading(&r, 1, t, sizeof t); snprintf(buf, sizeof buf, "Light %s", t); pnl_label_set_if_changed(c->light, buf);
        pnl_fmt_reading(&r, 2, t, sizeof t); snprintf(buf, sizeof buf, "Pump %s", t);  pnl_label_set_if_changed(c->pump, buf);
        pnl_obj_show(c->card, 1);
    }
    for (; k < HG_MAX_ZONES; k++) { s_card[k].id = 0; pnl_obj_show(s_card[k].card, 0); }
}

static void dash_teardown(void) {
    s_start = s_ring = s_def = s_def_lbl = s_master = s_mtitle = s_time = s_sta = s_ap = s_heap = NULL;
    s_quar = s_grid = NULL;
    memset(s_card, 0, sizeof s_card);
    s_ring_seen = -1;
}

const pnl_screen_ops_t PNL_SCR_DASHBOARD = { "Dashboard", dash_build, dash_update, dash_teardown, 1 };
