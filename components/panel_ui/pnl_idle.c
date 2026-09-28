/* Night dimming, the wake-only first touch and the idle operator-state wipe (D4, D18).
 *
 * The adapter's auto_sleep stays disabled: it pauses the LVGL worker and would freeze the clock. Dimming is only a
 * lower backlight duty; the LVGL task keeps turning. Everything here runs on the LVGL task (one 1 s lv_timer and the
 * catcher's event callback). */
#include <stdint.h>
#include <time.h>
#include "esp_log.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_dim.h"
#include "pnl_dim_hold.h"
#include "pnl_prefs_nvs.h"
#include "pnl_theme.h"       /* pnl_confirm_close */
#include "pnl_worker.h"
#include "scr_shell.h"
#include "scr_config.h"      /* cfg_zone_wipe_all, cfg_master_wipe */
#include "scr_system.h"      /* sys_wifi_wipe, sys_time_wipe, sys_password_wipe */
#include "zone_sections.h"   /* zone_console_wipe_all, zone_replace_wipe */
#include "wdg_keyboard.h"
#include "pnl_idle.h"

static const char *TAG = "pnl_idle";

#define IDLE_TICK_MS 1000
#define PCT_NONE     0xFF     /* no level applied yet, or a preview changed the duty behind our back (C21) */

static uint8_t   s_cur_pct = PCT_NONE;   /* last duty this module applied */
static uint8_t   s_wiped;                /* the wipe ran in this idle stretch; re-armed by the next activity */
static lv_obj_t *s_catcher;
static lv_timer_t *s_timer;

/* The decision for "now" with the given inactive time. The clock rule lives in pnl_dim_eval: the local time is valid
 * only while the snapshot says hg_app_time_is_set(), so an unset clock (or no snapshot yet) is day brightness. */
static void dim_decide(uint32_t idle_ms, pnl_dim_out_t *out) {
    const pnl_snap_t *sn = pnl_shell_snap();          /* the ONE LVGL-side copy (C11); NULL = treat as not started */
    int ok = sn && sn->started;
    pnl_local_t lt;
    pnl_local_time((int64_t)time(NULL), ok ? sn->st.utc_offset_s : 0, ok ? sn->st.time_is_set : 0, &lt);
    pnl_dim_eval(&pnl_prefs_get()->dim, ok ? &sn->sched : NULL, &lt, idle_ms, out);
}

/* Ruling C21 (pnl_dim_hold.h): while a Panel-screen preview holds the backlight, never touch it, and forget the
 * cached level so the first tick after the hold re-applies even an unchanged level. Returns 1 when the backlight is
 * at pct (applied now or already), 0 when held or the write failed (a dark panel: panel_hw_brightness -1). */
static int dim_apply(uint8_t pct) {
    if (pnl_dim_held(lv_tick_get())) { s_cur_pct = PCT_NONE; return 0; }
    if (pct == s_cur_pct) return 1;
    if (panel_hw_brightness(pct) != 0) { s_cur_pct = PCT_NONE; return 0; }
    s_cur_pct = pct;
    return 1;
}

static void catcher_pressed(lv_event_t *e) {
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);          /* this press and its release reach nothing else: wake only */
    if (s_catcher) { lv_obj_delete_async(s_catcher); s_catcher = NULL; }   /* not inside its own event: async */
    lv_display_trigger_activity(NULL);
    pnl_dim_out_t out;
    dim_decide(0, &out);                        /* in use now: the day level (clamped), unless the clock says OFF */
    (void)dim_apply(out.pct);
}

static void catcher_show(int on) {
    if (on && !s_catcher) {
        s_catcher = lv_obj_create(lv_layer_top());
        if (!s_catcher) return;
        lv_obj_remove_style_all(s_catcher);     /* fully transparent, no border */
        lv_obj_set_size(s_catcher, LV_PCT(100), LV_PCT(100));
        lv_obj_remove_flag(s_catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_catcher, catcher_pressed, LV_EVENT_PRESSED, NULL);
    } else if (!on && s_catcher) {
        lv_obj_delete(s_catcher);
        s_catcher = NULL;
    }
    if (s_catcher) {                            /* above the keyboard / a confirm opened after it (same top layer) */
        lv_obj_t *top = lv_layer_top();
        if (lv_obj_get_index(s_catcher) != (int32_t)lv_obj_get_child_count(top) - 1) lv_obj_move_foreground(s_catcher);
    }
}

/* D18: the web's logout wipe, applied at the glass. Every section hook panel_ui has (found by grepping panel_ui for
 * *_wipe / *_wipe_all): each is safe whether or not its screen is showing, and each leaves buffers a job still owns to
 * the worker -- the caller only runs this with no job pending anyway. Revealed secrets: cfg_master_wipe re-renders an
 * open master editor (its rows are deleted, which re-masks), and the Home navigation tears the open screen down. */
static void idle_wipe(void) {
    cfg_zone_wipe_all();        /* every zone's edit set, loaded doc and kept save outcome */
    cfg_master_wipe();          /* master edits (pending secrets included), frozen copy, scratch, kept outcome */
    zone_console_wipe_all();    /* every zone's transcript, history and draft */
    zone_replace_wipe();        /* every zone's MAC draft and the shown reply */
    sys_wifi_wipe();            /* Wi-Fi form text, passwords, scan list, kept outcomes */
    sys_time_wipe();            /* the TZ draft and kept outcomes */
    sys_password_wipe();        /* the unsubmitted new web password and its "Web password changed" box */
    sys_fleet_wipe();           /* the kept fleet outcome; a failed-reboot overlay (a "Rebooting..." one stays) */
    pnl_confirm_close();        /* the ONE confirm (C16): any box still open */
    if (wdg_keyboard_is_open()) wdg_keyboard_close();   /* wipes its text; re-masks */
    pnl_nav_go(PNL_DEST_HOME, 0);
    ESP_LOGI(TAG, "idle: operator state wiped, back to Home");
}

static void idle_tick(lv_timer_t *tm) {
    (void)tm;
    const pnl_prefs_t *p = pnl_prefs_get();
    uint32_t idle = lv_display_get_inactive_time(NULL);
    pnl_dim_out_t out, day;
    dim_decide(idle, &out);
    dim_decide(0, &day);                         /* the "in use" level: dimmed means below it */
    int applied = dim_apply(out.pct);
    catcher_show(applied && out.pct < day.pct);  /* only over a backlight really at the dim level: never while a
                                                    preview holds it, never after a failed write */

    if (idle >= (uint32_t)p->wipe_idle_s * 1000u) {
        if (!s_wiped && !pnl_worker_pending()) {   /* never wipe a buffer a job still owns; retried every tick */
            idle_wipe();
            s_wiped = 1;
        }
    } else {
        s_wiped = 0;
    }
}

void pnl_idle_start(void) {
    if (s_timer) return;
    s_timer = lv_timer_create(idle_tick, IDLE_TICK_MS, NULL);
    if (!s_timer) ESP_LOGE(TAG, "no LVGL timer -- night dimming and the idle wipe are off");
}
