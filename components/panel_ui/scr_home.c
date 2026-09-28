#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "lvgl.h"
#include "state_snap.h"
#include "pnl_fmt.h"
#include "pnl_home.h"
#include "pnl_time.h"
#include "pnl_fonts.h"
#include "pnl_palette.h"
#include "pnl_theme.h"
#include "pnl_prefs_nvs.h"
#include "scr_shell.h"

/* Spec "Shell -- the home screen". Full screen, no rail. Layout (1024 x 600):
 *   top bar y 10 | clock y 36 (180 px face, 129 px line) | date y 222 | context y 284 |
 *   band y 330 (118 px) | alarm badge y 452 | icons at the bottom. */
#define HOME_MARGIN 24
#define BAND_Y      330
#define BAND_H      118
#define TILE_H      98

static lv_obj_t   *s_ring, *s_src, *s_wifi, *s_clock, *s_date, *s_ctx, *s_band, *s_none, *s_badge;
static lv_obj_t   *s_tile[HG_MAX_ZONES], *s_dot[HG_MAX_ZONES], *s_l1[HG_MAX_ZONES], *s_l2[HG_MAX_ZONES], *s_l3[HG_MAX_ZONES];
static uint8_t     s_tile_id[HG_MAX_ZONES];
static int         s_tile_health[HG_MAX_ZONES];
static int         s_ntiles = -1;
static pnl_tile_detail_t s_detail;
static lv_timer_t *s_clock_timer;
static int         s_alarm_on = -1;
static uint32_t    s_ring_c, s_wifi_c;

/* Task 26 (D3): the analogue face -- lv_scale round, hour/minute/second needles, updated by the same 1 s clock timer */
static lv_obj_t *s_scale, *s_hand_h, *s_hand_m, *s_hand_s, *s_ana_unset;
static int       s_ana_shown = -1;   /* the second of the day the needles show; -1 = none (or "Clock not set") */
static const char *HOUR_TXT[] = { "12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", NULL };

static lv_obj_t *home_hand(lv_obj_t *scale, int width, uint32_t color) {
    lv_obj_t *l = lv_line_create(scale);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    return l;
}

static void home_analogue_build(lv_obj_t *parent) {
    s_scale = lv_scale_create(parent);
    lv_obj_set_size(s_scale, 200, 200);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(s_scale, 0, 60);
    lv_scale_set_total_tick_count(s_scale, 61);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_angle_range(s_scale, 360);
    lv_scale_set_rotation(s_scale, 270);             /* 0 at the top */
    lv_scale_set_label_show(s_scale, true);
    lv_scale_set_text_src(s_scale, HOUR_TXT);
    lv_obj_set_style_radius(s_scale, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_scale, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_scale, lv_color_hex(PNL_C_CARD), 0);
    s_hand_h = home_hand(s_scale, 8, PNL_C_TEXT);
    s_hand_m = home_hand(s_scale, 5, PNL_C_TEXT);
    s_hand_s = home_hand(s_scale, 2, PNL_C_ACCENT);
    s_ana_unset = lv_label_create(s_scale);
    lv_label_set_text(s_ana_unset, "Clock not set");  /* an unset clock is reported honestly, never as a plausible time */
    lv_obj_center(s_ana_unset);
    lv_obj_add_flag(s_ana_unset, LV_OBJ_FLAG_HIDDEN);
    s_ana_shown = -1;
}

/* clock_tick runs from the 1 s timer and again from every poll's update: move the needles only when the second moved */
static void home_analogue_set(const pnl_local_t *t) {
    if (!s_scale) return;
    int now = t->valid ? t->minute_of_day * 60 + t->sec : -1;
    if (now == s_ana_shown && now >= 0) return;
    lv_obj_t *hands[3] = { s_hand_h, s_hand_m, s_hand_s };
    for (int i = 0; i < 3; i++) pnl_obj_show(hands[i], t->valid);
    pnl_obj_show(s_ana_unset, !t->valid);
    s_ana_shown = now;
    if (!t->valid) return;
    lv_scale_set_line_needle_value(s_scale, s_hand_h, 55, (t->hour % 12) * 5 + t->min / 12);
    lv_scale_set_line_needle_value(s_scale, s_hand_m, 80, t->min);
    lv_scale_set_line_needle_value(s_scale, s_hand_s, 88, t->sec);
}

static void nav_cb(lv_event_t *e) { pnl_nav_go((pnl_dest_t)(intptr_t)lv_event_get_user_data(e), 0); }

static void tile_cb(lv_event_t *e) {
    uint8_t id = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    if (id) pnl_nav_go(PNL_DEST_ZONE, id);
}

static void band_cb(lv_event_t *e) {
    (void)e;
    if (s_alarm_on == 1) pnl_nav_go(PNL_DEST_ALARMS, 0);
}

/* A 1 s triangle wave on the band's background opacity, 255 -> 120 -> 255. */
static void band_opa_cb(void *var, int32_t v) {
    int32_t tri = v < 500 ? v : 1000 - v;
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)(255 - tri * 135 / 500), 0);
}

static void set_color_if_changed(lv_obj_t *o, uint32_t *seen, uint32_t hex) {
    if (*seen == hex) return;
    *seen = hex;
    lv_obj_set_style_text_color(o, lv_color_hex(hex), 0);
}

/* 1 s: the clock, the date and the context line. time(NULL) is UTC; the
 * offset and "is the clock set at all" come from the last poll. No shell copy
 * yet (NULL) or no poll yet (started 0, all zero) both mean "not set", so the
 * clock never shows a plausible wrong time. While unset the date line already
 * says "Clock not set"; the context line stays empty rather than repeat it. */
static void clock_tick(lv_timer_t *t) {
    (void)t;
    if (!s_clock && !s_scale) return;   /* the date and context lines keep ticking on the analogue face */
    const pnl_snap_t *sn = pnl_shell_snap();
    pnl_local_t lt;
    pnl_local_time((int64_t)time(NULL), sn ? sn->st.utc_offset_s : 0, sn ? sn->st.time_is_set : 0, &lt);
    home_analogue_set(&lt);
    char b[64];
    pnl_fmt_clock(&lt, b, sizeof b);
    pnl_label_set_if_changed(s_clock, b);
    pnl_fmt_date(&lt, b, sizeof b);
    pnl_label_set_if_changed(s_date, b);
    if (sn && lt.valid) pnl_ctx_line(&sn->sched, &lt, b, sizeof b);
    else b[0] = '\0';
    pnl_label_set_if_changed(s_ctx, b);
}

static void tiles_rebuild(const uint8_t *ids, int n) {
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (s_tile[i]) lv_obj_delete(s_tile[i]);
        s_tile[i] = s_dot[i] = s_l1[i] = s_l2[i] = s_l3[i] = NULL;
        s_tile_health[i] = -1;
    }
    s_ntiles = n;
    memcpy(s_tile_id, ids, (size_t)n);
    pnl_obj_show(s_none, n == 0);
    if (n == 0) return;
    pnl_band_t L;
    if (pnl_band_layout(n, PNL_BAND_WIDTH, &L) != 0) return;
    s_detail = L.detail;
    for (int k = 0; k < n; k++) {
        lv_obj_t *t = lv_obj_create(s_band);
        pnl_theme_card(t);
        lv_obj_set_size(t, L.tile_w, TILE_H);
        lv_obj_set_pos(t, L.x0 + k * (L.tile_w + L.gap), (BAND_H - TILE_H) / 2);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ids[k]);
        s_dot[k] = lv_obj_create(t);
        lv_obj_remove_style_all(s_dot[k]);
        lv_obj_set_size(s_dot[k], 16, 16);
        lv_obj_set_style_radius(s_dot[k], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dot[k], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_dot[k], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_dot[k], LV_ALIGN_TOP_RIGHT, 0, 4);
        s_l1[k] = pnl_label(t, "", L.detail == PNL_TILE_MIN ? &lv_font_montserrat_20 : &lv_font_montserrat_28, PNL_C_TEXT);
        lv_obj_align(s_l1[k], LV_ALIGN_TOP_LEFT, 0, 0);
        s_l2[k] = pnl_label(t, "", &lv_font_montserrat_28, PNL_C_TEXT);
        lv_obj_align(s_l2[k], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        if (L.detail == PNL_TILE_FULL) {
            s_l3[k] = pnl_label(t, "", &lv_font_montserrat_20, PNL_C_MUTED);
            lv_obj_align(s_l3[k], LV_ALIGN_BOTTOM_RIGHT, 0, -4);
        }
        s_tile[k] = t;
    }
}

/* The band follows the LIVE node list: one tile per used slot, rebuilt only
 * when the enrolled set (count or ids) changes. */
static void band_update(const pnl_snap_t *sn) {
    uint8_t ids[HG_MAX_ZONES];
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++)
        if (sn->st.node[i].used) ids[n++] = sn->st.node[i].id;
    if (n != s_ntiles || memcmp(ids, s_tile_id, (size_t)n) != 0) tiles_rebuild(ids, n);

    char b[32], r[16];
    for (int k = 0; k < s_ntiles; k++) {
        uint8_t id = s_tile_id[k];
        if (id < 1 || id > HG_MAX_ZONES || !s_tile[k]) continue;
        const hg_node_t *nd = &sn->st.node[id - 1];   /* slot = id - 1 (psvc_state_t) */
        if (!nd->used) continue;
        char name[17];
        if (s_detail == PNL_TILE_MIN) snprintf(name, sizeof name, "Z%u", (unsigned)nd->id);
        else pnl_zone_name(nd, name);
        pnl_label_set_if_changed(s_l1[k], name);
        if ((int)nd->health != s_tile_health[k]) {
            s_tile_health[k] = (int)nd->health;
            lv_obj_set_style_bg_color(s_dot[k], pnl_health_color(nd->health), 0);
        }
        pnl_readings_t rd;
        pnl_node_readings(nd, &rd);
        pnl_fmt_reading(&rd, 0, r, sizeof r);
        pnl_label_set_if_changed(s_l2[k], r);
        if (s_l3[k]) {
            pnl_fmt_reading(&rd, 1, r, sizeof r);
            snprintf(b, sizeof b, "Light %s", r);
            pnl_label_set_if_changed(s_l3[k], b);
        }
    }
}

static void alarm_update(const pnl_snap_t *sn) {
    int on = sn->st.alarms_active > 0 ? 1 : 0;
    if (on != s_alarm_on) {
        s_alarm_on = on;
        lv_anim_delete(s_band, band_opa_cb);
        if (on) {
            lv_obj_set_style_bg_opa(s_band, LV_OPA_COVER, 0);
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_band);
            lv_anim_set_values(&a, 0, 1000);
            lv_anim_set_duration(&a, 1000);
            lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
            lv_anim_set_exec_cb(&a, band_opa_cb);
            lv_anim_start(&a);
        } else {
            lv_obj_set_style_bg_opa(s_band, LV_OPA_TRANSP, 0);
        }
        pnl_obj_show(s_badge, on);
    }
    if (on) {
        char b[48];
        snprintf(b, sizeof b, "%d alarm(s) -- tap to view", sn->st.alarms_active);
        pnl_label_set_if_changed(s_badge, b);
    }
}

/* sn NULL (no shell copy yet) is treated exactly like started == 0: the top
 * bar, band and alarms wait for the first poll; the clock still ticks as unset. */
static void home_update(const pnl_snap_t *sn) {
    if (!s_band) return;
    if (sn && sn->started) {
        const psvc_state_t *st = &sn->st;
        char b[48];
        snprintf(b, sizeof b, "\xE2\x80\xA2 ring %s", state_snap_ring_state_name(st->ring.state));
        pnl_label_set_if_changed(s_ring, b);
        set_color_if_changed(s_ring, &s_ring_c, st->ring.state == RING_ST_OK ? PNL_C_OK_TEXT
                                              : st->ring.state == RING_ST_OPEN ? PNL_C_OFFLINE_TEXT : PNL_C_MUTED);
        pnl_label_set_if_changed(s_src, st->time_src);
        set_color_if_changed(s_wifi, &s_wifi_c, st->wifi.sta_up ? PNL_C_TEXT
                                              : st->wifi.ap_ssid[0] ? PNL_C_MUTED : PNL_C_OFFLINE_TEXT);
        band_update(sn);
        alarm_update(sn);
    }
    clock_tick(NULL);
}

static lv_obj_t *icon(lv_obj_t *page, const char *text, pnl_dest_t d, int x) {
    lv_obj_t *b = lv_button_create(page);
    lv_obj_set_size(b, 240, 90);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -14);
    lv_obj_t *l = pnl_label(b, text, &lv_font_montserrat_28, PNL_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    return b;
}

static void home_build(lv_obj_t *page, int arg) {
    (void)arg;
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    s_ring_c = s_wifi_c = 0xFFFFFFFFu;

    s_ring = pnl_label(page, "\xE2\x80\xA2 ring --", &lv_font_montserrat_20, PNL_C_MUTED);
    lv_obj_align(s_ring, LV_ALIGN_TOP_LEFT, HOME_MARGIN, 10);
    lv_obj_t *tr = lv_obj_create(page);
    lv_obj_remove_style_all(tr);
    lv_obj_set_size(tr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(tr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tr, 16, 0);
    lv_obj_remove_flag(tr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(tr, LV_ALIGN_TOP_RIGHT, -HOME_MARGIN, 10);
    s_src  = pnl_label(tr, "", &lv_font_montserrat_20, PNL_C_MUTED);
    s_wifi = pnl_label(tr, LV_SYMBOL_WIFI, &lv_font_montserrat_20, PNL_C_OFFLINE_TEXT);

    if (pnl_prefs_get()->face == 1) {
        s_clock = NULL;                                   /* the analogue face: no digital label */
        home_analogue_build(page);
        lv_obj_align(s_scale, LV_ALIGN_TOP_MID, 0, 20);   /* 200 px face ends at y 220, above the date line (y 222) */
    } else {
        s_clock = pnl_label(page, "--:--", pnl_font_clock(), PNL_C_TEXT);
        lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 36);
    }
    s_date = pnl_label(page, "", &lv_font_montserrat_48, PNL_C_TEXT);
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 222);
    s_ctx = pnl_label(page, "", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_align(s_ctx, LV_ALIGN_TOP_MID, 0, 284);

    s_band = lv_obj_create(page);
    lv_obj_remove_style_all(s_band);
    lv_obj_set_size(s_band, PNL_BAND_WIDTH, BAND_H);
    lv_obj_align(s_band, LV_ALIGN_TOP_MID, 0, BAND_Y);
    lv_obj_set_style_radius(s_band, 10, 0);
    lv_obj_set_style_bg_color(s_band, lv_color_hex(PNL_C_OFFLINE), 0);
    lv_obj_set_style_bg_opa(s_band, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_band, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_band, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_band, band_cb, LV_EVENT_CLICKED, NULL);
    s_none = pnl_label(s_band, "No zones enrolled", &lv_font_montserrat_28, PNL_C_MUTED);
    lv_obj_center(s_none);
    lv_obj_add_flag(s_none, LV_OBJ_FLAG_HIDDEN);

    s_badge = pnl_label(page, "", &lv_font_montserrat_20, PNL_C_OFFLINE_TEXT);
    lv_obj_align(s_badge, LV_ALIGN_TOP_MID, 0, BAND_Y + BAND_H + 4);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_badge, band_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_badge, LV_OBJ_FLAG_HIDDEN);

    icon(page, LV_SYMBOL_LIST " HillGrow", PNL_DEST_DASHBOARD, -260);
    icon(page, LV_SYMBOL_AUDIO " Audio", PNL_DEST_AUDIO, 0);
    icon(page, LV_SYMBOL_SETTINGS " Panel", PNL_DEST_PANEL, 260);

    s_clock_timer = lv_timer_create(clock_tick, 1000, NULL);
    home_update(pnl_shell_snap());
}

static void home_teardown(void) {
    if (s_clock_timer) { lv_timer_delete(s_clock_timer); s_clock_timer = NULL; }
    if (s_band) lv_anim_delete(s_band, band_opa_cb);
    s_ring = s_src = s_wifi = s_clock = s_date = s_ctx = s_band = s_none = s_badge = NULL;
    s_scale = s_hand_h = s_hand_m = s_hand_s = s_ana_unset = NULL;
    s_ana_shown = -1;
    for (int i = 0; i < HG_MAX_ZONES; i++) s_tile[i] = s_dot[i] = s_l1[i] = s_l2[i] = s_l3[i] = NULL;
    s_ntiles = -1;
    s_alarm_on = -1;
}

const pnl_screen_ops_t PNL_SCR_HOME = { "Home", home_build, home_update, home_teardown, 0 };
