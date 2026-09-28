/* scr_config.c -- the Config destination (glue; LVGL task only).
 * Owns the picker (Master + enrolled zones), the title, the status line and the routing to an editor. The
 * generated-editor frame both editors share (tabs, index selector, Save, rows, cfg_save_*) is cfg_frame.c; the
 * editors (cfg_zone.c, cfg_master.c) own their documents, edit sets and worker jobs. */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "scr_config.h"
#include "cfg_frame.h"
#include "scr_shell.h"
#include "pnl_fmt.h"
#include "pnl_palette.h"
#include "wdg_keyboard.h"

static const char *TAG = "scr_config";

#define PICK_TXT     24

static struct {
    lv_obj_t *title, *picker, *body, *status;
    int       open_zone;                 /* -1 none, 0 master (Task 21), 1..8 a zone */
    uint16_t  used_mask;
    int       n_pick;
    uint8_t   zone_of[1 + HG_MAX_ZONES];
    uint8_t   route_pending;             /* built before the first poll: route on the first started snapshot */
    int       route_arg;
} s_ui = { .open_zone = -1 };

/* button-matrix maps must outlive their widgets: file scope, never cleared */
static char        s_pick_txt[1 + HG_MAX_ZONES][PICK_TXT];
static const char *s_pick_map[1 + HG_MAX_ZONES + 1];

static int s_last_zone = -1;                                        /* "-1 = last used" */
/* No snapshot copy here (controller ruling C11): the shell's PSRAM copy, pnl_shell_snap(), may be NULL. */

/* ---------- title / status ---------- */
void cfg_set_title(const char *text) {
    if (s_ui.title) pnl_label_set_if_changed(s_ui.title, text ? text : "");
}
void cfg_set_status(const char *text, int is_error) {
    if (!s_ui.status) return;
    pnl_label_set_if_changed(s_ui.status, text ? text : "");
    lv_obj_set_style_text_color(s_ui.status, lv_color_hex(is_error ? PNL_C_OFFLINE_TEXT : PNL_C_MUTED), 0);
}

/* ---------- picker ---------- */
static int zone_used(const pnl_snap_t *s, int z) {   /* node ids are untrusted: slot = id-1 (psvc_state_fill) */
    return s && z >= 1 && z <= HG_MAX_ZONES && s->st.node[z - 1].used && s->st.node[z - 1].id == (uint8_t)z;
}
static uint16_t used_mask(const pnl_snap_t *s) {
    uint16_t m = 0;
    for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) m |= (uint16_t)(1u << z);
    return m;
}
static void picker_check(int zone) {
    if (!s_ui.picker) return;
    lv_buttonmatrix_clear_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_CHECKED);
    for (int i = 0; i < s_ui.n_pick; i++)
        if (zone >= 0 && s_ui.zone_of[i] == zone) lv_buttonmatrix_set_button_ctrl(s_ui.picker, (uint32_t)i, LV_BUTTONMATRIX_CTRL_CHECKED);
}
static int master_enabled(void);
static void picker_rebuild(const pnl_snap_t *s) {   /* s NULL (no snapshot yet): Master only */
    int n = 0;
    snprintf(s_pick_txt[0], PICK_TXT, "Master");
    s_pick_map[n] = s_pick_txt[0];
    s_ui.zone_of[n++] = 0;
    for (int z = 1; z <= HG_MAX_ZONES; z++) {
        if (!zone_used(s, z)) continue;
        char nm[17];
        pnl_zone_name(&s->st.node[z - 1], nm);
        snprintf(s_pick_txt[n], PICK_TXT, "%s", nm);
        s_pick_map[n] = s_pick_txt[n];
        s_ui.zone_of[n++] = (uint8_t)z;
    }
    s_pick_map[n] = "";
    s_ui.n_pick = n;
    s_ui.used_mask = used_mask(s);
    lv_buttonmatrix_set_map(s_ui.picker, s_pick_map);
    lv_buttonmatrix_clear_button_ctrl_all(s_ui.picker, LV_BUTTONMATRIX_CTRL_DISABLED);   /* same count keeps old bits */
    lv_buttonmatrix_set_button_ctrl_all(s_ui.picker, CFG_BTNM_CTRL);
    if (!master_enabled()) lv_buttonmatrix_set_button_ctrl(s_ui.picker, 0, LV_BUTTONMATRIX_CTRL_DISABLED);
    picker_check(s_ui.open_zone);
}

/* ---------- editor routing: the master (zone 0) and zones 1..8 ---------- */
static int master_enabled(void) { return 1; }
static void close_editor(void) {
    if (s_ui.open_zone >= 1) cfg_zone_close();
    else if (s_ui.open_zone == 0) cfg_master_close();
    s_ui.open_zone = -1;
    cfg_card_teardown();                        /* ruling C2: the card buttons go with the frame */
    cfg_frame_forget();
}
static void open_editor(int zone) {
    close_editor();
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    lv_obj_clean(s_ui.body);
    cfg_set_status("", 0);
    s_ui.open_zone = zone;
    picker_check(zone);
    s_last_zone = zone;
    if (zone >= 1) {
        cfg_card_set_zone((uint8_t)zone);       /* before the open: the frame (and its card buttons) may build inside it */
        cfg_zone_open(s_ui.body, (uint8_t)zone);
    } else {
        cfg_card_set_zone(0);
        cfg_master_open(s_ui.body);
    }
}
static int pick_target(const pnl_snap_t *s, int arg) {
    if (arg == 0) return 0;
    if (arg >= 1 && zone_used(s, arg)) return arg;
    if (s_last_zone == 0) return 0;
    if (s_last_zone >= 1 && zone_used(s, s_last_zone)) return s_last_zone;
    for (int z = 1; z <= HG_MAX_ZONES; z++) if (zone_used(s, z)) return z;
    return 0;                                   /* nothing enrolled: the master is always there */
}
static void ev_pick(lv_event_t *e) {
    (void)e;
    uint32_t sel = lv_buttonmatrix_get_selected_button(s_ui.picker);
    if (sel >= (uint32_t)s_ui.n_pick) return;
    int z = s_ui.zone_of[sel];
    if (z == s_ui.open_zone) { picker_check(z); return; }
    open_editor(z);
}
static void route_update(const pnl_snap_t *snap) {
    if (s_ui.open_zone >= 1) cfg_zone_update(snap);
    else if (s_ui.open_zone == 0) cfg_master_update(snap);
    else open_editor(pick_target(snap, -1));
}
/* ---------- end of editor routing ---------- */

/* ---------- the destination ---------- */
static void route_first(const pnl_snap_t *sn, int arg) {   /* the build's routing, once a snapshot has started */
    int z = pick_target(sn, arg);
    open_editor(z);
    if (arg >= 1 && z != arg) {
        char m[48];
        snprintf(m, sizeof m, "Zone %d is not enrolled.", arg);
        cfg_set_status(m, 1);
    }
}
static void cfg_build(lv_obj_t *content, int arg) {
    int64_t t0 = esp_timer_get_time();
    s_ui.open_zone = -1; s_ui.n_pick = 0; s_ui.used_mask = 0; s_ui.route_pending = 0;
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 8, 0);
    lv_obj_set_style_pad_row(content, 6, 0);
    s_ui.title = lv_label_create(content);
    lv_obj_set_style_text_font(s_ui.title, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_ui.title, "Config");
    s_ui.picker = lv_buttonmatrix_create(content);
    lv_buttonmatrix_set_one_checked(s_ui.picker, true);
    lv_obj_set_size(s_ui.picker, lv_pct(100), 56);
    lv_obj_add_event_cb(s_ui.picker, ev_pick, LV_EVENT_VALUE_CHANGED, NULL);
    s_ui.body = lv_obj_create(content);
    lv_obj_remove_style_all(s_ui.body);
    lv_obj_set_width(s_ui.body, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.body, 1);
    lv_obj_set_flex_flow(s_ui.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.body, 6, 0);
    s_ui.status = lv_label_create(content);
    lv_obj_set_width(s_ui.status, lv_pct(100));
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(s_ui.status, "");
    const pnl_snap_t *sn = pnl_shell_snap();
    if (sn && sn->started) {
        picker_rebuild(sn);
        route_first(sn, arg);
    } else {                                     /* NULL = no shell copy yet: the same as "not started" */
        picker_rebuild(NULL);
        lv_label_set_text(lv_label_create(s_ui.body), "Starting...");
        s_ui.route_pending = 1;
        s_ui.route_arg = arg;
    }
    int64_t dt = esp_timer_get_time() - t0;
    if (dt > CFG_SLOW_US) ESP_LOGW(TAG, "build took %lld ms", (long long)(dt / 1000));
}
static void cfg_update(const pnl_snap_t *snap) {
    if (!s_ui.picker || !snap || !snap->started) return;
    if (used_mask(snap) != s_ui.used_mask) picker_rebuild(snap);
    if (s_ui.route_pending) { s_ui.route_pending = 0; route_first(snap, s_ui.route_arg); return; }
    route_update(snap);
}
static void cfg_teardown(void) {
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    close_editor();
    s_ui.title = NULL; s_ui.picker = NULL; s_ui.body = NULL; s_ui.status = NULL;
    s_ui.route_pending = 0;
}

const pnl_screen_ops_t PNL_SCR_CONFIG = {
    .title = "Config", .build = cfg_build, .update = cfg_update, .teardown = cfg_teardown, .in_rail = 1,
};
