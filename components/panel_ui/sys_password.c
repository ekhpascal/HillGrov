/* Glue: System -> Password, the web password reset WITHOUT the old password (spec Decision 3 and "Consequences of no
 * panel auth": standing at the panel is the gate, so the panel is the deliberate recovery path). The web's own form
 * verifies the old password first (components/http_srv/http_login.c:132-176); the worker here calls
 * psvc_web_password_set() (Task 4), which hashes into a private copy, commits it and drops every web session inside
 * the master-config lock. docs/what_we_learned.md: a password change must say loudly that every phone is logged out.
 * Secrets: the draft lives in s_pw only until submit, teardown or the idle wipe (sys_password_wipe, Task 27), and is
 * wiped with pnl_zero() (volatile stores), never memset. The job arg copy is wiped by the pool after done().
 * Stale completions: the sys_wifi.c note -- the outcome is kept in module state, the widget pointers (cleared by
 * pw_teardown) are the liveness test, not screen_gen. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lvgl.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"      /* PSVC_LOCK_PANEL_MS */
#include "pcfg_gen.h"       /* PCFG_KB_TEXT */
#include "pnl_input.h"      /* pnl_text_ok, pnl_zero */
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define PW_MIN 8    /* WA_PW_MIN / WA_PW_MAX, components/web_auth/web_auth.h:22-23 */
#define PW_MAX 63

static char     s_pw[PW_MAX + 1];
static uint8_t  s_busy, s_confirming, s_last_err;
static char     s_last[128];                /* the last outcome, kept for a rebuilt section (no secret in it) */
static lv_obj_t *s_pw_lbl, *s_set_btn, *s_msg, *s_ok_mb;

static int pw_ready(void) { return pnl_text_ok(PCFG_KB_TEXT, s_pw, PW_MIN, PW_MAX); }

static void refresh(void) {
    if (s_pw_lbl) lv_label_set_text(s_pw_lbl, s_pw[0] ? "********" : "(not entered)");
    pnl_kit_enable(s_set_btn, !s_busy && !s_confirming && pw_ready());   /* NULL-safe */
}

static void kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (accepted) {
        pnl_zero(s_pw, sizeof s_pw);            /* whole buffer first: a shorter text must not leave an old tail (C3) */
        snprintf(s_pw, sizeof s_pw, "%s", text ? text : "");
    }
    refresh();
}

static void pw_click(lv_event_t *e) {
    (void)e;
    /* masked: the keyboard's eye reveals and re-masks by itself after 10 s (Task 19); it always starts empty */
    wdg_keyboard_open("New web password (8 to 63 characters)", PCFG_KB_TEXT, "", PW_MIN, PW_MAX, 1, kb_done, NULL);
}

static void pw_run(pnl_job_t *j) { j->rc = psvc_web_password_set((const char *)j->arg, PSVC_LOCK_PANEL_MS); }

static void ok_close_click(lv_event_t *e) { lv_msgbox_close_async((lv_obj_t *)lv_event_get_user_data(e)); }

static void ok_deleted(lv_event_t *e) { if (lv_event_get_target(e) == s_ok_mb) s_ok_mb = NULL; }

/* Large and unmistakable (docs/what_we_learned.md): every phone just lost its session. Not a confirm (nothing to
 * decide), so not pnl_confirm; it outlives the screen on the top layer until OK. A second success replaces it. */
static void show_logged_out_box(const char *text) {
    if (s_ok_mb) lv_msgbox_close(s_ok_mb);      /* ok_deleted clears s_ok_mb */
    lv_obj_t *mb = lv_msgbox_create(NULL);
    if (!mb) return;                            /* no LVGL memory: the section's own message still says it */
    lv_msgbox_add_title(mb, "Web password changed");
    lv_obj_t *t = lv_msgbox_add_text(mb, text);
    if (t) lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_t *b = lv_msgbox_add_footer_button(mb, "OK");
    if (b) lv_obj_add_event_cb(b, ok_close_click, LV_EVENT_CLICKED, mb);
    lv_obj_add_event_cb(mb, ok_deleted, LV_EVENT_DELETE, NULL);
    s_ok_mb = mb;
}

static void pw_done(pnl_job_t *j) {
    s_busy = 0;
    pnl_msg(PNL_CTX_PASSWORD, j->rc, NULL, s_last, sizeof s_last);
    s_last_err = j->rc != PSVC_OK;
    if (!s_last_err) show_logged_out_box(s_last);
    pnl_kit_msg_set(s_msg, s_last, s_last_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
    refresh();
}

static void pw_go(void *ctx) {                  /* [Change password] */
    (void)ctx;
    s_confirming = 0;
    if (s_busy || !pw_ready()) { refresh(); return; }   /* wiped while the box was up */
    char pw[PW_MAX + 1];
    memcpy(pw, s_pw, sizeof pw);                /* s_pw is always NUL-terminated inside its size */
    if (pnl_worker_submit(pw_run, pw_done, pw, sizeof pw) != 0) {
        pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR);   /* the draft stays for a retry */
    } else {
        s_busy = 1;
        s_last[0] = '\0';
        pnl_zero(s_pw, sizeof s_pw);            /* wiped right after submit */
        pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO);
    }
    pnl_zero(pw, sizeof pw);                    /* a dying local: a plain memset is a dead store at -Os */
    refresh();
}

static void pw_cancel(void *ctx) {              /* [Cancel], or the box dismissed by a teardown / the idle wipe */
    (void)ctx;
    s_confirming = 0;
    refresh();                                  /* NULL-safe */
}

static void set_click(lv_event_t *e) {
    (void)e;
    if (s_busy || s_confirming || !pw_ready()) return;
    s_confirming = 1;                           /* before the call: on_cancel may run inside it */
    pnl_confirm("Change the web password?", "This logs out every phone and browser using the web UI",
                "Change password", pw_go, pw_cancel, NULL);
    refresh();
}

static void pw_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Web password");
    lv_obj_t *note = pnl_kit_msg(c);
    pnl_kit_msg_set(note, "No old password is needed here: standing at the panel is the gate.", PNL_KIT_INFO);
    s_pw_lbl = pnl_kit_field(c, "New password", pw_click, NULL);
    s_set_btn = pnl_kit_button(c, "Set password", set_click, NULL);
    s_msg = pnl_kit_msg(c);
    lv_obj_set_style_text_font(s_msg, &lv_font_montserrat_28, 0);
    /* a change still running, or its kept outcome (it may have landed while the operator was elsewhere) */
    if (s_busy) pnl_kit_msg_set(s_msg, "...", PNL_KIT_INFO);
    else if (s_last[0]) pnl_kit_msg_set(s_msg, s_last, s_last_err ? PNL_KIT_ERR : PNL_KIT_OK);
    refresh();
}

static void pw_teardown(void) {
    if (s_confirming) pnl_confirm_close();      /* runs pw_cancel while the widgets still exist */
    if (s_pw_lbl && wdg_keyboard_is_open()) wdg_keyboard_close();   /* wipes a half-typed password; modal, so ours */
    pnl_zero(s_pw, sizeof s_pw);                /* an unsubmitted password never outlives its screen */
    s_pw_lbl = s_set_btn = s_msg = NULL;
}

void sys_password_wipe(void) {
    if (s_confirming) pnl_confirm_close();
    if (s_pw_lbl && wdg_keyboard_is_open()) wdg_keyboard_close();   /* the section is showing: the keyboard is ours */
    pnl_zero(s_pw, sizeof s_pw);
    if (!s_busy) { s_last[0] = '\0'; pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO); }
    refresh();
}

const pnl_sys_section_t PNL_SYS_PASSWORD = { .title = "Password", .build = pw_build, .update = NULL,
                                             .teardown = pw_teardown };
