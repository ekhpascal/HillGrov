/* Glue: System -> Time, the web's Time card (web/app.js:945-958) and tzSet (app.js:1832-1843), plus the manual
 * "Set clock" (D14, owner-confirmed). TZ goes through psvc_tz_set on the worker (never a CLI line); "Set clock" is the
 * one CLI line here (SET TIME, cmd_common.c:117-135), because SET TIME is the only way to set the clock and carries no
 * secret. The master's clock and SET TIME are UTC -- nothing here calls setenv("TZ") or tzset: the operator picks LOCAL
 * wall time and pnl_set_time_line() converts it with the snapshot's time_svc_utc_offset() (the offset in effect now).
 * Stale completions: the sys_wifi.c note -- outcomes are kept in module state, the widget pointers (cleared by
 * time_teardown) are the liveness test, not screen_gen. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "hg_mcfg.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"
#include "pcfg_gen.h"       /* PCFG_KB_TEXT_NOSPACE */
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_time.h"
#include "pnl_fmt.h"
#include "pnl_msg.h"
#include "pnl_cmd.h"
#include "pnl_poll.h"       /* pnl_poll_kick */
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define YEAR0 2020   /* the master's own SET TIME range, 2020..2099 (components/app_common/app_if_common.c:138) */
#define YEARS 80

static char     s_tz[48];
static char     s_tz_last[96], s_clk_last[PNL_JOB_OUT_MAX];   /* kept outcomes for a rebuilt section (sys_wifi.c note) */
static uint8_t  s_tz_err, s_clk_err;
static uint8_t  s_tz_dirty, s_tz_busy, s_clk_busy, s_have_snap, s_prefilled;
static int32_t  s_offset;
static lv_obj_t *s_now_lbl, *s_tz_lbl, *s_tz_msg, *s_clk_msg, *s_clk_btn, *s_r_y, *s_r_mo, *s_r_d, *s_r_h, *s_r_mi;
static char     s_opt_y[YEARS * 5 + 1], s_opt_mo[12 * 3 + 1], s_opt_d[31 * 3 + 1], s_opt_h[24 * 3 + 1], s_opt_mi[60 * 3 + 1];

static void build_opts(char *out, size_t cap, int from, int n, int width) {
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + o, cap - o, "%s%0*d", i ? "\n" : "", width, from + i);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

static lv_obj_t *roller(lv_obj_t *parent, const char *opts) {
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, LV_ROLLER_MODE_NORMAL);   /* copies opts */
    lv_roller_set_visible_row_count(r, 3);
    return r;
}

/* ---- TZ ---- */
static void tz_show(void) { if (s_tz_lbl) lv_label_set_text(s_tz_lbl, s_tz[0] ? s_tz : "(none)"); }

static void tz_prefill(void) {                  /* from the master config, like app.js's cfgDoc[0].TIME.TZ */
    hg_mcfg_t m;
    psvc_mcfg_get(&m);                          /* [ANY]: a copy */
    memset(s_tz, 0, sizeof s_tz);
    snprintf(s_tz, sizeof s_tz, "%s", m.tz);
    pnl_zero(&m, sizeof m);                     /* the copy holds both Wi-Fi passwords; a memset here is a dead store */
}

static void tz_kb_done(void *ctx, int accepted, const char *text) {
    (void)ctx;
    if (!accepted) return;
    memset(s_tz, 0, sizeof s_tz);               /* whole buffer before reuse */
    snprintf(s_tz, sizeof s_tz, "%s", text ? text : "");
    s_tz_dirty = 1;
    tz_show();
}

static void tz_click(lv_event_t *e) {
    (void)e;
    wdg_keyboard_open("POSIX TZ (e.g. CET-1CEST,M3.5.0,M10.5.0/3)", PCFG_KB_TEXT_NOSPACE, s_tz, 1, 47, 0, tz_kb_done, NULL);
}

static void tz_run(pnl_job_t *j) { j->rc = psvc_tz_set((const char *)j->arg, PSVC_LOCK_PANEL_MS); }

static void tz_done(pnl_job_t *j) {
    s_tz_busy = 0;
    if (j->rc == PSVC_OK) {
        /* j->arg is the TZ that was submitted (tz_set_click's zero-padded 48 B copy; the pool wipes it only after this
         * returns). A draft edited while the Set TZ was in flight is newer than what was saved: it stays dirty, so the
         * next build keeps it instead of re-reading the config over it. */
        if (strncmp((const char *)j->arg, s_tz, sizeof s_tz) == 0) s_tz_dirty = 0;
        pnl_poll_kick();                                          /* the new offset reaches "Now" at once */
    }
    pnl_msg(PNL_CTX_TZ, j->rc, NULL, s_tz_last, sizeof s_tz_last);
    s_tz_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_tz_msg, s_tz_last, s_tz_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
}

static void tz_set_click(lv_event_t *e) {
    (void)e;
    if (s_tz_busy) return;
    if (!s_tz[0] || strchr(s_tz, ' ')) {                                /* app.js:1834 */
        pnl_kit_msg_set(s_tz_msg, "Enter a POSIX TZ with no spaces", PNL_KIT_ERR);
        return;
    }
    char tz[48];
    memset(tz, 0, sizeof tz);
    snprintf(tz, sizeof tz, "%s", s_tz);
    if (pnl_worker_submit(tz_run, tz_done, tz, sizeof tz) != 0) {
        pnl_kit_msg_set(s_tz_msg, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_tz_busy = 1;
    s_tz_last[0] = '\0';
    pnl_kit_msg_set(s_tz_msg, "...", PNL_KIT_INFO);
}

/* ---- Set clock (D14): local time in, SET TIME <UTC> out ---- */
static void clk_run(pnl_job_t *j) { j->irc = pnl_cmd_run((const char *)j->arg, (char *)j->out, PNL_JOB_OUT_MAX); }

static void clk_done(pnl_job_t *j) {
    s_clk_busy = 0;
    memcpy(s_clk_last, j->out, sizeof s_clk_last);   /* the reply; pnl_cmd_run NUL-terminates within out */
    s_clk_last[sizeof s_clk_last - 1] = '\0';
    size_t n = strlen(s_clk_last);                   /* cmd_okf ends the line with '\n': no blank line under the label */
    while (n && (s_clk_last[n - 1] == '\n' || s_clk_last[n - 1] == '\r')) s_clk_last[--n] = '\0';
    s_clk_err = j->irc != 0;
    if (!s_clk_err) pnl_poll_kick();                 /* "Now" shows the new clock at once */
    pnl_kit_msg_set(s_clk_msg, s_clk_last, s_clk_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
    pnl_kit_enable(s_clk_btn, s_have_snap);
}

static void clk_click(lv_event_t *e) {
    (void)e;
    if (s_clk_busy || !s_have_snap || !s_r_y) return;
    int y  = YEAR0 + (int)lv_roller_get_selected(s_r_y);
    int mo = 1 + (int)lv_roller_get_selected(s_r_mo);
    int d  = 1 + (int)lv_roller_get_selected(s_r_d);
    int h  = (int)lv_roller_get_selected(s_r_h);
    int mi = (int)lv_roller_get_selected(s_r_mi);
    char line[48];
    memset(line, 0, sizeof line);
    if (pnl_set_time_line(y, mo, d, h, mi, s_offset, line, sizeof line) != 0) {
        pnl_kit_msg_set(s_clk_msg, "Pick a real date between 2020 and 2099", PNL_KIT_ERR);
        return;
    }
    if (pnl_worker_submit(clk_run, clk_done, line, sizeof line) != 0) {
        pnl_kit_msg_set(s_clk_msg, "Panel busy -- try again", PNL_KIT_ERR);
        return;
    }
    s_clk_busy = 1;
    s_clk_last[0] = '\0';
    pnl_kit_enable(s_clk_btn, 0);
    pnl_kit_msg_set(s_clk_msg, "...", PNL_KIT_INFO);
}

/* ---- section ---- */
static void time_build(lv_obj_t *parent) {
    if (!s_tz_dirty) tz_prefill();              /* an unsaved draft survives a section switch */
    lv_obj_t *c = pnl_kit_card(parent, "Time");
    s_now_lbl = pnl_kit_msg(c);
    s_tz_lbl = pnl_kit_field(c, "Time zone", tz_click, NULL);
    pnl_kit_button(c, "Set TZ", tz_set_click, NULL);
    s_tz_msg = pnl_kit_msg(c);
    if (s_tz_busy) pnl_kit_msg_set(s_tz_msg, "...", PNL_KIT_INFO);
    else if (s_tz_last[0]) pnl_kit_msg_set(s_tz_msg, s_tz_last, s_tz_err ? PNL_KIT_ERR : PNL_KIT_OK);
    tz_show();

    lv_obj_t *k = pnl_kit_card(parent, "Set clock (local time)");
    lv_obj_t *note = pnl_kit_msg(k);
    pnl_kit_msg_set(note, "For when NTP is unavailable. The master keeps UTC; this converts with the current UTC offset.",
                    PNL_KIT_INFO);
    if (!s_opt_y[0]) {
        build_opts(s_opt_y, sizeof s_opt_y, YEAR0, YEARS, 4);
        build_opts(s_opt_mo, sizeof s_opt_mo, 1, 12, 2);
        build_opts(s_opt_d, sizeof s_opt_d, 1, 31, 2);
        build_opts(s_opt_h, sizeof s_opt_h, 0, 24, 2);
        build_opts(s_opt_mi, sizeof s_opt_mi, 0, 60, 2);
    }
    lv_obj_t *r = pnl_kit_row(k);
    s_r_y = roller(r, s_opt_y);
    s_r_mo = roller(r, s_opt_mo);
    s_r_d = roller(r, s_opt_d);
    lv_obj_t *sep = lv_label_create(r);
    lv_label_set_text(sep, "   ");
    s_r_h = roller(r, s_opt_h);
    s_r_mi = roller(r, s_opt_mi);
    lv_roller_set_selected(s_r_y, 2026 - YEAR0, LV_ANIM_OFF);
    lv_roller_set_selected(s_r_h, 12, LV_ANIM_OFF);
    s_clk_btn = pnl_kit_button(k, "Set clock", clk_click, NULL);
    pnl_kit_enable(s_clk_btn, 0);               /* until the first snapshot delivers the UTC offset */
    s_clk_msg = pnl_kit_msg(k);
    if (s_clk_busy) pnl_kit_msg_set(s_clk_msg, "...", PNL_KIT_INFO);
    else if (s_clk_last[0]) pnl_kit_msg_set(s_clk_msg, s_clk_last, s_clk_err ? PNL_KIT_ERR : PNL_KIT_OK);
    s_prefilled = 0;
}

static void time_update(const pnl_snap_t *s) {
    if (!s_now_lbl) return;
    char b[80];
    pnl_fmt_master_time(s->st.time, s->st.time_src, s->st.utc_offset_s, s->st.time_is_set, b, sizeof b);
    pnl_label_set_if_changed(s_now_lbl, b);
    s_offset = s->st.utc_offset_s;
    s_have_snap = 1;
    if (!s_clk_busy) pnl_kit_enable(s_clk_btn, 1);
    if (!s_prefilled && s_r_y) {
        pnl_local_t t;
        pnl_local_time((int64_t)time(NULL), s->st.utc_offset_s, s->st.time_is_set, &t);
        if (t.valid && t.year >= YEAR0 && t.year < YEAR0 + YEARS) {
            lv_roller_set_selected(s_r_y, (uint32_t)(t.year - YEAR0), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_mo, (uint32_t)(t.mon - 1), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_d, (uint32_t)(t.mday - 1), LV_ANIM_OFF);
            lv_roller_set_selected(s_r_h, (uint32_t)t.hour, LV_ANIM_OFF);
            lv_roller_set_selected(s_r_mi, (uint32_t)t.min, LV_ANIM_OFF);
        }
        s_prefilled = 1;
    }
}

static void time_teardown(void) {
    if (s_tz_lbl && wdg_keyboard_is_open()) wdg_keyboard_close();   /* modal, so it is the TZ keyboard */
    s_now_lbl = s_tz_lbl = s_tz_msg = s_clk_msg = s_clk_btn = NULL;
    s_r_y = s_r_mo = s_r_d = s_r_h = s_r_mi = NULL;
}

void sys_time_wipe(void) {
    if (s_tz_lbl && wdg_keyboard_is_open()) wdg_keyboard_close();
    s_tz_dirty = 0;
    if (s_tz_lbl) tz_prefill();                 /* showing: back to the saved TZ; otherwise the next build re-reads it */
    else memset(s_tz, 0, sizeof s_tz);
    tz_show();
    if (!s_tz_busy) { s_tz_last[0] = '\0'; pnl_kit_msg_set(s_tz_msg, "", PNL_KIT_INFO); }
    if (!s_clk_busy) { s_clk_last[0] = '\0'; pnl_kit_msg_set(s_clk_msg, "", PNL_KIT_INFO); }
}

const pnl_sys_section_t PNL_SYS_TIME = { .title = "Time", .build = time_build, .update = time_update,
                                         .teardown = time_teardown };
