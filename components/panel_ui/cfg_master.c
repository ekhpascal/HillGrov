/* cfg_master.c -- the generated editor for the master (zone 0): WIFI, TIME, SYS (glue; LVGL task only).
 * No panel auth (Decision 3), but secrets are masked with a deliberate reveal: the display copy never holds
 * them, a blank secret means "unchanged", and every buffer that held one is zeroed after use (pnl_zero).
 * Secrets are never logged: nothing here logs a field value, only the no-PSRAM case.
 * Save: the dirty set only, pre-validated on a fresh scratch copy, then psvc_mcfg_edit on the worker, which applies
 * it to a FRESH copy under the master-config lock (the web PUT's semantics and refusals). The save state is the
 * shared cfg_save_t (cfg_frame.c, C14); the AP warning is the shared pnl_confirm (pnl_theme.c, C16). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "scr_config.h"
#include "pnl_worker.h"
#include "pnl_msg.h"
#include "pnl_theme.h"      /* pnl_confirm */
#include "psvc_mcfg.h"
#include "psvc_rc.h"
#include "hg_mcfg.h"
#include "time_core.h"
#include "pnl_input.h"      /* pnl_zero */
#include "wdg_keyboard.h"

static const char *TAG = "cfg_master";

typedef struct { const psvc_fedit_t *e; int n; } msave_arg_t;

static hg_mcfg_t    *s_m;          /* PSRAM display copy -- secrets zeroed the moment it is taken */
static hg_mcfg_t    *s_scratch;    /* PSRAM pre-validation / reveal copy -- zeroed straight after each use */
static pcfg_edits_t *s_medits;     /* PSRAM; survives destination changes (cfg_master_wipe clears it) */
static psvc_fedit_t *s_mfedits;    /* PSRAM frozen save set (may hold passwords) -- worker-owned while s_save.saving */
static cfg_view_t    s_mview;
static cfg_save_t    s_save;       /* the shared save state (cfg_frame.c, C14) */
static int           s_pending_n;  /* entries in s_mfedits while the AP confirm is open */
static uint8_t       s_open, s_confirming, s_rerender_due;

static int buffers_ok(void) {
    if (!s_m)        s_m        = heap_caps_calloc(1, sizeof *s_m, MALLOC_CAP_SPIRAM);
    if (!s_scratch)  s_scratch  = heap_caps_calloc(1, sizeof *s_scratch, MALLOC_CAP_SPIRAM);
    if (!s_mfedits)  s_mfedits  = heap_caps_calloc(PCFG_EDIT_MAX, sizeof *s_mfedits, MALLOC_CAP_SPIRAM);
    if (!s_medits) {
        s_medits = heap_caps_calloc(1, sizeof *s_medits, MALLOC_CAP_SPIRAM);
        if (s_medits) pcfg_edits_reset(s_medits, PCFG_TABLE_MASTER, 0);
    }
    return s_m && s_scratch && s_mfedits && s_medits;
}
static void take_copy(void) {                        /* [ANY] psvc_mcfg_get: a copy, no blocking call */
    psvc_mcfg_get(s_m);
    pnl_zero(s_m->sta_pass, sizeof s_m->sta_pass);
    pnl_zero(s_m->ap_pass, sizeof s_m->ap_pass);
    pnl_zero(s_m->web_salt, sizeof s_m->web_salt);
    pnl_zero(s_m->web_hash, sizeof s_m->web_hash);
}
static void wipe_frozen(void) {                      /* never while s_save.saving: the worker owns the set then */
    if (s_mfedits) pnl_zero(s_mfedits, PCFG_EDIT_MAX * sizeof *s_mfedits);
}

static const char *master_value(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap) {
    (void)ctx; (void)group; (void)idx;
    if (hg_mcfg_is_secret(f) || hg_field_read(f, s_m, buf, cap) != 0) snprintf(buf, cap, "%s", "");
    return buf;
}

/* wdg_reveal_fn: job-free. A pending (unsaved) new secret if there is one, otherwise the stored value through a
 * PSRAM scratch copy that is zeroed straight after. out is wdg_field's 65-byte local; it wipes it and re-masks after
 * 10 s or when the row is deleted (teardown, re-render). */
static int master_reveal(void *ctx, const hg_field_t *f, char *out, size_t cap) {
    (void)ctx;
    if (!f || !hg_mcfg_is_secret(f) || !s_scratch) return -1;
    const psvc_fedit_t *e = s_medits ? pcfg_edits_get(s_medits, f->group, -1, f) : NULL;
    if (e && e->text[0]) { snprintf(out, cap, "%s", e->text); return 0; }   /* the new, unsaved value */
    psvc_mcfg_get(s_scratch);
    int rc = hg_field_read(f, s_scratch, out, cap);
    pnl_zero(s_scratch, sizeof *s_scratch);
    return rc == 0 ? 0 : -1;
}

/* ---------- save ---------- */
static void msave_run(pnl_job_t *j) {               /* pnl_work: the master-config lock, NVS, the radio apply */
    msave_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    psvc_fedits_t fe = { a.e, a.n };
    j->rc = psvc_mcfg_edit(psvc_mcfg_fields_fn, &fe, PSVC_LOCK_PANEL_MS, "PANEL MCFG", j->err, sizeof j->err);
}
static void msave_done(pnl_job_t *j) {              /* LVGL task */
    msave_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    j->err[sizeof j->err - 1] = '\0';                /* a field path ("GROUP.KEY"), never a value */
    int ok = j->rc == PSVC_OK;
    int live = j->screen_gen == pnl_screen_gen() && s_open;   /* this editor's widgets are the ones on screen */
    char msg[CFG_SAVE_MSG_MAX];
    pnl_msg(PNL_CTX_MCFG_SAVE, j->rc, NULL, msg, sizeof msg);   /* "Saved." / master config busy / the token */
    if (ok) {
        pcfg_edits_drop_saved(s_medits, a.e, a.n);   /* re-edited entries stay dirty */
        take_copy();                                 /* the web refetches immediately (app.js:1589-1591) */
    }
    wipe_frozen();                                   /* the set is ours again, and may hold passwords */
    if (!live) {                                     /* THE RULE: stale -- touch no widget; the outcome waits */
        char k[CFG_SAVE_MSG_MAX + 112];              /* never truncated here; cfg_save_end keeps CFG_SAVE_MSG_MAX */
        snprintf(k, sizeof k, "Master: %s%s%s", msg, (!ok && j->err[0]) ? ": " : "", ok ? "" : j->err);
        cfg_save_end(&s_save, 0, k, !ok);
        if (ok && s_open) s_rerender_due = 1;        /* the rebuilt editor shows the refreshed copy at the next update */
        return;
    }
    cfg_save_end(&s_save, 1, msg, !ok);
    if (ok) {
        if (wdg_keyboard_is_open()) s_rerender_due = 1; else cfg_frame_rerender();
    } else if (j->err[0]) {
        cfg_show_error(&s_mview, j->err, psvc_rc_token(j->rc));   /* the field path, highlighted */
    }
}
static void submit_save(void) {
    msave_arg_t a = { .e = s_mfedits, .n = s_pending_n };
    s_pending_n = 0;
    if (pnl_worker_submit(msave_run, msave_done, &a, sizeof a) != 0) {
        wipe_frozen();
        cfg_set_status("Panel busy, retry", 1);
        return;
    }
    cfg_save_begin(&s_save);
}
static int touches_ap(const psvc_fedit_t *e, int n) {
    for (int i = 0; i < n; i++)
        if (e[i].f && e[i].group == HG_MG_WIFI && (strcmp(e[i].f->key, "AP_SSID") == 0 || strcmp(e[i].f->key, "AP_PASS") == 0))
            return 1;
    return 0;
}
static void ap_go(void *ctx) {                      /* [Save anyway] */
    (void)ctx;
    s_confirming = 0;
    if (s_open && !s_save.saving) { submit_save(); return; }
    if (!s_save.saving) wipe_frozen();
    s_pending_n = 0;
}
static void ap_cancel(void *ctx) {                  /* [Cancel], or the box dismissed by a teardown / the idle wipe */
    (void)ctx;
    s_confirming = 0;
    s_pending_n = 0;
    if (!s_save.saving) wipe_frozen();
    if (s_open) cfg_set_status("Not saved.", 0);
}
static void master_save(void) {
    if (s_save.saving || !s_open || s_confirming) return;
    int n = pcfg_edits_export(s_medits, s_mfedits, PCFG_EDIT_MAX);   /* blank secrets dropped: "unchanged" */
    if (n <= 0) { cfg_set_status("No changes to save", 0); return; }
    psvc_fedits_t fe = { s_mfedits, n };
    char err[96];
    err[0] = '\0';
    psvc_mcfg_get(s_scratch);                        /* fresh, as the worker's will be: the display copy has no secrets */
    int frc = psvc_mcfg_fields_fn(s_scratch, &fe, err, sizeof err);
    int vrc = frc == 0 ? hg_mcfg_validate(s_scratch, tz_check, err, sizeof err) : 0;
    pnl_zero(s_scratch, sizeof *s_scratch);
    if (frc != 0 || vrc != 0) {
        wipe_frozen();
        cfg_show_error(&s_mview, err, psvc_rc_token(frc != 0 ? PSVC_E_INVALID_FIELD : PSVC_E_VALIDATION));
        return;
    }
    s_pending_n = n;
    if (touches_ap(s_mfedits, n)) {
        s_confirming = 1;
        pnl_confirm("Change the access point?", "Changing the AP drops every phone connected to it.", "Save anyway",
                    ap_go, ap_cancel, NULL);
        return;
    }
    submit_save();
}

/* ---------- the editor's life ---------- */
void cfg_master_open(lv_obj_t *body) {
    cfg_set_title("Config -- Master");
    if (!buffers_ok()) {
        ESP_LOGW(TAG, "no PSRAM for the master editor");
        lv_obj_t *l = lv_label_create(body);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, "Out of memory (PSRAM) -- the editor cannot open.");
        return;
    }
    s_open = 1;
    s_rerender_due = 0;
    take_copy();
    s_mview = (cfg_view_t){ .table = PCFG_TABLE_MASTER, .zone = 0, .value = master_value, .ctx = NULL,
                            .hw_or_null = NULL, .edits = s_medits, .reveal = master_reveal };
    cfg_frame_build(body, &s_mview, master_save);
    cfg_save_sync(&s_save, 1);                       /* a save still in flight, or its kept outcome */
}

void cfg_master_close(void) {
    s_open = 0;                                      /* first: a dismissed confirm must not touch the status line */
    s_rerender_due = 0;
    if (s_confirming) pnl_confirm_close();           /* runs ap_cancel: the frozen set is wiped */
    if (wdg_keyboard_is_open()) wdg_keyboard_close();   /* wipes a half-typed password */
}

void cfg_master_update(const pnl_snap_t *s) {
    (void)s;
    if (!s_open) return;
    cfg_save_sync(&s_save, 0);                       /* the Save button follows a save that ended off-screen */
    if (s_rerender_due && !wdg_keyboard_is_open()) { s_rerender_due = 0; cfg_frame_rerender(); }
}

/* Task 33: an import from the card was applied -- re-read the copy, as after a Save (the web refetches at once). */
void cfg_master_imported(void) {
    if (!s_m) return;
    take_copy();
    if (s_open) s_rerender_due = 1;                  /* shown at the next update: never under an open keyboard */
}

void cfg_master_wipe(void) {
    if (s_confirming) pnl_confirm_close();
    if (s_medits) pcfg_edits_wipe(s_medits);         /* pending passwords included */
    if (!s_save.saving) wipe_frozen();
    if (s_scratch) pnl_zero(s_scratch, sizeof *s_scratch);
    if (s_m) take_copy();                            /* keeps the no-secret invariant; drops nothing else sensitive */
    if (s_open) s_rerender_due = 1;                  /* dropped edits must lose their dots on screen */
    cfg_save_forget(&s_save);
}
