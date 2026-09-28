/* Glue: System -> Wi-Fi, the web's Wi-Fi card (web/app.js:913-944) and its handlers wifiScan / wifiPick / wifiJoin /
 * apSet (app.js:1787-1830). SSIDs and passwords never travel as CLI lines: the worker calls psvc_wifi_* (Task 4).
 * THE RULE: the scan, the join and the AP change run only on the worker; this file reads copies (the snapshot, the
 * scan list once scan_done() hands it back).
 *
 * Stale completions: every done() stores its outcome in module state first, then draws through NULL-safe setters.
 * wifi_teardown() clears the widget pointers before the widgets go, so those pointers are the liveness test (the
 * Task 22 pattern) rather than screen_gen: a scan, join or AP change that ends after the operator left System and
 * came back still reaches the rebuilt widgets instead of leaving "Scanning..." (and a disabled Scan) on screen. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "psvc_net.h"
#include "psvc_mcfg.h"      /* PSVC_LOCK_PANEL_MS */
#include "pcfg_gen.h"       /* PCFG_KB_TEXT */
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_fmt.h"
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "wdg_keyboard.h"
#include "scr_system.h"

#define SCAN_CAP 20

typedef struct { char *buf; size_t cap; uint8_t min_len, max_len, secret; const char *title; lv_obj_t **lbl; } wfield_t;
typedef struct { wifi_scan_t *out; int cap; } scan_arg_t;
typedef struct { char ssid[33]; char pass[65]; } wifi_arg_t;   /* 98 B by value; the pool wipes arg after done() */

static wifi_scan_t *s_scan;                 /* PSRAM; the worker owns it from submit until scan_done() */
static int      s_scan_n = -1;              /* -1 = no list to show */
static char     s_scan_err[64];
static uint8_t  s_scanning, s_joining, s_apping, s_confirming;
static char     s_join_last[96], s_ap_last[96]; /* the last outcomes, kept for a rebuilt section */
static uint8_t  s_join_err, s_ap_err;
static char     s_sta_ssid[33], s_sta_pass[65], s_ap_ssid[33], s_ap_pass[65];
static lv_obj_t *s_sta_lbl, *s_ap_lbl, *s_scan_btn, *s_list, *s_join_msg, *s_ap_msg;
static lv_obj_t *s_f_sta_ssid, *s_f_sta_pass, *s_f_ap_ssid, *s_f_ap_pass;

static const wfield_t F_STA_SSID = { s_sta_ssid, sizeof s_sta_ssid, 0, 32, 0, "House Wi-Fi SSID", &s_f_sta_ssid };
static const wfield_t F_STA_PASS = { s_sta_pass, sizeof s_sta_pass, 0, 63, 1, "House Wi-Fi password", &s_f_sta_pass };
static const wfield_t F_AP_SSID  = { s_ap_ssid,  sizeof s_ap_ssid,  1, 32, 0, "AP SSID", &s_f_ap_ssid };
static const wfield_t F_AP_PASS  = { s_ap_pass,  sizeof s_ap_pass,  8, 63, 1, "AP password (8+ chars)", &s_f_ap_pass };

/* An SSID is raw bytes (UTF-8, say): shown, any byte outside 0x20..0x7E becomes '?' (the built-in Montserrat has no
 * glyph for it, Global Constraints "UI text"); joined, the raw bytes go to the worker untouched. */
static void ascii_into(char *dst, size_t cap, const char *src) {
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) dst[i] = (src[i] >= 0x20 && src[i] <= 0x7E) ? src[i] : '?';
    dst[i] = '\0';
}

/* The web's .trim() on an SSID (app.js:1807, 1820): leading and trailing spaces off; the password is never trimmed. */
static void copy_trimmed(char *dst, size_t cap, const char *src) {
    while (*src == ' ') src++;
    size_t n = strlen(src);
    while (n && src[n - 1] == ' ') n--;
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void show_field(const wfield_t *f) {
    if (!*f->lbl) return;
    if (f->secret) { lv_label_set_text(*f->lbl, f->buf[0] ? "********" : "(none)"); return; }
    char t[33];
    ascii_into(t, sizeof t, f->buf);
    lv_label_set_text(*f->lbl, t[0] ? t : "(none)");
}

static void show_all(void) {
    show_field(&F_STA_SSID); show_field(&F_STA_PASS); show_field(&F_AP_SSID); show_field(&F_AP_PASS);
}

static void kb_done(void *ctx, int accepted, const char *text) {
    const wfield_t *f = (const wfield_t *)ctx;
    if (accepted) {
        pnl_zero(f->buf, f->cap);          /* whole buffer first: a shorter text must not leave an old tail (C3) */
        snprintf(f->buf, f->cap, "%s", text ? text : "");
    }
    show_field(f);
}

static void field_click(lv_event_t *e) {
    const wfield_t *f = (const wfield_t *)lv_event_get_user_data(e);
    /* the keyboard's masked mode has the reveal (eye) toggle that re-masks after 10 s (Task 19); a secret's keyboard
     * starts from the operator's own draft only -- the stored password never reaches this task */
    wdg_keyboard_open(f->title, PCFG_KB_TEXT, f->buf, f->min_len, f->max_len, f->secret, kb_done, (void *)f);
}

/* ---- scan ---- */
static void pick_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < s_scan_n) {                                   /* app.js wifiPick */
        memset(s_sta_ssid, 0, sizeof s_sta_ssid);
        snprintf(s_sta_ssid, sizeof s_sta_ssid, "%s", s_scan[i].ssid);
    }
    show_field(&F_STA_SSID);
}

static void render_scan(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    pnl_kit_enable(s_scan_btn, !s_scanning);
    lv_obj_t *m;
    if (s_scanning) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "Scanning... (the AP pauses briefly)", PNL_KIT_INFO); return; }
    if (s_scan_err[0]) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, s_scan_err, PNL_KIT_ERR); return; }
    if (s_scan_n < 0) return;
    if (s_scan_n == 0) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "No networks found.", PNL_KIT_INFO); return; }
    for (int i = 0; i < s_scan_n; i++) {
        char ssid[33], t[80];
        ascii_into(ssid, sizeof ssid, s_scan[i].ssid);
        /* auth is a raw wifi_auth_mode_t: 0 == WIFI_AUTH_OPEN, anything else is secured (app.js:928-930) */
        snprintf(t, sizeof t, "%s  |  %s  |  %d dBm", ssid, s_scan[i].auth == 0 ? "open" : "secured", (int)s_scan[i].rssi);
        lv_obj_t *b = pnl_kit_button(s_list, t, pick_click, (void *)(intptr_t)i);
        lv_obj_set_width(b, LV_PCT(100));
    }
}

static void scan_run(pnl_job_t *j) {
    const scan_arg_t *a = (const scan_arg_t *)j->arg;
    int n = 0;
    j->rc = psvc_wifi_scan(a->out, a->cap, &n, PSVC_LOCK_PANEL_MS);
    j->irc = n;
}

static void scan_done(pnl_job_t *j) {
    s_scanning = 0;
    if (j->rc == PSVC_OK) { s_scan_n = j->irc; s_scan_err[0] = '\0'; }
    else { s_scan_n = -1; pnl_msg(PNL_CTX_SCAN, j->rc, NULL, s_scan_err, sizeof s_scan_err); }
    render_scan();   /* NULL-safe: s_list is the liveness test */
}

static void scan_click(lv_event_t *e) {
    (void)e;
    if (s_scanning) return;
    if (!s_scan) s_scan = heap_caps_calloc(SCAN_CAP, sizeof *s_scan, MALLOC_CAP_SPIRAM);
    if (!s_scan) { snprintf(s_scan_err, sizeof s_scan_err, "Scan unavailable (no memory)"); render_scan(); return; }
    scan_arg_t a = { .out = s_scan, .cap = SCAN_CAP };
    if (pnl_worker_submit(scan_run, scan_done, &a, sizeof a) != 0) {
        snprintf(s_scan_err, sizeof s_scan_err, "Panel busy -- try again");
    } else {
        s_scanning = 1;
        s_scan_err[0] = '\0';
    }
    render_scan();
}

/* ---- join the house Wi-Fi (STA) ---- */
static void join_run(pnl_job_t *j) {
    const wifi_arg_t *a = (const wifi_arg_t *)j->arg;
    j->rc = psvc_wifi_set_sta(a->ssid, a->pass, PSVC_LOCK_PANEL_MS);
}

static void join_done(pnl_job_t *j) {
    s_joining = 0;
    pnl_msg(PNL_CTX_WIFI_JOIN, j->rc, NULL, s_join_last, sizeof s_join_last);   /* OK: "Saved -- joining..." */
    s_join_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_join_msg, s_join_last, s_join_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
}

static void join_click(lv_event_t *e) {
    (void)e;
    if (s_joining) return;
    wifi_arg_t a;
    memset(&a, 0, sizeof a);
    copy_trimmed(a.ssid, sizeof a.ssid, s_sta_ssid);
    if (!a.ssid[0]) { pnl_kit_msg_set(s_join_msg, "Enter an SSID", PNL_KIT_ERR); return; }   /* app.js:1809 */
    /* the web's join: the password is sent as typed, "" = an open network (app.js:1811, psvc_net.h) -- there is no
     * stored password to keep here, the pair is new */
    snprintf(a.pass, sizeof a.pass, "%s", s_sta_pass);
    if (pnl_worker_submit(join_run, join_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_join_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_joining = 1;
        s_join_last[0] = '\0';
        pnl_zero(s_sta_pass, sizeof s_sta_pass);     /* secrets are wiped right after use */
        show_field(&F_STA_PASS);
        pnl_kit_msg_set(s_join_msg, "...", PNL_KIT_INFO);
    }
    pnl_zero(&a, sizeof a);                          /* a dying local: a plain memset is a dead store at -Os */
}

/* ---- set the AP ---- */
static void ap_run(pnl_job_t *j) {
    const wifi_arg_t *a = (const wifi_arg_t *)j->arg;
    j->rc = psvc_wifi_set_ap(a->ssid, a->pass, PSVC_LOCK_PANEL_MS);
}

static void ap_done(pnl_job_t *j) {
    s_apping = 0;
    pnl_msg(PNL_CTX_WIFI_AP, j->rc, NULL, s_ap_last, sizeof s_ap_last);
    s_ap_err = j->rc != PSVC_OK;
    pnl_kit_msg_set(s_ap_msg, s_ap_last, s_ap_err ? PNL_KIT_ERR : PNL_KIT_OK);   /* NULL-safe */
}

static void ap_go(void *ctx) {                       /* [Change AP] */
    (void)ctx;
    s_confirming = 0;
    wifi_arg_t a;
    memset(&a, 0, sizeof a);
    copy_trimmed(a.ssid, sizeof a.ssid, s_ap_ssid);
    snprintf(a.pass, sizeof a.pass, "%s", s_ap_pass);
    if (!a.ssid[0] || strlen(a.pass) < 8) {          /* the fields were wiped while the box was up */
        pnl_kit_msg_set(s_ap_msg, "AP SSID required, password 8+ chars", PNL_KIT_ERR);
    } else if (pnl_worker_submit(ap_run, ap_done, &a, sizeof a) != 0) {
        pnl_kit_msg_set(s_ap_msg, "Panel busy -- try again", PNL_KIT_ERR);
    } else {
        s_apping = 1;
        s_ap_last[0] = '\0';
        pnl_zero(s_ap_pass, sizeof s_ap_pass);
        show_field(&F_AP_PASS);
        pnl_kit_msg_set(s_ap_msg, "...", PNL_KIT_INFO);
    }
    pnl_zero(&a, sizeof a);
}

static void ap_cancel(void *ctx) {                   /* [Cancel], or the box dismissed by a teardown / the idle wipe */
    (void)ctx;
    s_confirming = 0;
    pnl_kit_msg_set(s_ap_msg, "Not saved.", PNL_KIT_INFO);   /* NULL-safe */
}

static void ap_click(lv_event_t *e) {
    (void)e;
    if (s_apping || s_confirming) return;
    char ssid[33];
    copy_trimmed(ssid, sizeof ssid, s_ap_ssid);
    if (!ssid[0] || strlen(s_ap_pass) < 8) {                            /* app.js:1821 */
        pnl_kit_msg_set(s_ap_msg, "AP SSID required, password 8+ chars", PNL_KIT_ERR);
        return;
    }
    s_confirming = 1;                                /* before the call: on_cancel may run inside it */
    pnl_confirm("Change the AP?", "Changing the AP drops every phone connected to it", "Change AP", ap_go, ap_cancel,
                NULL);
}

/* ---- section ---- */
static void wifi_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Wi-Fi");
    s_sta_lbl = pnl_kit_msg(c);
    s_ap_lbl = pnl_kit_msg(c);
    s_scan_btn = pnl_kit_button(c, "Scan", scan_click, NULL);
    s_list = lv_obj_create(c);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);

    lv_obj_t *sta = pnl_kit_card(parent, "Join house Wi-Fi");
    s_f_sta_ssid = pnl_kit_field(sta, "SSID", field_click, (void *)&F_STA_SSID);
    s_f_sta_pass = pnl_kit_field(sta, "Password", field_click, (void *)&F_STA_PASS);
    pnl_kit_button(sta, "Join", join_click, NULL);
    s_join_msg = pnl_kit_msg(sta);

    lv_obj_t *ap = pnl_kit_card(parent, "Master AP");
    s_f_ap_ssid = pnl_kit_field(ap, "AP SSID", field_click, (void *)&F_AP_SSID);
    s_f_ap_pass = pnl_kit_field(ap, "AP password", field_click, (void *)&F_AP_PASS);
    pnl_kit_button(ap, "Set AP", ap_click, NULL);
    s_ap_msg = pnl_kit_msg(ap);
    /* a join or AP change still running, or its kept outcome (it may have landed while the operator was elsewhere) */
    if (s_joining) pnl_kit_msg_set(s_join_msg, "...", PNL_KIT_INFO);
    else if (s_join_last[0]) pnl_kit_msg_set(s_join_msg, s_join_last, s_join_err ? PNL_KIT_ERR : PNL_KIT_OK);
    if (s_apping) pnl_kit_msg_set(s_ap_msg, "...", PNL_KIT_INFO);
    else if (s_ap_last[0]) pnl_kit_msg_set(s_ap_msg, s_ap_last, s_ap_err ? PNL_KIT_ERR : PNL_KIT_OK);
    show_all();
    render_scan();
}

static void wifi_update(const pnl_snap_t *s) {
    char b[96], line[112];
    pnl_fmt_sta(&s->st.wifi, b, sizeof b);
    snprintf(line, sizeof line, "STA: %s", b);
    pnl_label_set_if_changed(s_sta_lbl, line);       /* NULL-safe */
    pnl_fmt_ap(&s->st.wifi, b, sizeof b);
    snprintf(line, sizeof line, "AP: %s", b);
    pnl_label_set_if_changed(s_ap_lbl, line);
}

static void wifi_teardown(void) {
    if (s_confirming) pnl_confirm_close();           /* runs ap_cancel while the widgets still exist */
    if (s_list && wdg_keyboard_is_open()) wdg_keyboard_close();   /* wipes a half-typed password; modal, so ours */
    s_sta_lbl = s_ap_lbl = s_scan_btn = s_list = s_join_msg = s_ap_msg = NULL;
    s_f_sta_ssid = s_f_sta_pass = s_f_ap_ssid = s_f_ap_pass = NULL;
}

void sys_wifi_wipe(void) {
    if (s_confirming) pnl_confirm_close();
    if (s_list && wdg_keyboard_is_open()) wdg_keyboard_close();   /* the section is showing: the keyboard is ours */
    pnl_zero(s_sta_ssid, sizeof s_sta_ssid);
    pnl_zero(s_sta_pass, sizeof s_sta_pass);
    pnl_zero(s_ap_ssid, sizeof s_ap_ssid);
    pnl_zero(s_ap_pass, sizeof s_ap_pass);
    s_join_last[0] = s_ap_last[0] = '\0';
    if (!s_joining) pnl_kit_msg_set(s_join_msg, "", PNL_KIT_INFO);
    if (!s_apping) pnl_kit_msg_set(s_ap_msg, "", PNL_KIT_INFO);
    if (!s_scanning) {                    /* a scan in flight owns s_scan until scan_done() */
        s_scan_n = -1;
        s_scan_err[0] = '\0';
        if (s_scan) memset(s_scan, 0, SCAN_CAP * sizeof *s_scan);
    }
    show_all();
    render_scan();
}

const pnl_sys_section_t PNL_SYS_WIFI = { .title = "Wi-Fi", .build = wifi_build, .update = wifi_update,
                                         .teardown = wifi_teardown };
