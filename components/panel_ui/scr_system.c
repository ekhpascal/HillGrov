#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

static const char *TAG = "scr_system";

typedef struct { const char *title; const pnl_sys_section_t *sec; } sys_row_t;
static const sys_row_t SYS_ROWS[PNL_SYS_SEC_COUNT] = {
    [PNL_SYS_SEC_WIFI]     = { "Wi-Fi",    &PNL_SYS_WIFI },
    [PNL_SYS_SEC_TIME]     = { "Time",     &PNL_SYS_TIME },
    [PNL_SYS_SEC_PASSWORD] = { "Password", &PNL_SYS_PASSWORD },
    [PNL_SYS_SEC_FLEET]    = { "Fleet",    &PNL_SYS_PLACEHOLDER },
    [PNL_SYS_SEC_FIRMWARE] = { "Firmware", &PNL_SYS_PLACEHOLDER },
};

static lv_obj_t   *s_body, *s_tab[PNL_SYS_SEC_COUNT];
static int         s_cur = -1, s_last = PNL_SYS_SEC_WIFI;
static const char *s_cur_title = "";

/* ---- the placeholder section (Tasks 25/32 replace it row by row) ---- */
static void ph_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, s_cur_title);
    lv_obj_t *m = pnl_kit_msg(c);
    char b[64];
    snprintf(b, sizeof b, "%s: not available yet", s_cur_title);
    pnl_kit_msg_set(m, b, PNL_KIT_INFO);
}
const pnl_sys_section_t PNL_SYS_PLACEHOLDER = { .title = "", .build = ph_build, .update = NULL, .teardown = NULL };

static void open_section(int i) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->teardown) SYS_ROWS[s_cur].sec->teardown();
    lv_obj_clean(s_body);
    s_cur = i;
    s_last = i;
    s_cur_title = SYS_ROWS[i].title;
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) {
        if (k == i) lv_obj_add_state(s_tab[k], LV_STATE_CHECKED);
        else lv_obj_remove_state(s_tab[k], LV_STATE_CHECKED);
    }
    int64_t t0 = esp_timer_get_time();
    SYS_ROWS[i].sec->build(s_body);
    /* the first update() is fed from the shell's PSRAM copy (ruling C11: screens keep no snapshot of their own); it is
     * current at build (the shell refreshed it) and within one 250 ms tick at a section switch */
    const pnl_snap_t *sn = pnl_shell_snap();
    if (sn && sn->started && SYS_ROWS[i].sec->update) SYS_ROWS[i].sec->update(sn);
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    if (ms > 200) ESP_LOGW(TAG, "System section %s built in %lld ms (budget 200 ms)", s_cur_title, (long long)ms);
}

static void tab_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i != s_cur) open_section(i);
}

static void sys_build(lv_obj_t *content, int arg) {
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *tabs = pnl_kit_row(content);
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) {
        /* not LV_OBJ_FLAG_CHECKABLE: LVGL toggles CHECKED on RELEASED for a checkable object, so re-tapping the active
         * tab (tab_click ignores it) would clear its highlight; open_section() owns the CHECKED state alone */
        s_tab[k] = pnl_kit_button(tabs, SYS_ROWS[k].title, tab_click, (void *)(intptr_t)k);
    }
    s_body = lv_obj_create(content);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_flex_grow(s_body, 1);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_body, 0, 0);
    s_cur = -1;
    open_section((arg >= 0 && arg < PNL_SYS_SEC_COUNT) ? arg : s_last);
}

static void sys_update(const pnl_snap_t *snap) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->update) SYS_ROWS[s_cur].sec->update(snap);
}

static void sys_teardown(void) {
    if (s_cur >= 0 && SYS_ROWS[s_cur].sec->teardown) SYS_ROWS[s_cur].sec->teardown();
    s_cur = -1;
    s_body = NULL;
    for (int k = 0; k < PNL_SYS_SEC_COUNT; k++) s_tab[k] = NULL;
}

const pnl_screen_ops_t PNL_SCR_SYSTEM = { .title = "System", .build = sys_build, .update = sys_update,
                                          .teardown = sys_teardown, .in_rail = 1 };
