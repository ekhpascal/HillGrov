#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "hg_json.h"
#include "time_core.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "psvc_mcfg.h"

static const char *TAG = "psvc_mcfg";

/* The secret wipe for this file's stack copies. The master builds with -Os: GCC drops a plain memset on a local that
 * is never read again as a dead store; volatile stores cannot be dropped. panel_ui's pnl_zero() (pnl_zero.h) is the
 * same thing, but panel_svc sits below panel_ui and cannot include it. Pure C: the host suite builds this file. */
static inline void psvc_wipe(void *p, size_t n) {
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) *v++ = 0;
}

typedef struct {
    psvc_mcfg_fn fn;
    void        *ctx;
    char        *err;
    size_t       errcap;
} edit_ctx_t;

/* Runs inside mcfg_ops_edit_ms() on its private copy. The explicit validate is
 * what gives a refusal its field path: mcfg_commit()'s own check has no path
 * out (the old cfg_put_zone0 comment, http_api_cfg.c:185-191). */
static int edit_tramp(hg_mcfg_t *m, void *c_) {
    edit_ctx_t *c = (edit_ctx_t *)c_;
    int rc = c->fn(m, c->ctx, c->err, c->errcap);
    if (rc != 0) return rc > 0 ? rc : PSVC_EDIT_VALIDATION;   /* an fn must never return negative (mcfg_ops.h) */
    if (hg_mcfg_validate(m, tz_check, c->err, c->errcap) != 0) return PSVC_EDIT_VALIDATION;
    return 0;
}

/* Still under the mcfg_ops lock (mcfg_ops.h: apply's scope is deliberate). The
 * credentials are persisted by now, so a failed re-apply is a warning. */
static void edit_apply(void *c_) {
    (void)c_;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; the change is stored and takes effect on reboot");
    time_svc_apply_mcfg();
}

psvc_rc_t psvc_mcfg_edit(psvc_mcfg_fn fn, void *ctx, uint32_t lock_ms, const char *what, char *err, size_t errcap) {
    char scratch[2];
    if (!err || errcap == 0) { err = scratch; errcap = sizeof scratch; }
    err[0] = '\0';
    if (!fn) return PSVC_E_INTERNAL;
    edit_ctx_t c = { fn, ctx, err, errcap };
    int rc = mcfg_ops_edit_ms(edit_tramp, &c, edit_apply, what ? what : "MCFG EDIT", lock_ms);
    switch (rc) {
    case 0:                       return PSVC_OK;
    case -1:                      err[0] = '\0'; return PSVC_E_BUSY;
    case -2:                      err[0] = '\0'; return PSVC_E_STORAGE;
    case -3:                      err[0] = '\0'; return PSVC_E_VALIDATION;
    case PSVC_EDIT_BAD_JSON:      return PSVC_E_BAD_JSON;
    case PSVC_EDIT_INVALID_FIELD: return PSVC_E_INVALID_FIELD;
    case PSVC_EDIT_VALIDATION:    return PSVC_E_VALIDATION;
    default:                      return PSVC_E_INTERNAL;
    }
}

int psvc_mcfg_json_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap) {
    int rc = hg_json_merge_mcfg(m, (const char *)ctx, err, errcap);
    if (rc == 0) return 0;
    return rc == -1 ? PSVC_EDIT_BAD_JSON : PSVC_EDIT_INVALID_FIELD;
}

int psvc_mcfg_fields_fn(hg_mcfg_t *m, void *ctx, char *err, size_t errcap) {
    const psvc_fedits_t *set = (const psvc_fedits_t *)ctx;
    if (!set || (set->n > 0 && !set->e)) return PSVC_EDIT_INVALID_FIELD;
    for (int i = 0; i < set->n; i++) {
        const psvc_fedit_t *e = &set->e[i];
        const hg_field_t *f = e->f;
        if (!f || f->group >= HG_MG_COUNT) {
            if (err && errcap) snprintf(err, errcap, "?");
            return PSVC_EDIT_INVALID_FIELD;
        }
        char text[PSVC_FEDIT_TEXT_MAX];
        memcpy(text, e->text, sizeof text);
        text[sizeof text - 1] = '\0';                            /* never trust the caller's terminator */
        int bad = 0;
        if (!(hg_mcfg_is_secret(f) && text[0] == '\0'))          /* blank secret = unchanged */
            bad = hg_field_write(f, m, text) != 0;
        psvc_wipe(text, sizeof text);                            /* the text may be a password: wiped on every path */
        if (bad) {
            if (err && errcap) snprintf(err, errcap, "%s.%s", HG_MGROUP_NAMES[f->group], f->key);
            return PSVC_EDIT_INVALID_FIELD;
        }
    }
    return 0;
}

void psvc_mcfg_get(hg_mcfg_t *out) { *out = *mcfg_get(); }
