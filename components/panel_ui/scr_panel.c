/* Glue: the Panel destination (spec naming: "Panel holds display-local preferences -- brightness, dim schedule, clock
 * face (including an analogue option), orientation, about"). Never a system setting: those live in System, as on the
 * web. Every change goes to pnl_prefs_set(), which saves on the worker; a save refused by NVS (full, or writes
 * disabled) leaves the value in effect until the next restart, and the top line says so.
 * Stale completions: the outcome of a save is kept in pnl_prefs_nvs (pnl_prefs_save_state), and the widget pointers,
 * cleared by panel_teardown, are the liveness test -- the sys_wifi.c pattern. The About lines read the shell's snapshot
 * (ruling C11: no copy of our own), which may be NULL. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"      /* CONFIG_LV_MEM_SIZE_KILOBYTES */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"  /* uxTaskGetStackHighWaterMark */
#include "lvgl.h"
#include "panel_hw.h"
#include "pnl_poll.h"       /* pnl_poll_stack_free */
#include "pnl_worker.h"     /* pnl_worker_stack_free */
#include "pnl_prefs_nvs.h"
#include "pnl_dim_hold.h"
#include "pnl_palette.h"
#include "pnl_ui_kit.h"
#include "pnl_fmt.h"        /* pnl_fmt_roller_opts */
#include "scr_shell.h"
#include "scr_system.h"     /* sys_reboot_confirm */

#define ABOUT_N 8

static const uint16_t IDLE_S[] = { 30, 60, 120, 300 };
static const char     IDLE_TXT[] = "30 s\n1 min\n2 min\n5 min";
static const uint16_t WIPE_S[] = { 120, 300, 600 };
static const char     WIPE_TXT[] = "2 min\n5 min\n10 min";
static const char    *MODE_TXT[3] = { "Off", "Follow the lights", "Fixed hours" };
static const char    *ORIENT_TXT[2] = { "Normal", "Flipped" };

static lv_obj_t *s_day_lbl, *s_night_lbl, *s_mode[3], *s_face[2], *s_orient[2], *s_orient_msg;
static lv_obj_t *s_start, *s_end, *s_save_msg, *s_about[ABOUT_N];
static char      s_hours[24 * 3 + 1];
static uint8_t   s_preview_dirty;             /* a slider moved and the value is not saved yet */
static int       s_save_shown = -1;           /* the pnl_prefs_save_state() on show; -1 = none drawn */

/* The CHECKED state is owned here alone: the buttons are NOT LV_OBJ_FLAG_CHECKABLE (LVGL toggles CHECKED on RELEASED
 * for those, so re-tapping the selected one would clear its highlight -- the System tabs lesson). */
static void set_checked(lv_obj_t **b, int n, int sel) {
    for (int i = 0; i < n; i++) {
        if (!b[i]) continue;
        if (i == sel) lv_obj_add_state(b[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(b[i], LV_STATE_CHECKED);
    }
}

static int index_of(const uint16_t *v, int n, uint16_t x, int dflt) {
    for (int i = 0; i < n; i++) if (v[i] == x) return i;
    return dflt;
}

static void save_msg_update(void) {
    pnl_prefs_save_state_t st = pnl_prefs_save_state();
    if (!s_save_msg || (int)st == s_save_shown) return;
    s_save_shown = (int)st;
    switch (st) {
    case PNL_PREFS_SAVE_PENDING: pnl_kit_msg_set(s_save_msg, "Saving...", PNL_KIT_INFO); break;
    case PNL_PREFS_SAVE_OK:      pnl_kit_msg_set(s_save_msg, "Saved", PNL_KIT_OK); break;
    case PNL_PREFS_SAVE_FAILED:  pnl_kit_msg_set(s_save_msg, "Not saved -- the panel's settings storage refused the "
                                                 "write. The change stays in effect until the next restart.",
                                                 PNL_KIT_ERR); break;
    default:                     pnl_kit_msg_set(s_save_msg, "", PNL_KIT_INFO); break;
    }
}

static void commit(const pnl_prefs_t *p) {
    pnl_prefs_set(p);
    save_msg_update();
}

static void orient_note(void) {
    if (!s_orient_msg) return;
    panel_hw_status_t hs;
    panel_hw_status(&hs);
    uint8_t want = pnl_prefs_get()->orient;
    char b[96];
    if (want == hs.orient) snprintf(b, sizeof b, "Applies after a restart.");
    else snprintf(b, sizeof b, "Now %s -- %s applies after a restart.", ORIENT_TXT[hs.orient ? 1 : 0],
                  ORIENT_TXT[want ? 1 : 0]);
    pnl_kit_msg_set(s_orient_msg, b, PNL_KIT_INFO);
}

/* ---- brightness: previewed live while dragged (held against the dimming timer), saved once on release ---- */
static void pct_labels(void) {
    const pnl_prefs_t *p = pnl_prefs_get();
    if (s_day_lbl) lv_label_set_text_fmt(s_day_lbl, "Day brightness %u %%", (unsigned)p->dim.day_pct);
    if (s_night_lbl) lv_label_set_text_fmt(s_night_lbl, "Night brightness %u %%", (unsigned)p->dim.night_pct);
}

static void preview(int night, int32_t v) {
    pnl_prefs_t p = *pnl_prefs_get();
    if (night) p.dim.night_pct = (uint8_t)v;
    else p.dim.day_pct = (uint8_t)v;
    pnl_prefs_preview(&p);                        /* clamps to 5..100 */
    s_preview_dirty = 1;
    pnl_dim_hold(PNL_DIM_PREVIEW_HOLD_MS);        /* ruling C21: the dimming timer keeps its hands off meanwhile */
    const pnl_prefs_t *q = pnl_prefs_get();
    (void)panel_hw_brightness(night ? q->dim.night_pct : q->dim.day_pct);   /* show the level being chosen */
    pct_labels();
}

static void day_changed(lv_event_t *e) { preview(0, lv_slider_get_value(lv_event_get_target_obj(e))); }
static void night_changed(lv_event_t *e) { preview(1, lv_slider_get_value(lv_event_get_target_obj(e))); }

static void preview_end(void) {
    if (!s_preview_dirty) return;
    s_preview_dirty = 0;
    pnl_dim_hold(0);
    (void)panel_hw_brightness(pnl_prefs_get()->dim.day_pct);   /* a panel in use is at day brightness */
    commit(pnl_prefs_get());                                   /* save once, on release */
}

static void slider_released(lv_event_t *e) { (void)e; preview_end(); }

/* ---- the choices ---- */
static void mode_click(lv_event_t *e) {
    uint8_t m = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    set_checked(s_mode, 3, m);
    if (pnl_prefs_get()->dim.mode == m) return;
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.mode = m;
    commit(&p);
}

/* Each roller writes only its own field, so a stored value the roller cannot show (it shows the default row) is kept
 * until the operator turns that roller. */
static void hours_changed(lv_event_t *e) {
    lv_obj_t *r = lv_event_get_target_obj(e);
    uint16_t v = (uint16_t)(lv_roller_get_selected(r) * 60u);
    pnl_prefs_t p = *pnl_prefs_get();
    uint16_t *f = r == s_start ? &p.dim.fixed_start_min : &p.dim.fixed_end_min;
    if (*f == v) return;
    *f = v;
    commit(&p);
}

static void idle_changed(lv_event_t *e) {
    uint32_t i = lv_roller_get_selected(lv_event_get_target_obj(e));
    if (i >= sizeof IDLE_S / sizeof IDLE_S[0] || pnl_prefs_get()->dim.idle_s == IDLE_S[i]) return;
    pnl_prefs_t p = *pnl_prefs_get();
    p.dim.idle_s = IDLE_S[i];
    commit(&p);
}

static void wipe_changed(lv_event_t *e) {
    uint32_t i = lv_roller_get_selected(lv_event_get_target_obj(e));
    if (i >= sizeof WIPE_S / sizeof WIPE_S[0] || pnl_prefs_get()->wipe_idle_s == WIPE_S[i]) return;
    pnl_prefs_t p = *pnl_prefs_get();
    p.wipe_idle_s = WIPE_S[i];
    commit(&p);
}

static void face_click(lv_event_t *e) {
    uint8_t f = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    set_checked(s_face, 2, f);
    if (pnl_prefs_get()->face == f) return;
    pnl_prefs_t p = *pnl_prefs_get();
    p.face = f;
    commit(&p);
}

static void orient_click(lv_event_t *e) {
    uint8_t o = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    set_checked(s_orient, 2, o);
    if (pnl_prefs_get()->orient != o) {
        pnl_prefs_t p = *pnl_prefs_get();
        p.orient = o;
        commit(&p);
    }
    orient_note();
}

/* The save job is queued before the reboot job, and the one worker runs jobs in order, so a restart confirmed at once
 * still boots with the new orientation (unless NVS refused the write: then the top line says so). */
static void restart_click(lv_event_t *e) { (void)e; sys_reboot_confirm("to apply the new orientation"); }
static void touch_click(lv_event_t *e) { (void)e; pnl_nav_go(PNL_DEST_DIAG, 0); }

static lv_obj_t *slider(lv_obj_t *parent, uint8_t v, lv_event_cb_t changed) {
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_width(s, LV_PCT(90));
    lv_slider_set_range(s, 5, 100);               /* D4: never a dark panel */
    lv_slider_set_value(s, v, LV_ANIM_OFF);
    lv_obj_add_event_cb(s, changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, slider_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s, slider_released, LV_EVENT_PRESS_LOST, NULL);   /* preview_end runs once either way */
    return s;
}

static lv_obj_t *choice(lv_obj_t *row, const char *text, lv_event_cb_t cb, int i) {
    return pnl_kit_button(row, text, cb, (void *)(intptr_t)i);
}

static void panel_update(const pnl_snap_t *s);

static void panel_build(lv_obj_t *content, int arg) {
    (void)arg;
    const pnl_prefs_t *p = pnl_prefs_get();
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(content, 72, 0);     /* clear of the shell's "Home" button (top left) */
    lv_obj_set_style_pad_hor(content, 16, 0);
    lv_obj_set_style_pad_bottom(content, 16, 0);
    lv_obj_set_style_pad_row(content, 12, 0);
    s_preview_dirty = 0;
    s_save_shown = -1;

    s_save_msg = pnl_kit_msg(content);
    save_msg_update();

    lv_obj_t *b = pnl_kit_card(content, "Brightness");
    s_day_lbl = lv_label_create(b);
    (void)slider(b, p->dim.day_pct, day_changed);
    s_night_lbl = lv_label_create(b);
    (void)slider(b, p->dim.night_pct, night_changed);
    pct_labels();

    lv_obj_t *d = pnl_kit_card(content, "Night dimming");
    lv_obj_t *r = pnl_kit_row(d);
    for (int i = 0; i < 3; i++) s_mode[i] = choice(r, MODE_TXT[i], mode_click, i);
    set_checked(s_mode, 3, p->dim.mode);
    if (!s_hours[0]) (void)pnl_fmt_roller_opts(s_hours, sizeof s_hours, 0, 24, 2);
    lv_obj_t *hr = pnl_kit_row(d);
    lv_obj_t *l = lv_label_create(hr); lv_label_set_text(l, "Fixed hours: dim from");
    s_start = pnl_kit_roller(hr, s_hours, p->dim.fixed_start_min / 60u, hours_changed, NULL);
    l = lv_label_create(hr); lv_label_set_text(l, "to");
    s_end = pnl_kit_roller(hr, s_hours, p->dim.fixed_end_min / 60u, hours_changed, NULL);
    lv_obj_t *ir = pnl_kit_row(d);
    l = lv_label_create(ir); lv_label_set_text(l, "Dim after idle");
    (void)pnl_kit_roller(ir, IDLE_TXT, (uint32_t)index_of(IDLE_S, 4, p->dim.idle_s, 1), idle_changed, NULL);
    l = lv_label_create(ir); lv_label_set_text(l, "Return to Home and wipe after");
    (void)pnl_kit_roller(ir, WIPE_TXT, (uint32_t)index_of(WIPE_S, 3, p->wipe_idle_s, 1), wipe_changed, NULL);
    lv_obj_t *note = pnl_kit_msg(d);
    pnl_kit_msg_set(note, "Follow the lights: dim while every scheduled shelf light is off. The first touch on a dimmed "
                          "panel only wakes it.", PNL_KIT_INFO);

    lv_obj_t *f = pnl_kit_card(content, "Clock face");
    r = pnl_kit_row(f);
    s_face[0] = choice(r, "Digital", face_click, 0);
    s_face[1] = choice(r, "Analogue", face_click, 1);
    set_checked(s_face, 2, p->face);

    lv_obj_t *o = pnl_kit_card(content, "Orientation");
    r = pnl_kit_row(o);
    for (int i = 0; i < 2; i++) s_orient[i] = choice(r, ORIENT_TXT[i], orient_click, i);
    set_checked(s_orient, 2, p->orient);
    s_orient_msg = pnl_kit_msg(o);
    orient_note();
    pnl_kit_button(o, "Restart now", restart_click, NULL);

    lv_obj_t *a = pnl_kit_card(content, "About");
    for (int i = 0; i < ABOUT_N; i++) s_about[i] = pnl_kit_msg(a);
    pnl_kit_button(a, "Touch test", touch_click, NULL);
    panel_update(pnl_shell_snap());               /* the local lines at once; the snapshot lines if a poll landed */
}

static void panel_update(const pnl_snap_t *s) {
    save_msg_update();                            /* a save's outcome lands within one poll */
    if (!s_about[0]) return;
    const psvc_state_t *st = (s && s->started) ? &s->st : NULL;   /* NULL or no poll yet: "--" */
    char b[160];
    snprintf(b, sizeof b, "Version %s", st ? st->version : "--");
    pnl_label_set_if_changed(s_about[0], b);
    if (st) snprintf(b, sizeof b, "Firmware: slot %s, %s%s", st->fw_slot, st->fw_state,
                     strcmp(st->fw_state, "PENDING") == 0 ? " -- OTA trial in progress (a restart retires this image)"
                                                          : "");
    else snprintf(b, sizeof b, "Firmware: --");
    pnl_label_set_if_changed(s_about[1], b);
    if (st) snprintf(b, sizeof b, "STA IP %s | AP IP %s", st->wifi.sta_up ? st->wifi.sta_ip : "--",
                     st->wifi.ap_ip[0] ? st->wifi.ap_ip : "--");
    else snprintf(b, sizeof b, "STA IP -- | AP IP --");
    pnl_label_set_if_changed(s_about[2], b);
    if (st) snprintf(b, sizeof b, "Uptime %lu s", (unsigned long)st->uptime_s);
    else snprintf(b, sizeof b, "Uptime --");
    pnl_label_set_if_changed(s_about[3], b);
    if (st) snprintf(b, sizeof b, "Internal RAM %lu KB free, %lu KB minimum", (unsigned long)st->heap_int_free_kb,
                     (unsigned long)st->heap_int_min_kb);
    else snprintf(b, sizeof b, "Internal RAM --");
    pnl_label_set_if_changed(s_about[4], b);
    /* The figure every stage gate records (Global Constraints "Memory"): the budget is the INTERNAL pool, 75 % of
     * CONFIG_LV_MEM_SIZE_KILOBYTES; mon.total_size also counts Task 7's PSRAM overflow pool, so it is not the
     * denominator (the scr_diag.c line). */
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    snprintf(b, sizeof b, "LVGL pool: max used %u of %u KB internal (+%u KB PSRAM)", (unsigned)(mon.max_used / 1024u),
             (unsigned)CONFIG_LV_MEM_SIZE_KILOBYTES,
             (unsigned)(mon.total_size / 1024u > CONFIG_LV_MEM_SIZE_KILOBYTES
                        ? mon.total_size / 1024u - CONFIG_LV_MEM_SIZE_KILOBYTES : 0));
    pnl_label_set_if_changed(s_about[5], b);
    panel_hw_status_t hs;
    panel_hw_status(&hs);
    snprintf(b, sizeof b, "Touch: %lu reads, %lu errors, %lu points, last %u,%u%s", (unsigned long)hs.reads,
             (unsigned long)hs.read_errs, (unsigned long)hs.points, (unsigned)hs.last_x, (unsigned)hs.last_y,
             hs.touch_ok ? "" : " (touch unavailable)");
    pnl_label_set_if_changed(s_about[6], b);
    /* Stack margins (the task stacks are chosen, not measured; the Stage 3 and 4 gates fail below 1024 B). This
     * runs on the LVGL task, so NULL is the LVGL task itself. */
    uint32_t sp = 0, sw = 0;
    pnl_poll_stack_free(&sp, &sw);
    snprintf(b, sizeof b, "Stack free (min, bytes): lvgl %lu, pnl_work %lu, pnl_poll %lu, pnl_wifi %lu",
             (unsigned long)uxTaskGetStackHighWaterMark(NULL), (unsigned long)pnl_worker_stack_free(),
             (unsigned long)sp, (unsigned long)sw);
    pnl_label_set_if_changed(s_about[7], b);
}

static void panel_teardown(void) {
    preview_end();                                /* a drag cut short by a navigation is still saved */
    s_day_lbl = s_night_lbl = s_start = s_end = s_save_msg = s_orient_msg = NULL;
    for (int i = 0; i < 3; i++) s_mode[i] = NULL;
    for (int i = 0; i < 2; i++) { s_face[i] = NULL; s_orient[i] = NULL; }
    for (int i = 0; i < ABOUT_N; i++) s_about[i] = NULL;
    s_save_shown = -1;
}

const pnl_screen_ops_t PNL_SCR_PANEL = { .title = "Panel", .build = panel_build, .update = panel_update,
                                         .teardown = panel_teardown, .in_rail = 0 };
