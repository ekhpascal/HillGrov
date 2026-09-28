/* cfg_zone.c -- the generated editor for zones 1..8 (glue; LVGL task only).
 * Document: psvc_zone_cfg_get on the worker into a PSRAM buffer. Edits: one pcfg_edits_t per zone, kept across
 * tabs and destinations. Save: the dirty set only, pre-validated on a scratch copy, then psvc_zone_cfg_edit on
 * the worker, which applies it to a FRESH copy (last writer wins per field, like the web PUT). The hardware plane
 * is never written: pcfg_edits_set refuses its rows and psvc_zone_fields_fn refuses them again. */
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "scr_config.h"
#include "pnl_worker.h"
#include "pnl_poll.h"
#include "pnl_msg.h"
#include "psvc_zcfg.h"
#include "psvc_rc.h"
#include "wdg_keyboard.h"

static const char *TAG = "cfg_zone";

#define RELOAD_MS   3000u  /* the web refetches 3 s after a 202 (app.js:1594) */
#define HEAL_SETTLE 10     /* polls (~10 s): a zone already ONLINE + synced when its load failed gets one retry then */

typedef struct { hg_zone_cfg_t cfg; hg_zone_hw_t hw; uint32_t gen; int hw_present; } zdoc_t;
typedef struct { uint8_t zone; zdoc_t *dst; } load_arg_t;
typedef struct { uint8_t zone; const psvc_fedit_t *e; int n; } save_arg_t;
typedef enum { ZS_CLOSED = 0, ZS_LOADING, ZS_LOADED, ZS_FAILED } zstate_t;

static zdoc_t        *s_doc;                   /* PSRAM: the document on screen */
static zdoc_t        *s_ldoc;                  /* PSRAM: the load target -- worker-owned while s_load_pending */
static psvc_fedit_t  *s_fedits;                /* PSRAM: the frozen save set -- worker-owned while s_save.saving */
static hg_zone_cfg_t *s_scratch;               /* PSRAM: the local pre-validation copy */
static pcfg_edits_t  *s_edits[HG_MAX_ZONES];   /* PSRAM, lazily per zone */
static cfg_view_t     s_view;
static cfg_save_t     s_save;                  /* the shared save state (scr_config.c, C14) */
static lv_obj_t      *s_body;
static lv_timer_t    *s_reload;
static zstate_t       s_state;
static uint8_t        s_zone, s_load_pending, s_load_again, s_heal_used, s_heal_armed, s_heal_wait,
                      s_watch, s_rerender_due, s_gone;
static uint32_t       s_watch_seq;

static void submit_load(void);
static int  submit_load_job(void);
static void zone_save(void);

static int buffers_ok(void) {
    if (!s_doc)     s_doc     = heap_caps_calloc(1, sizeof *s_doc, MALLOC_CAP_SPIRAM);
    if (!s_ldoc)    s_ldoc    = heap_caps_calloc(1, sizeof *s_ldoc, MALLOC_CAP_SPIRAM);
    if (!s_fedits)  s_fedits  = heap_caps_calloc(PCFG_EDIT_MAX, sizeof *s_fedits, MALLOC_CAP_SPIRAM);
    if (!s_scratch) s_scratch = heap_caps_calloc(1, sizeof *s_scratch, MALLOC_CAP_SPIRAM);
    return s_doc && s_ldoc && s_fedits && s_scratch;
}
static pcfg_edits_t *edits_for(uint8_t zone) {
    pcfg_edits_t **pe = &s_edits[zone - 1];
    if (!*pe) {
        *pe = heap_caps_calloc(1, sizeof **pe, MALLOC_CAP_SPIRAM);
        if (*pe) pcfg_edits_reset(*pe, PCFG_TABLE_ZONE, zone);
        else ESP_LOGW(TAG, "no PSRAM for the zone %u edit set", (unsigned)zone);
    }
    return *pe;
}

static const char *zone_value(void *ctx, uint8_t group, int idx, const hg_field_t *f, char *buf, size_t cap) {
    (void)ctx;
    const void *base = hg_field_base(group, idx, &s_doc->hw, &s_doc->cfg);
    if (!base || hg_field_read(f, base, buf, cap) != 0) snprintf(buf, cap, "?");
    return buf;
}

static void set_title(void) {
    char t[64];
    if (s_state == ZS_LOADED) snprintf(t, sizeof t, "Config -- Zone %u (gen %lu)", (unsigned)s_zone, (unsigned long)s_doc->gen);
    else snprintf(t, sizeof t, "Config -- Zone %u", (unsigned)s_zone);
    cfg_set_title(t);
}

static void body_message(const char *text, int retry);
static void start_loading(void) {
    s_state = ZS_LOADING;
    set_title();
    body_message("Loading...", 0);
    submit_load();
}
static void retry_async(void *unused) {
    (void)unused;
    if (s_state != ZS_FAILED || !s_body) return;
    start_loading();
}
static void ev_retry(lv_event_t *e) { (void)e; lv_async_call(retry_async, NULL); }   /* never delete the button inside its own event */

static void body_message(const char *text, int retry) {
    if (!s_body) return;
    lv_obj_clean(s_body);
    lv_obj_t *l = lv_label_create(s_body);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(l, text);
    if (retry) {
        lv_obj_t *b = lv_button_create(s_body);
        lv_obj_set_size(b, 180, 56);
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text(bl, "Retry");
        lv_obj_center(bl);
        lv_obj_add_event_cb(b, ev_retry, LV_EVENT_CLICKED, NULL);
    }
}
static void load_failed(const char *text) {
    s_state = ZS_FAILED;
    s_heal_armed = 0;                             /* cfg_zone_update decides from the next snapshots */
    s_heal_wait = HEAL_SETTLE;
    set_title();
    body_message(text, 1);
}

static void show_loaded(void) {
    lv_obj_clean(s_body);
    cfg_frame_build(s_body, &s_view, zone_save);
    cfg_save_sync(&s_save, 1);
}

/* ---------- load ---------- */
static void load_run(pnl_job_t *j) {             /* pnl_work: may block; never lv_* */
    load_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    j->rc = psvc_zone_cfg_get(a.zone, &a.dst->cfg, &a.dst->hw, &a.dst->gen, &a.dst->hw_present);
}
static void load_done(pnl_job_t *j) {            /* LVGL task */
    load_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    s_load_pending = 0;
    int live = j->screen_gen == pnl_screen_gen() && s_state != ZS_CLOSED && a.zone == s_zone;
    if (!live) {                                  /* THE RULE: stale -- touch no widget */
        if (s_load_again) { s_load_again = 0; if (s_state == ZS_LOADING) (void)submit_load_job(); }   /* the rebuilt
                                                        editor waits; a refused submit is retried by cfg_zone_update */
        return;
    }
    int again = s_load_again;                     /* a reload asked for while this one was in flight */
    s_load_again = 0;
    if (j->rc == PSVC_OK) {
        int was_loaded = s_state == ZS_LOADED;
        memcpy(s_doc, s_ldoc, sizeof *s_doc);
        s_state = ZS_LOADED;
        s_view.hw_or_null = s_doc->hw_present ? &s_doc->hw : NULL;   /* the hw_present rule (http_api_cfg.c:132-149) */
        set_title();
        if (!was_loaded) show_loaded();
        else if (wdg_keyboard_is_open()) s_rerender_due = 1;         /* never yank a keyboard from under a finger */
        else cfg_frame_rerender();
        if (again) submit_load();
        return;
    }
    char m[200];
    pnl_msg_arg_t ma = { .zone = a.zone };
    pnl_msg(PNL_CTX_ZONE_LOAD, j->rc, &ma, m, sizeof m);
    if (s_state == ZS_LOADED) {                   /* a reload failed: keep the document on screen */
        char r[220];
        snprintf(r, sizeof r, "Reload failed: %s", m);
        cfg_set_status(r, 1);
        return;
    }
    load_failed(m);
}
static int submit_load_job(void) {               /* touches no widget: safe inside a stale done() */
    if (s_load_pending) { s_load_again = 1; return 0; }   /* s_ldoc belongs to the worker until done() */
    load_arg_t a = { .zone = s_zone, .dst = s_ldoc };
    if (pnl_worker_submit(load_run, load_done, &a, sizeof a) != 0) return -1;
    s_load_pending = 1;
    return 0;
}
static void submit_load(void) {
    if (submit_load_job() != 0 && s_state != ZS_LOADED) load_failed("Panel busy -- tap Retry.");
}
static void reload_cb(lv_timer_t *t) {
    (void)t;
    s_reload = NULL;                              /* repeat count 1: LVGL deletes it after this */
    if (s_state == ZS_LOADED) submit_load();
}
static void arm_reload(void) {
    if (s_reload) lv_timer_delete(s_reload);
    s_reload = lv_timer_create(reload_cb, RELOAD_MS, NULL);
    if (s_reload) lv_timer_set_repeat_count(s_reload, 1);
}

/* ---------- save ---------- */
static void cat(char *dst, size_t cap, const char *src) {   /* bounded append; clips silently (status text) */
    size_t l = strlen(dst);
    while (*src && l + 1 < cap) dst[l++] = *src++;
    dst[l] = '\0';
}
static void save_run(pnl_job_t *j) {             /* pnl_work */
    save_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    psvc_fedits_t fe = { a.e, a.n };
    j->rc = psvc_zone_cfg_edit(a.zone, psvc_zone_fields_fn, &fe, j->err, sizeof j->err, (char *)j->out, sizeof j->out);
}
static void save_done(pnl_job_t *j) {            /* LVGL task */
    save_arg_t a;
    memcpy(&a, j->arg, sizeof a);
    j->err[sizeof j->err - 1] = '\0';
    j->out[PNL_JOB_OUT_MAX - 1] = 0;
    int ok = j->rc == PSVC_OK;
    int same = s_state != ZS_CLOSED && a.zone == s_zone;          /* this zone's editor is open (maybe rebuilt) */
    int live = j->screen_gen == pnl_screen_gen() && same && s_state == ZS_LOADED;   /* and its widgets are ours */
    char msg[CFG_SAVE_MSG_MAX];
    pnl_msg_arg_t ma = { .zone = a.zone };
    pnl_msg(PNL_CTX_ZONE_SAVE, j->rc, &ma, msg, sizeof msg);
    const char *w = (const char *)j->out;
    if (ok && w[0]) { cat(msg, sizeof msg, " ("); cat(msg, sizeof msg, w); cat(msg, sizeof msg, ")"); }   /* as the web does */
    if (ok) {
        pcfg_edits_drop_saved(s_edits[a.zone - 1], a.e, a.n);      /* re-edited entries stay dirty */
        if (same && s_state == ZS_LOADED) {       /* merge into the doc on screen, as the web does (app.js:1546-1574) */
            psvc_fedits_t fe = { a.e, a.n };
            char e2[8], w2[8];
            (void)psvc_zone_fields_fn(&s_doc->cfg, NULL, &fe, e2, sizeof e2, w2, sizeof w2);
        }
        if (same) {                               /* follow cfg_busy / cfg_sync, then the authoritative refetch */
            s_watch = 1;
            s_watch_seq = pnl_poll_seq();
            arm_reload();
        }
        pnl_poll_kick();
    }
    if (!live) {                                  /* stale: the outcome waits for the next frame, named by zone */
        char k[CFG_SAVE_MSG_MAX];
        snprintf(k, sizeof k, "Zone %u: ", (unsigned)a.zone);
        cat(k, sizeof k, msg);
        if (!ok && j->err[0]) { cat(k, sizeof k, ": "); cat(k, sizeof k, j->err); }
        cfg_save_end(&s_save, 0, k, !ok);
        if (ok && same) s_rerender_due = 1;       /* the merged doc shows at the next update */
    } else {
        cfg_save_end(&s_save, 1, msg, !ok);
        if (ok) {
            if (wdg_keyboard_is_open()) s_rerender_due = 1; else cfg_frame_rerender();
        } else if (j->err[0]) {
            cfg_show_error(&s_view, j->err, psvc_rc_token(j->rc));
        }
    }
    memset(s_fedits, 0, PCFG_EDIT_MAX * sizeof *s_fedits);   /* the frozen set is ours again (zones hold no secrets) */
}
static void zone_save(void) {
    if (s_save.saving || s_state != ZS_LOADED) return;
    int n = s_view.edits ? pcfg_edits_export(s_view.edits, s_fedits, PCFG_EDIT_MAX) : 0;
    if (n <= 0) { cfg_set_status("No changes to save", 0); return; }
    const hg_zone_hw_t *hw = s_doc->hw_present ? &s_doc->hw : NULL;     /* the hw_present rule, both ways */
    psvc_fedits_t fe = { s_fedits, n };
    char err[96], warn[8];
    err[0] = '\0';
    *s_scratch = s_doc->cfg;                      /* the document on screen: the same path shapes the worker's refusal has */
    if (psvc_zone_fields_fn(s_scratch, hw, &fe, err, sizeof err, warn, sizeof warn) != 0) {
        cfg_show_error(&s_view, err, psvc_rc_token(PSVC_E_INVALID_FIELD));
        return;
    }
    if (hg_cfg_validate(s_scratch, hw, err, sizeof err) != 0) {
        cfg_show_error(&s_view, err, psvc_rc_token(PSVC_E_VALIDATION));
        return;
    }
    save_arg_t a = { .zone = s_zone, .e = s_fedits, .n = n };
    if (pnl_worker_submit(save_run, save_done, &a, sizeof a) != 0) { cfg_set_status("Panel busy, retry", 1); return; }
    s_watch = 0;                                  /* a new save restarts the landing watch */
    cfg_save_begin(&s_save);
}

/* ---------- the editor's life ---------- */
void cfg_zone_open(lv_obj_t *body, uint8_t zone) {
    s_body = body;
    s_zone = zone;
    s_heal_used = 0; s_watch = 0; s_rerender_due = 0; s_gone = 0;
    s_state = ZS_LOADING;
    set_title();
    if (!buffers_ok()) {
        s_state = ZS_FAILED;
        s_heal_used = 1;                          /* a retry cannot conjure PSRAM */
        body_message("Out of memory (PSRAM) -- the editor cannot open.", 0);
        return;
    }
    s_view = (cfg_view_t){ .table = PCFG_TABLE_ZONE, .zone = zone, .value = zone_value, .ctx = NULL,
                           .hw_or_null = NULL, .edits = edits_for(zone), .reveal = NULL };
    start_loading();
}

void cfg_zone_close(void) {
    if (s_reload) { lv_timer_delete(s_reload); s_reload = NULL; }
    if (wdg_keyboard_is_open()) wdg_keyboard_close();
    s_state = ZS_CLOSED;
    s_body = NULL;
    s_zone = 0;
    s_watch = 0;
    s_rerender_due = 0;
}

/* One self-heal per visit when the zone TURNS ONLINE with cfg_sync OK: a zone seen not ready after the failure
 * heals on its first ready snapshot; a zone that was already ready (a never-synced zone adopting its config) gets
 * the one retry after HEAL_SETTLE polls instead of at once. */
static void heal_check(const hg_node_t *n, const pnl_snap_t *s) {
    if (s_state != ZS_FAILED || s_heal_used || s_load_pending) return;
    int ready = n->health == NODE_H_ONLINE && !s->st.cfg_sync_failed[s_zone - 1];
    if (!ready) { s_heal_armed = 1; return; }
    if (!s_heal_armed && s_heal_wait && --s_heal_wait) return;
    s_heal_used = 1;
    start_loading();
}

void cfg_zone_update(const pnl_snap_t *s) {
    if (s_state == ZS_CLOSED || s_zone < 1 || !s) return;
    if (s_state == ZS_LOADED) cfg_save_sync(&s_save, 0);   /* the Save button follows a save that ended off-screen */
    const hg_node_t *n = &s->st.node[s_zone - 1];
    if (!n->used) {
        if (!s_gone) { s_gone = 1; cfg_set_status("This zone is no longer enrolled.", 1); }
        return;
    }
    s_gone = 0;
    heal_check(n, s);
    if (s_state == ZS_LOADING && !s_load_pending) submit_load();   /* a stale done() could not resubmit */
    if (s_state != ZS_LOADED) return;
    /* seq >= watch + 2: that poll's gather began after the save was queued (the first publish after done() may
     * have read cfg_busy before node_mgr_cfg_set) */
    if (s_watch && !s_save.saving && s->seq >= s_watch_seq + 2u && !s->cfg_busy[s_zone - 1]) {
        s_watch = 0;              /* while busy the status keeps "Queued, pushing to zone" (+ warnings) */
        if (s->st.cfg_sync_failed[s_zone - 1])   /* 1 only while a plane's sec 4.4 latch is set -- not sticky */
            cfg_set_status("The zone refused the new config (CFG_SYNC_FAILED) -- see Alarms.", 1);
        else cfg_set_status("Landed on the zone.", 0);
    }
    if (s_rerender_due && !wdg_keyboard_is_open()) { s_rerender_due = 0; cfg_frame_rerender(); }
}

/* Task 33: an import from the card was queued for this zone (psvc_zone_cfg_edit OK) -- the web's cfgApplyPut: that
 * zone's unsaved edits are discarded (open or not), and an open editor follows cfg_busy / cfg_sync to "Landed on the
 * zone." and refetches the document 3 s on. */
void cfg_zone_imported(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    if (s_edits[zone - 1]) pcfg_edits_wipe(s_edits[zone - 1]);   /* the web: cfgDirty[id] = {} on success (app.js:1583) */
    if (s_state != ZS_LOADED || zone != s_zone) return;   /* another zone, or not loaded: its next open loads fresh */
    s_rerender_due = 1;                           /* the dropped edits lose their dots now, not only at the reload */
    s_watch = 1;
    s_watch_seq = pnl_poll_seq();
    arm_reload();
}

void cfg_zone_wipe_all(void) {
    for (int z = 0; z < HG_MAX_ZONES; z++) if (s_edits[z]) pcfg_edits_wipe(s_edits[z]);
    if (s_doc && s_state == ZS_CLOSED) memset(s_doc, 0, sizeof *s_doc);
    else if (s_state == ZS_LOADED) s_rerender_due = 1;   /* dropped edits must lose their dots on screen */
    cfg_save_forget(&s_save);
}
