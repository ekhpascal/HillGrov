/* cfg_card.c -- Config export and import through the microSD card (glue; D13). The web exports the document it has
 * cached (GET /api/config, zone 0 with ?secrets=0) as a download and imports a picked file by PUTting it unchanged,
 * dropping a blank STA_PASS / AP_PASS for zone 0 first (web/app.js:1102-1108, 1756-1784). The panel writes and reads
 * the same JSON on the card:
 *   - Export: the same document GET builds (hg_json_export_cfg with the hw plane zeroed when absent, or
 *     hg_json_export_mcfg with secrets omitted -- no Wi-Fi password ever reaches the card, as none reaches the web's
 *     download) to PNL_SD_DIR "/zone<N>.json" | "/master.json".
 *   - Import: that file, at most 4096 bytes (the web's PUT body cap; larger is refused like its 413), through the same
 *     edit paths as the web PUT: psvc_zone_cfg_edit(psvc_zone_json_fn) -- the "hw" section only becomes warnings, the
 *     hardware plane is never written -- or psvc_mcfg_edit(psvc_mcfg_json_import_fn), where a blank secret means
 *     unchanged (the web client's rule, moved into hg_json). Same validation, same outcomes as Save; on success that
 *     editor's unsaved edits are discarded, as the web's cfgApplyPut does (app.js:1583), and the confirm says so.
 * Card rules (pnl_sd.h): every card operation runs on the worker, mount -> use -> unmount, each FILE* is closed before
 * the unmount, FAT32 only, never formatted. Nothing here logs: a file's contents never reach a log, and the document
 * buffer is wiped (pnl_zero) after every job, since an import file may carry passwords.
 * THE RULE: a done() touches a card button only through its pointer, which the button's own LV_EVENT_DELETE clears
 * (and cfg_card_teardown() drops when the editor closes); the outcome goes to the status line while the Config screen
 * that queued it is still up or a card bar is live on a rebuilt one, and is kept for the next editor frame otherwise
 * (cfg_card_wipe drops it). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "hg_json.h"
#include "psvc_mcfg.h"
#include "psvc_zcfg.h"
#include "pnl_sd.h"
#include "pnl_input.h"      /* pnl_zero */
#include "pnl_msg.h"
#include "pnl_poll.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"     /* pnl_kit_button / pnl_kit_enable, and pnl_confirm (pnl_theme.h) */
#include "scr_config.h"

#define CARD_DOC_MAX 4096u   /* the web's PUT body cap (http_api_cfg.c:171): an export that fits is an import that fits */
#define CARD_KEPT_MAX (PNL_JOB_OUT_MAX + 32)   /* "Zone N: " + a job outcome */

typedef struct { uint8_t zone, import; } card_arg_t;

static char     *s_doc;               /* CARD_DOC_MAX + 1, PSRAM; the worker owns it while s_busy */
static uint8_t   s_busy, s_zone, s_confirming, s_kept_err;
static lv_obj_t *s_exp_btn, *s_imp_btn;
static char      s_kept[CARD_KEPT_MAX];   /* an outcome that landed after its Config screen was torn down */

static void card_path(uint8_t zone, char *out, size_t cap) {
    if (zone == 0) snprintf(out, cap, "%s/master.json", PNL_SD_DIR);
    else snprintf(out, cap, "%s/zone%u.json", PNL_SD_DIR, (unsigned)zone);
}

/* ---------- worker side ---------- */
static void export_run(pnl_job_t *j) {
    const card_arg_t *a = (const card_arg_t *)j->arg;
    char *out = (char *)j->out;
    j->irc = 1;
    int n;
    if (a->zone == 0) {
        hg_mcfg_t m;
        psvc_mcfg_get(&m);
        n = hg_json_export_mcfg(&m, 0, s_doc, CARD_DOC_MAX);   /* secrets omitted: no password ever reaches the card */
        pnl_zero(&m, sizeof m);                 /* the copy holds both Wi-Fi passwords; memset here is a dead store */
    } else {
        hg_zone_cfg_t cfg;
        hg_zone_hw_t hw;
        uint32_t gen = 0;
        int hw_present = 0;
        psvc_rc_t rc = psvc_zone_cfg_get(a->zone, &cfg, &hw, &gen, &hw_present);   /* hw zeroed when absent, as GET */
        if (rc != PSVC_OK) {
            pnl_msg_arg_t ma = { .zone = a->zone };
            pnl_msg(PNL_CTX_ZONE_LOAD, rc, &ma, out, PNL_JOB_OUT_MAX);
            return;                             /* the card was never mounted */
        }
        n = hg_json_export_cfg(&hw, &cfg, gen, s_doc, CARD_DOC_MAX);
    }
    if (n < 0) {
        snprintf(out, PNL_JOB_OUT_MAX, "Export failed (document too large)");
        pnl_zero(s_doc, CARD_DOC_MAX + 1);
        return;
    }
    pnl_sd_rc_t sd = pnl_sd_mount();
    if (sd != PNL_SD_OK) {
        snprintf(out, PNL_JOB_OUT_MAX, "%s", pnl_sd_rc_text(sd));
        pnl_zero(s_doc, CARD_DOC_MAX + 1);
        return;
    }
    char path[48];
    card_path(a->zone, path, sizeof path);
    (void)mkdir(PNL_SD_DIR, 0775);              /* EEXIST is fine; a real failure shows as the fopen failing */
    FILE *f = fopen(path, "w");
    size_t w = f ? fwrite(s_doc, 1, (size_t)n, f) : 0;
    int ce = f ? fclose(f) : -1;                /* before the unmount */
    pnl_sd_unmount();
    pnl_zero(s_doc, CARD_DOC_MAX + 1);
    if (!f || w != (size_t)n || ce != 0) snprintf(out, PNL_JOB_OUT_MAX, "microSD write failed");
    else { snprintf(out, PNL_JOB_OUT_MAX, "Exported to %s", path + strlen(PNL_SD_MOUNT)); j->irc = 0; }
}

static void import_run(pnl_job_t *j) {
    const card_arg_t *a = (const card_arg_t *)j->arg;
    char *out = (char *)j->out;
    j->irc = 1;
    pnl_sd_rc_t sd = pnl_sd_mount();
    if (sd != PNL_SD_OK) { snprintf(out, PNL_JOB_OUT_MAX, "%s", pnl_sd_rc_text(sd)); return; }
    char path[48];
    card_path(a->zone, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        pnl_sd_unmount();
        snprintf(out, PNL_JOB_OUT_MAX, "No %s on the card", path + strlen(PNL_SD_MOUNT));
        return;
    }
    size_t n = fread(s_doc, 1, CARD_DOC_MAX + 1, f);
    int bad = ferror(f);
    fclose(f);                                  /* before the unmount */
    pnl_sd_unmount();                           /* the card is not needed for the apply */
    if (bad || n > CARD_DOC_MAX) {
        snprintf(out, PNL_JOB_OUT_MAX, "%s", bad ? "microSD read failed" : "File too large (4096 bytes max)");
        pnl_zero(s_doc, CARD_DOC_MAX + 1);
        return;
    }
    s_doc[n] = '\0';
    char err[96] = "", warn[160] = "";
    psvc_rc_t rc;
    pnl_msg_ctx_t ctx;
    if (a->zone == 0) {
        rc = psvc_mcfg_edit(psvc_mcfg_json_import_fn, s_doc, PSVC_LOCK_PANEL_MS, "PANEL IMPORT", err, sizeof err);
        ctx = PNL_CTX_MCFG_SAVE;
    } else {
        rc = psvc_zone_cfg_edit(a->zone, psvc_zone_json_fn, s_doc, err, sizeof err, warn, sizeof warn);
        ctx = PNL_CTX_ZONE_SAVE;
    }
    pnl_zero(s_doc, CARD_DOC_MAX + 1);          /* an import file may carry secrets */
    if (rc == PSVC_E_BAD_JSON) {
        snprintf(out, PNL_JOB_OUT_MAX, "Invalid JSON file");   /* app.js:1770 */
        return;
    }
    char m[128];
    pnl_msg_arg_t ma = { .zone = a->zone };
    pnl_msg(ctx, rc, &ma, m, sizeof m);
    /* err is a field path and warn a list of key paths ("hw.<path> readonly", unknown keys): never a value.
     * Bounded so -Wformat-truncation (-Wall -Werror) can prove the fit: 127 + " (" + 120 + ")" + NUL = 251 <= 256, and
     * 127 + ": " + 95 + NUL = 225 <= 256. A longer warning list is cut at 120 characters on the glass. */
    if (rc == PSVC_OK && warn[0]) snprintf(out, PNL_JOB_OUT_MAX, "%s (%.120s)", m, warn);   /* hw keys -> warnings, like Save */
    else if (rc != PSVC_OK && err[0]) snprintf(out, PNL_JOB_OUT_MAX, "%s: %s", m, err);
    else snprintf(out, PNL_JOB_OUT_MAX, "%s", m);
    j->irc = rc == PSVC_OK ? 0 : 1;
}

/* ---------- LVGL side ---------- */
static void card_done(pnl_job_t *j) {
    card_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    j->out[PNL_JOB_OUT_MAX - 1] = 0;
    const char *out = (const char *)j->out;
    int is_err = j->irc != 0;
    s_busy = 0;
    if (a.import && !is_err) {                  /* the Save outcomes: refetch the document, follow the push */
        pnl_poll_kick();
        if (a.zone == 0) cfg_master_imported();
        else cfg_zone_imported(a.zone);
    }
    pnl_kit_enable(s_exp_btn, 1);               /* NULL-safe; NULL once the buttons are gone (btn_deleted) */
    pnl_kit_enable(s_imp_btn, 1);
    char who[24] = "";
    if (a.zone == 0) snprintf(who, sizeof who, "Master: ");
    else snprintf(who, sizeof who, "Zone %u: ", (unsigned)a.zone);
    /* Shown now while the Config screen that queued it is still up, or while a card bar is live on a rebuilt one:
     * teardown NULLs the old pointers, so a non-NULL s_imp_btn is the current screen's bar (status NULL-safe). */
    if (j->screen_gen == pnl_screen_gen() || s_imp_btn) {
        char t[CARD_KEPT_MAX];
        snprintf(t, sizeof t, "%s%s", (s_imp_btn && s_zone == a.zone) ? "" : who, out);   /* another editor: say whose */
        s_kept[0] = '\0';
        cfg_set_status(t, is_err);
        return;
    }
    snprintf(s_kept, sizeof s_kept, "%s%s", who, out);   /* THE RULE: stale -- touch no widget; shown by the next bar */
    s_kept_err = (uint8_t)is_err;
}

static int card_submit(pnl_job_run_fn run, uint8_t zone, uint8_t import) {
    if (s_busy) { cfg_set_status("microSD busy -- try again", 1); return -1; }
    if (!s_doc) s_doc = heap_caps_calloc(1, CARD_DOC_MAX + 1, MALLOC_CAP_SPIRAM);
    if (!s_doc) { cfg_set_status("microSD unavailable (no memory)", 1); return -1; }
    card_arg_t a = { .zone = zone, .import = import };
    if (pnl_worker_submit(run, card_done, &a, sizeof a) != 0) { cfg_set_status("Panel busy -- try again", 1); return -1; }
    s_busy = 1;
    s_kept[0] = '\0';                           /* superseded */
    pnl_kit_enable(s_exp_btn, 0);
    pnl_kit_enable(s_imp_btn, 0);
    cfg_set_status("...", 0);
    return 0;
}

void cfg_card_export(uint8_t zone) { (void)card_submit(export_run, zone, 0); }

static void import_go(void *ctx) {
    uint8_t zone = (uint8_t)(intptr_t)ctx;
    s_confirming = 0;
    if (!s_imp_btn || zone != s_zone) return;   /* the editor that asked is gone: nothing is applied */
    (void)card_submit(import_run, zone, 1);
}

static void import_cancel(void *ctx) { (void)ctx; s_confirming = 0; }

void cfg_card_import(uint8_t zone) {
    if (s_busy) { cfg_set_status("microSD busy -- try again", 1); return; }
    char path[48], t[200];
    card_path(zone, path, sizeof path);
    if (zone == 0)
        snprintf(t, sizeof t, "Apply %s to the master? It may change Wi-Fi: changing the AP drops every phone connected "
                 "to it. Unsaved edits in this editor are discarded.", path + strlen(PNL_SD_MOUNT));
    else
        snprintf(t, sizeof t, "Apply %s to zone %u? Unsaved edits in this editor are discarded.",
                 path + strlen(PNL_SD_MOUNT), (unsigned)zone);
    s_confirming = 1;                           /* before the call: on_cancel may run inside it (pnl_theme.h) */
    pnl_confirm("Import from card?", t, "Import", import_go, import_cancel, (void *)(intptr_t)zone);
}

static void exp_click(lv_event_t *e) { (void)e; cfg_card_export(s_zone); }
static void imp_click(lv_event_t *e) { (void)e; cfg_card_import(s_zone); }

static void btn_deleted(lv_event_t *e) {        /* ruling C2: a done() after the frame went never touches a freed button */
    lv_obj_t *o = lv_event_get_target_obj(e);
    if (o == s_exp_btn) s_exp_btn = NULL;
    if (o == s_imp_btn) s_imp_btn = NULL;
}

void cfg_card_bar(lv_obj_t *bar) {
    s_exp_btn = pnl_kit_button(bar, "Export to card", exp_click, NULL);
    s_imp_btn = pnl_kit_button(bar, "Import from card", imp_click, NULL);
    if (s_exp_btn) lv_obj_add_event_cb(s_exp_btn, btn_deleted, LV_EVENT_DELETE, NULL);
    if (s_imp_btn) lv_obj_add_event_cb(s_imp_btn, btn_deleted, LV_EVENT_DELETE, NULL);
    pnl_kit_enable(s_exp_btn, !s_busy);
    pnl_kit_enable(s_imp_btn, !s_busy);
    if (!s_busy && s_kept[0]) { cfg_set_status(s_kept, s_kept_err); s_kept[0] = '\0'; }   /* a stale done()'s outcome */
}

void cfg_card_set_zone(uint8_t zone) { s_zone = zone; }

void cfg_card_teardown(void) {
    s_exp_btn = s_imp_btn = NULL;
    if (s_confirming) pnl_confirm_close();      /* runs import_cancel: nothing is applied */
}

void cfg_card_wipe(void) {                      /* the idle wipe: the kept outcome (the document is wiped per job) */
    s_kept[0] = '\0';
    s_kept_err = 0;
}
