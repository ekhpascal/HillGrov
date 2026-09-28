/* Glue: System -> Firmware, the web's Firmware card (web/app.js:970-976, 1866-1907) fed from the microSD card instead of
 * a file picker. Read card lists the .bin files with their kind and version (pnl_sd_classify on each image's own
 * header); only master and zone images get an Install button. An install runs on the worker through the same install
 * core as POST /api/fw/{master,zone} (psvc_fw_install), so every refusal the web gives -- a fleet run, an OTA trial, a
 * web upload in flight (UPLOAD_ACTIVE), a zone pulling /fw/zone.bin (ZONE_FW_BUSY), the wrong image -- is given here
 * with the same meaning, and while the panel installs the web gets 409 UPLOAD_ACTIVE and SET FW ZONE gets ERR FW_BUSY.
 * A master install ends with "Reboot now" (the one reboot flow, sys_reboot_confirm).
 * Card rules (pnl_sd.h): every card operation runs on the worker, mount -> use -> unmount, and the install's FILE* is
 * closed before the unmount on every path. An install whose file no longer has the listed size (the card was swapped
 * between Read card and Install) is refused before anything is written. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "psvc_fw.h"
#include "pnl_sd.h"
#include "pnl_msg.h"
#include "pnl_worker.h"
#include "pnl_ui_kit.h"
#include "scr_system.h"

#define FW_MAX_FILES 16

typedef struct { char path[96]; uint32_t size; uint8_t kind; } inst_arg_t;   /* 101 B by value */

static pnl_sd_file_t  *s_files;                 /* PSRAM FW_MAX_FILES; the worker owns it while s_reading */
static pnl_fw_class_t  s_cls[FW_MAX_FILES];     /* written by the worker while s_reading, like s_files */
static char            s_ver[FW_MAX_FILES][33];
static int             s_n = -1;                /* -1 = the card has not been read */
static char            s_read_err[128];
static uint8_t         s_reading, s_installing;
static char            s_fw_last[128];          /* the last install's outcome: survives leaving and re-entering System */
static uint8_t         s_fw_last_ok, s_fw_last_kind; /* kind: 0 none, else 1 + psvc_fw_kind_t */
static lv_obj_t       *s_list, *s_read_btn, *s_msg, *s_bar, *s_reboot_btn, *s_hint;
static lv_timer_t     *s_prog;

/* Results are kept in module state and drawn through NULL-safe helpers, never gated on screen_gen: the section's
 * teardown NULLs every widget pointer, so a done() that lands while the operator is elsewhere draws nothing, and one
 * that lands after they came back reaches the rebuilt widgets (a gen check would leave "Reading the card..." or a
 * frozen bar on screen until yet another navigation). */

static const char *kind_text(pnl_fw_class_t c) {
    switch (c) {
    case PNL_FW_MASTER:     return "master";
    case PNL_FW_ZONE:       return "zone";
    case PNL_FW_RADIO:      return "radio (not installable here)";
    case PNL_FW_WRONG_CHIP: return "wrong chip";
    default:                return "unknown";
    }
}

static void install_click(lv_event_t *e);

static void render_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    pnl_kit_enable(s_read_btn, !s_reading && !s_installing);
    lv_obj_t *m;
    if (s_reading) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "Reading the card...", PNL_KIT_INFO); return; }
    if (s_read_err[0]) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, s_read_err, PNL_KIT_ERR); return; }
    if (s_n < 0) {
        m = pnl_kit_msg(s_list);
        pnl_kit_msg_set(m, "Put .bin files in the card's root or in /hillgrow, then Read card.", PNL_KIT_INFO);
        return;
    }
    if (s_n == 0) { m = pnl_kit_msg(s_list); pnl_kit_msg_set(m, "No .bin files on the card.", PNL_KIT_INFO); return; }
    for (int i = 0; i < s_n; i++) {
        lv_obj_t *r = pnl_kit_row(s_list);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char t[160];
        snprintf(t, sizeof t, "%.63s  |  %s%s%.32s  |  %lu KB", s_files[i].name, kind_text(s_cls[i]),
                 s_ver[i][0] ? " v" : "", s_ver[i], (unsigned long)(s_files[i].size / 1024u));
        lv_obj_t *l = lv_label_create(r);
        lv_label_set_text(l, t);
        lv_obj_set_flex_grow(l, 1);
        if (s_cls[i] == PNL_FW_MASTER || s_cls[i] == PNL_FW_ZONE) {
            lv_obj_t *b = pnl_kit_button(r, "Install", install_click, (void *)(intptr_t)i);
            pnl_kit_enable(b, !s_installing);
        }
    }
}

/* ---- read the card ---- */
static void read_run(pnl_job_t *j) {
    pnl_sd_rc_t rc = pnl_sd_mount();
    int n = -1;
    if (rc == PNL_SD_OK) {
        n = pnl_sd_list_bins(s_files, FW_MAX_FILES);
        pnl_sd_unmount();
        if (n < 0) rc = PNL_SD_IO;
    }
    if (n > 0) {
        for (int i = 0; i < n; i++) s_cls[i] = pnl_sd_classify(&s_files[i], CONFIG_IDF_FIRMWARE_CHIP_ID, NULL);
        pnl_sd_sort(s_files, s_cls, n);
        for (int i = 0; i < n; i++) (void)pnl_sd_classify(&s_files[i], CONFIG_IDF_FIRMWARE_CHIP_ID, s_ver[i]);
    }
    j->irc = (int)rc;
    memcpy(j->out, &n, sizeof n);
}

static void read_done(pnl_job_t *j) {
    int n;
    memcpy(&n, j->out, sizeof n);
    s_reading = 0;
    if (j->irc != PNL_SD_OK) { s_n = -1; snprintf(s_read_err, sizeof s_read_err, "%s", pnl_sd_rc_text((pnl_sd_rc_t)j->irc)); }
    else { s_n = n; s_read_err[0] = '\0'; }
    render_list();                              /* NULL-safe: draws only if the section is up (see the note above) */
}

static void read_click(lv_event_t *e) {
    (void)e;
    if (s_reading || s_installing) return;
    if (!s_files) s_files = heap_caps_calloc(FW_MAX_FILES, sizeof *s_files, MALLOC_CAP_SPIRAM);
    if (!s_files) { snprintf(s_read_err, sizeof s_read_err, "Card listing unavailable (no memory)"); render_list(); return; }
    if (pnl_worker_submit(read_run, read_done, NULL, 0) != 0) {
        snprintf(s_read_err, sizeof s_read_err, "Panel busy -- try again");
    } else {
        s_reading = 1;
        s_read_err[0] = '\0';
    }
    render_list();
}

/* ---- install ---- */
static void prog_tick(lv_timer_t *t) {
    (void)t;
    const char *k = NULL;
    uint8_t pct = 0;
    if (psvc_fw_progress(&k, &pct) && s_bar) lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);   /* no animation: flash erases stall UI */
}

static void prog_stop(void) {
    if (s_prog) { lv_timer_delete(s_prog); s_prog = NULL; }
}

static void inst_run(pnl_job_t *j) {
    const inst_arg_t *a = (const inst_arg_t *)j->arg;
    psvc_fw_result_t res;
    memset(&res, 0, sizeof res);
    pnl_sd_rc_t sd = pnl_sd_mount();
    j->irc = (int)sd;
    j->rc = PSVC_E_RECV_FAILED;                 /* the file could not be opened: "microSD read failed" */
    if (sd == PNL_SD_OK) {
        FILE *f = fopen(a->path, "rb");
        if (f) {
            struct stat fs;
            if (fstat(fileno(f), &fs) != 0 || fs.st_size < 0 || (uint32_t)fs.st_size != a->size) {
                /* the card was swapped (or the file rewritten) between Read card and Install: the listing's size
                 * and header no longer describe this file, so nothing is installed from it */
                snprintf(j->err, sizeof j->err, "The file on the card changed since Read card -- nothing was "
                         "written. Read the card again.");
            } else {
                pnl_sd_src_t src = { .f = f, .left = a->size };
                psvc_fw_stats_t st;
                j->rc = psvc_fw_install((psvc_fw_kind_t)a->kind, a->size, pnl_sd_read, &src, &res, &st);
            }
            fclose(f);                          /* before the unmount, on every path */
        }
        pnl_sd_unmount();
    }
    memcpy(j->out, &res, sizeof res);
}

static void reboot_click(lv_event_t *e) { (void)e; sys_reboot_confirm("to run the new image"); }

/* Draws the running install or the kept outcome (or nothing) on whatever widgets exist now; a no-op while torn down.
 * Idempotent: every widget it owns is set from module state each time. */
static void show_outcome(void) {
    if (!s_msg) return;
    int master_ok = !s_installing && s_fw_last_ok && s_fw_last_kind == 1 + PSVC_FW_MASTER;
    int zone_ok = !s_installing && s_fw_last_ok && s_fw_last_kind == 1 + PSVC_FW_ZONE;
    if (s_installing) lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    if (master_ok) lv_obj_remove_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    pnl_kit_msg_set(s_hint, zone_ok ? "Push it to zones from Fleet." : "", PNL_KIT_INFO);
    if (s_installing)
        pnl_kit_msg_set(s_msg, "Installing -- the screen may pause while flash is erased.", PNL_KIT_INFO);
    else if (s_fw_last_kind)
        pnl_kit_msg_set(s_msg, s_fw_last, s_fw_last_ok ? PNL_KIT_OK : PNL_KIT_ERR);
    else
        pnl_kit_msg_set(s_msg, "", PNL_KIT_INFO);
}

static void inst_done(pnl_job_t *j) {
    const inst_arg_t *a = (const inst_arg_t *)j->arg;
    psvc_fw_result_t res;
    memcpy(&res, j->out, sizeof res);
    s_installing = 0;
    prog_stop();
    j->err[sizeof j->err - 1] = '\0';
    int changed = j->err[0] != '\0';            /* inst_run's size check refused the file (nothing written) */
    if (j->irc != PNL_SD_OK) {
        snprintf(s_fw_last, sizeof s_fw_last, "%s", pnl_sd_rc_text((pnl_sd_rc_t)j->irc));
    } else if (changed) {
        snprintf(s_fw_last, sizeof s_fw_last, "%s", j->err);
    } else {
        pnl_msg_arg_t ma = { .zone = 0, .version = res.version, .slot = res.slot, .len = res.len };
        pnl_msg(a->kind == PSVC_FW_MASTER ? PNL_CTX_FW_MASTER : PNL_CTX_FW_ZONE, j->rc, &ma, s_fw_last, sizeof s_fw_last);
    }
    s_fw_last_ok = (uint8_t)(j->irc == PNL_SD_OK && !changed && j->rc == PSVC_OK);
    s_fw_last_kind = (uint8_t)(1 + a->kind);    /* module state first; the widgets may be gone (note above) */
    show_outcome();
    render_list();
}

static void install_go(void *ctx) {
    int i = (int)(intptr_t)ctx;
    if (!s_msg || s_installing || s_reading || i < 0 || i >= s_n) return;   /* the section is up and the list is current */
    inst_arg_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.path, sizeof a.path, "%s", s_files[i].path);
    a.size = s_files[i].size;
    a.kind = (uint8_t)(s_cls[i] == PNL_FW_MASTER ? PSVC_FW_MASTER : PSVC_FW_ZONE);
    if (pnl_worker_submit(inst_run, inst_done, &a, sizeof a) != 0) { pnl_kit_msg_set(s_msg, "Panel busy -- try again", PNL_KIT_ERR); return; }
    s_installing = 1;
    s_fw_last_kind = 0;                         /* the previous outcome is superseded */
    s_fw_last_ok = 0;
    s_fw_last[0] = '\0';
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    show_outcome();                             /* the bar and "Installing --"; Reboot now and the hint hidden */
    if (!s_prog) s_prog = lv_timer_create(prog_tick, 250, NULL);
    render_list();
}

static void install_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    char t[200];
    if (s_cls[i] == PNL_FW_MASTER)
        snprintf(t, sizeof t, "Write %.63s (master v%.32s) to the inactive slot? The master keeps running; reboot "
                 "afterwards to start it.", s_files[i].name, s_ver[i]);
    else
        snprintf(t, sizeof t, "Store %.63s (zone v%.32s) as the zone image? The stored zone image is replaced.",
                 s_files[i].name, s_ver[i]);
    pnl_confirm("Install firmware?", t, "Install", install_go, NULL, (void *)(intptr_t)i);
}

/* The idle wipe (Task 27, D18): the kept install outcome (and its Reboot now) and the card listing. A read or an
 * install still running keeps its state -- its done() owns it (the idle wipe waits for an empty worker anyway). */
void sys_fw_wipe(void) {
    if (!s_installing) { s_fw_last[0] = '\0'; s_fw_last_ok = 0; s_fw_last_kind = 0; }
    if (!s_reading) { s_n = -1; s_read_err[0] = '\0'; }
    show_outcome();                             /* NULL-safe */
    render_list();                              /* NULL-safe */
}

/* ---- section ---- */
static void fw_build(lv_obj_t *parent) {
    lv_obj_t *c = pnl_kit_card(parent, "Firmware from microSD");
    lv_obj_t *note = pnl_kit_msg(c);
    pnl_kit_msg_set(note, "FAT32 cards only. Master and zone images are recognised from their own header.", PNL_KIT_INFO);
    s_read_btn = pnl_kit_button(c, "Read card", read_click, NULL);
    s_list = lv_obj_create(c);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_height(s_list, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    s_bar = lv_bar_create(c);
    lv_obj_set_width(s_bar, LV_PCT(100));
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    s_msg = pnl_kit_msg(c);
    s_hint = pnl_kit_msg(c);
    s_reboot_btn = pnl_kit_button(c, "Reboot now", reboot_click, NULL);
    lv_obj_add_flag(s_reboot_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_installing && !s_prog) s_prog = lv_timer_create(prog_tick, 250, NULL);   /* rebuilt while an install runs */
    show_outcome();                             /* the running install, or the kept outcome (and Reboot now) */
    render_list();
}

static void fw_teardown(void) {
    prog_stop();
    s_list = s_read_btn = s_msg = s_bar = s_reboot_btn = s_hint = NULL;
}

const pnl_sys_section_t PNL_SYS_FIRMWARE = { .title = "Firmware", .build = fw_build, .update = NULL,
                                             .teardown = fw_teardown };
