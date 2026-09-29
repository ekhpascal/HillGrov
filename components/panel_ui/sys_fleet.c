/* Glue: System -> Fleet, the web's Fleet and Reboot cards (web/app.js:989-1015) and fleetUpdate / fleetUpdateAll /
 * fleetAbort / rebootMaster (app.js:1909-1954). The worker calls psvc_fleet_* (the same calls and the same refusal
 * tokens as POST/DELETE /api/fleet); Update and Update all are enabled only while the fleet line is "IDLE", Abort only
 * while it is not (app.js:990-1008), and all three stay disabled until the first poll delivers a fleet line.
 * The ONE reboot flow for the whole panel lives here (D16): sys_reboot_confirm() -- a confirm, a second confirm while
 * the running slot is on OTA trial (any reset during a trial retires the new image), then a full-screen "Rebooting..."
 * shown before the reply, exactly as the web does (REBOOT CONFIRM restarts at once, so the reply rarely arrives).
 * Stale completions: the sys_wifi.c note -- the outcome is kept in module state, the widget pointers (cleared by
 * fleet_teardown) are the liveness test, not screen_gen. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lvgl.h"
#include "ring_proto.h"     /* HG_MAX_ZONES, hg_node_t */
#include "psvc_fleet.h"
#include "pnl_fmt.h"        /* pnl_zone_name */
#include "pnl_msg.h"
#include "pnl_cmd.h"
#include "pnl_poll.h"       /* pnl_poll_kick */
#include "pnl_worker.h"
#include "pnl_palette.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

enum { OP_ZONE = 0, OP_ALL, OP_ABORT };
typedef struct { uint8_t op, zone; } fleet_arg_t;

static uint8_t  s_busy, s_idle, s_last_err;
static uint8_t  s_have_status;              /* a poll has delivered a fleet line since this section was built */
static char     s_last[96];                 /* the last outcome, kept for a rebuilt section */
static int      s_rows_mask = -1;           /* -1 = rows not built yet (0xFF is a real mask: 8 enrolled zones) */
static int      s_trial_shown = -1;         /* -1 = trial line not drawn yet */
static lv_obj_t *s_status, *s_rows, *s_all_btn, *s_abort_btn, *s_msg, *s_fw_lbl, *s_trial_lbl;
static lv_obj_t *s_upd[HG_MAX_ZONES], *s_name[HG_MAX_ZONES];

/* ---- the one reboot flow (D16) ---- */
static lv_obj_t *s_overlay, *s_overlay_lbl;   /* on the top layer: outlives any screen until Close (or the reset) */
static uint8_t   s_overlay_failed;             /* the overlay shows a refusal and a Close button, not "Rebooting..." */

static void overlay_close(lv_event_t *e) {
    (void)e;
    if (s_overlay) lv_obj_delete_async(s_overlay);   /* the Close button lives inside it: never delete synchronously */
    s_overlay = s_overlay_lbl = NULL;
    s_overlay_failed = 0;
}

static void overlay_fail(const char *text) {
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_overlay_lbl, text);
    pnl_kit_button(s_overlay, "Close", overlay_close, NULL);
    s_overlay_failed = 1;
}

static void reboot_run(pnl_job_t *j) { j->irc = pnl_cmd_run("REBOOT CONFIRM", (char *)j->out, PNL_JOB_OUT_MAX); }

static void reboot_done(pnl_job_t *j) {
    /* OK means esp_restart() is already under way (cmd_common.c h_reboot): keep "Rebooting...". Only a real rejection
     * overrides the optimistic text (app.js:1920-1923). */
    if (j->irc == 0 || !s_overlay_lbl) return;
    char r[PNL_JOB_OUT_MAX];
    memcpy(r, j->out, sizeof r);            /* pnl_cmd_run NUL-terminates within out */
    r[sizeof r - 1] = '\0';
    pnl_fmt_trim_eol(r);                    /* the ERR line ends with '\n' */
    overlay_fail(r[0] ? r : "Reboot failed");
}

static void reboot_go(void *ctx) {
    (void)ctx;
    if (s_overlay) return;                  /* one reboot at a time */
    s_overlay = lv_obj_create(lv_layer_top());
    if (!s_overlay) return;
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(PNL_C_BG), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_flex_flow(s_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_overlay_lbl = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_overlay_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_overlay_lbl, lv_color_hex(PNL_C_TEXT), 0);
    lv_label_set_text(s_overlay_lbl, "Rebooting...");          /* shown before the reply, like the web */
    if (pnl_worker_submit(reboot_run, reboot_done, NULL, 0) != 0) overlay_fail("Panel busy -- reboot not sent");
}

static void reboot_trial_check(void *ctx) {
    (void)ctx;
    /* the shell's copy (ruling C11), at most one poll old; a trial cannot START in that window (it needs a reset) */
    const pnl_snap_t *s = pnl_shell_snap();
    if (s && s->started && strcmp(s->st.fw_state, "PENDING") != 0) { reboot_go(NULL); return; }
    if (s && s->started)
        pnl_confirm("OTA trial in progress", "An OTA trial is in progress -- rebooting now retires the new image "
                    "and the master boots the previous firmware.", "Reboot anyway", reboot_go, NULL, NULL);
    else    /* no snapshot yet (the first second after boot, which is exactly when a trial runs): assume one may */
        pnl_confirm("Firmware state unknown", "If an OTA trial is in progress, rebooting now retires the new image.",
                    "Reboot anyway", reboot_go, NULL, NULL);
}

void sys_reboot_confirm(const char *why) {
    char text[128];
    if (why && why[0]) snprintf(text, sizeof text, "Reboot the master %s?", why);
    else snprintf(text, sizeof text, "Reboot the master?");
    pnl_confirm("Reboot", text, "Reboot", reboot_trial_check, NULL, NULL);
}

/* ---- fleet ---- */
static void fleet_run(pnl_job_t *j) {
    const fleet_arg_t *a = (const fleet_arg_t *)j->arg;
    j->rc = a->op == OP_ZONE ? psvc_fleet_zone(a->zone) : a->op == OP_ALL ? psvc_fleet_all() : psvc_fleet_abort();
}

static void enable_all(void) {              /* NULL-safe: pnl_kit_enable ignores a torn-down widget */
    int ready = s_have_status && !s_busy;   /* before the first fleet line, every button is disabled */
    for (int i = 0; i < HG_MAX_ZONES; i++) pnl_kit_enable(s_upd[i], ready && s_idle);
    pnl_kit_enable(s_all_btn, ready && s_idle);
    pnl_kit_enable(s_abort_btn, ready && !s_idle);
}

static void fleet_done(pnl_job_t *j) {
    const fleet_arg_t *a = (const fleet_arg_t *)j->arg;
    s_busy = 0;
    pnl_msg_ctx_t ctx = a->op == OP_ZONE ? PNL_CTX_FLEET_ZONE : a->op == OP_ALL ? PNL_CTX_FLEET_ALL : PNL_CTX_FLEET_ABORT;
    pnl_msg_arg_t ma = { .zone = a->zone, .version = NULL, .slot = NULL, .len = 0 };
    pnl_msg(ctx, j->rc, &ma, s_last, sizeof s_last);
    s_last_err = j->rc != PSVC_OK;
    pnl_poll_kick();                        /* the fleet line moves off IDLE (or back) at once */
    pnl_kit_msg_set(s_msg, s_last, s_last_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
    enable_all();
}

static void submit(uint8_t op, uint8_t zone) {
    if (s_busy) return;
    fleet_arg_t a = { .op = op, .zone = zone };
    if (pnl_worker_submit(fleet_run, fleet_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_busy = 1;
    s_last[0] = '\0';
    enable_all();
    pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO);
}

/* The idle wipe (Task 27): the kept fleet outcome, and a FAILED reboot overlay (a dialog waiting for Close). A
 * "Rebooting..." overlay stays: the master is going down (the one intended exception). */
void sys_fleet_wipe(void) {
    if (!s_busy) { s_last[0] = '\0'; s_last_err = 0; pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO); }   /* NULL-safe */
    if (s_overlay && s_overlay_failed) {    /* called from a timer, never from inside the overlay: delete now */
        lv_obj_delete(s_overlay);
        s_overlay = s_overlay_lbl = NULL;
        s_overlay_failed = 0;
    }
}

static void upd_click(lv_event_t *e) { submit(OP_ZONE, (uint8_t)(intptr_t)lv_event_get_user_data(e)); }
static void all_click(lv_event_t *e) { (void)e; submit(OP_ALL, 0); }
static void abort_click(lv_event_t *e) { (void)e; submit(OP_ABORT, 0); }
static void reboot_click(lv_event_t *e) { (void)e; sys_reboot_confirm(NULL); }

static void rebuild_rows(uint8_t mask) {    /* one row per enrolled zone (app.js:996-1001), rebuilt when the set moves */
    lv_obj_clean(s_rows);
    for (int i = 0; i < HG_MAX_ZONES; i++) s_upd[i] = s_name[i] = NULL;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (!(mask & (1u << i))) continue;
        lv_obj_t *r = pnl_kit_row(s_rows);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        s_name[i] = lv_label_create(r);
        lv_label_set_text(s_name[i], "");
        lv_obj_set_width(s_name[i], 220);
        s_upd[i] = pnl_kit_button(r, "Update", upd_click, (void *)(intptr_t)(i + 1));   /* slot = id-1 */
    }
    s_rows_mask = (int)mask;
}

static void fleet_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Fleet");
    s_status = pnl_kit_msg(c);
    pnl_kit_msg_set(s_status, "Status: --", PNL_KIT_INFO);     /* the web's "—" before the first poll */
    s_rows = lv_obj_create(c);
    lv_obj_set_width(s_rows, LV_PCT(100));
    lv_obj_set_height(s_rows, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_rows, 0, 0);
    lv_obj_set_style_pad_all(s_rows, 0, 0);
    lv_obj_t *r = pnl_kit_row(c);
    s_all_btn = pnl_kit_button(r, "Update all", all_click, NULL);
    s_abort_btn = pnl_kit_button(r, "Abort", abort_click, NULL);
    s_msg = pnl_kit_msg(c);
    /* a request still running, or its kept outcome (it may have landed while the operator was elsewhere) */
    if (s_busy) pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO);
    else if (s_last[0]) pnl_kit_msg_set(s_msg, s_last, s_last_err ? PNL_KIT_ERR : PNL_KIT_OK);
    s_rows_mask = -1;                       /* force the first update to build the rows */
    s_trial_shown = -1;
    s_have_status = 0;                      /* Update, Update all AND Abort disabled until a poll delivers a fleet line: */
    s_idle = 0;                             /* the web never offers Abort before its first poll (app.js:995, 1007) */
    enable_all();

    lv_obj_t *rb = pnl_kit_card(parent, "Reboot");
    s_fw_lbl = pnl_kit_msg(rb);
    s_trial_lbl = pnl_kit_msg(rb);
    pnl_kit_button(rb, "Reboot master", reboot_click, NULL);
}

static void fleet_update(const pnl_snap_t *s) {
    if (!s_status) return;
    char b[64];
    snprintf(b, sizeof b, "Status: %s", s->st.fleet_line[0] ? s->st.fleet_line : "--");
    pnl_label_set_if_changed(s_status, b);
    uint8_t mask = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (s->st.node[i].used) mask |= (uint8_t)(1u << i);
    if ((int)mask != s_rows_mask) rebuild_rows(mask);
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        if (!s_name[i]) continue;
        char name[17];
        pnl_zone_name(&s->st.node[i], name);   /* a rename shows without a rebuild */
        pnl_label_set_if_changed(s_name[i], name);
    }
    s_have_status = s->st.fleet_line[0] != '\0';
    s_idle = (uint8_t)psvc_fleet_idle(s->st.fleet_line);
    enable_all();
    snprintf(b, sizeof b, "Master firmware: %s %s", s->st.fw_slot, s->st.fw_state);
    pnl_label_set_if_changed(s_fw_lbl, b);
    int trial = strcmp(s->st.fw_state, "PENDING") == 0;
    if (trial != s_trial_shown) {
        pnl_kit_msg_set(s_trial_lbl, trial ? "OTA trial in progress -- rebooting now retires this image" : "",
                        trial ? PNL_KIT_ERR : PNL_KIT_INFO);
        s_trial_shown = trial;
    }
}

static void fleet_teardown(void) {
    s_status = s_rows = s_all_btn = s_abort_btn = s_msg = s_fw_lbl = s_trial_lbl = NULL;
    for (int i = 0; i < HG_MAX_ZONES; i++) s_upd[i] = s_name[i] = NULL;
}

const pnl_sys_section_t PNL_SYS_FLEET = { .title = "Fleet", .build = fleet_build, .update = fleet_update,
                                          .teardown = fleet_teardown };
