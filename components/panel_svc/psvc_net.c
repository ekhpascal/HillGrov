#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "http_auth.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "node_mgr.h"
#include "psvc_net.h"

static const char *TAG = "psvc_net";

/* The one place that owns WIFI.*, WEB.* and TIME.TZ writes for every face, on
 * top of components/mcfg_ops (snapshot -> modify -> mcfg_commit() -> apply,
 * atomic under one lock). mcfg_commit() does the validating (including TZ,
 * through the tz_check time_svc installs) and the NVS write.
 *
 * mcfg_ops_edit_ms() reserves three negative codes (mcfg_ops.h): -1 the lock
 * was not taken within lock_ms, -2 mcfg_commit() failed on storage, -3
 * mcfg_commit() rejected the config. from_ops() turns them into the shared
 * vocabulary; the legacy net_ops_t members then go through
 * psvc_rc_to_net_legacy(), which reproduces this file's pre-panel_svc
 * convention exactly: BUSY and STORAGE -> -2, INVALID -> -1, INTERNAL -> -3.
 *
 * Every apply below runs as mcfg_ops_edit_ms()'s `apply` callback, i.e. still
 * holding the lock: GET /api/wifi/scan holds the same lock across its blocking
 * radio scan specifically so a STA/AP reconfigure cannot land mid-scan. */

static psvc_rc_t from_ops(int rc) {
    switch (rc) {
    case 0:  return PSVC_OK;
    case -1: return PSVC_E_BUSY;      /* the mcfg_ops lock was not acquired within lock_ms */
    case -2: return PSVC_E_STORAGE;   /* mcfg_commit() failed on NVS or its own mutex */
    case -3: return PSVC_E_INVALID;   /* mcfg_commit() rejected the config (bad TZ, SSID or pass) */
    default: return PSVC_E_INTERNAL;
    }
}

/* mcfg_get() hands back a pointer into the live RAM buffer; copy it at once
 * and never touch that pointer again (mcfg_store.h's RAM contract). */
static void net_get_mcfg(hg_mcfg_t *out) { *out = *mcfg_get(); }

/* ---- STA ---- */

static int set_sta_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->sta_ssid, sizeof m->sta_ssid, "%s", a[0]);
    snprintf(m->sta_pass, sizeof m->sta_pass, "%s", a[1]);
    return 0;
}

/* The credentials are already persisted here, so a failed re-apply is a
 * warning, not a rejection: the next boot joins anyway. */
static void set_sta_apply(void *ctx) {
    (void)ctx;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; STA change takes effect on reboot");
}

psvc_rc_t psvc_wifi_set_sta(const char *ssid, const char *pass, uint32_t lock_ms) {
    if (!ssid || !pass) return PSVC_E_INVALID;
    const char *a[2] = { ssid, pass };
    return from_ops(mcfg_ops_edit_ms(set_sta_fn, a, set_sta_apply, "SET WIFI STA", lock_ms));
}

static int net_set_sta(const char *ssid, const char *pass) {
    return psvc_rc_to_net_legacy(psvc_wifi_set_sta(ssid, pass, PSVC_LOCK_LEGACY_MS));
}

/* ---- AP ---- */

static int set_ap_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->ap_ssid, sizeof m->ap_ssid, "%s", a[0]);
    snprintf(m->ap_pass, sizeof m->ap_pass, "%s", a[1]);
    m->flags &= (uint8_t)~MCFG_F_AP_DEFAULT;   /* no longer the shipped HillGrow/hillgrow1 pair */
    return 0;
}

static void set_ap_apply(void *ctx) {
    (void)ctx;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; AP change takes effect on reboot");
}

psvc_rc_t psvc_wifi_set_ap(const char *ssid, const char *pass, uint32_t lock_ms) {
    if (!ssid || !pass) return PSVC_E_INVALID;
    const char *a[2] = { ssid, pass };
    return from_ops(mcfg_ops_edit_ms(set_ap_fn, a, set_ap_apply, "SET WIFI AP", lock_ms));
}

static int net_set_ap(const char *ssid, const char *pass) {
    return psvc_rc_to_net_legacy(psvc_wifi_set_ap(ssid, pass, PSVC_LOCK_LEGACY_MS));
}

/* ---- TZ ---- */

static int set_tz_fn(hg_mcfg_t *m, void *ctx) {
    snprintf(m->tz, sizeof m->tz, "%s", (const char *)ctx);
    return 0;
}

static void set_tz_apply(void *ctx) {
    (void)ctx;
    time_svc_apply_mcfg();
}

psvc_rc_t psvc_tz_set(const char *tz, uint32_t lock_ms) {
    if (!tz) return PSVC_E_INVALID;
    /* a bad POSIX TZ fails tz_check inside mcfg_commit -> -3 -> INVALID */
    return from_ops(mcfg_ops_edit_ms(set_tz_fn, (void *)tz, set_tz_apply, "SET TZ", lock_ms));
}

static int net_set_tz(const char *tz) {
    return psvc_rc_to_net_legacy(psvc_tz_set(tz, PSVC_LOCK_LEGACY_MS));
}

/* ---- web password ----
 * The wa_state_t that hashes this password lives in components/http_auth: it
 * is the same state that holds the live web login sessions, so a password
 * changed from the console or the panel invalidates the cookies issued against
 * the old one. Order matters: hash into the PRIVATE copy, commit it, and only
 * then drop the sessions (the apply below) -- dropping first would log every
 * operator out even when the commit went on to fail. */

typedef struct { const char *pw; int hash_rc; } pw_edit_ctx_t;

static int set_password_fn(hg_mcfg_t *m, void *ctx_) {
    pw_edit_ctx_t *ctx = (pw_edit_ctx_t *)ctx_;
    /* http_auth_hash_password: 0 ok, -1 outside 8..63, -3 SHA-256 unavailable.
     * Both failures are negative and mcfg_ops.h reserves every negative for
     * itself, so a failure declines with 1 and the verdict travels in ctx. */
    ctx->hash_rc = http_auth_hash_password(m, ctx->pw);
    return ctx->hash_rc == 0 ? 0 : 1;
}

static void set_password_apply(void *ctx) {
    (void)ctx;
    http_auth_sessions_drop();
}

psvc_rc_t psvc_web_password_set(const char *pw, uint32_t lock_ms) {
    if (!pw) return PSVC_E_INVALID;
    pw_edit_ctx_t ctx = { .pw = pw, .hash_rc = 0 };
    int rc = mcfg_ops_edit_ms(set_password_fn, &ctx, set_password_apply, "SET WEB PASSWORD", lock_ms);
    if (rc == 1) {
        if (ctx.hash_rc == -1) {
            ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
            return PSVC_E_INVALID;
        }
        return PSVC_E_INTERNAL;   /* -3: this board cannot hash; nothing was committed */
    }
    return from_ops(rc);
}

int master_web_set_password(const char *pw) {
    return psvc_rc_to_net_legacy(psvc_web_password_set(pw, PSVC_LOCK_LEGACY_MS));
}

/* ---- Wi-Fi scan ---- */

psvc_rc_t psvc_wifi_scan(wifi_scan_t *out, int cap, int *n, uint32_t lock_ms) {
    if (n) *n = 0;
    if (!out || cap <= 0 || !n) return PSVC_E_INTERNAL;
    if (mcfg_ops_lock(lock_ms) != 0) return PSVC_E_BUSY;
    int got = wifi_mgr_scan(out, cap);
    mcfg_ops_unlock();
    if (got < 0) return PSVC_E_INTERNAL;
    *n = got;
    return PSVC_OK;
}

/* ---- node binding ----
 * node_mgr owns the ztab, so the binding is written there (and to NVS) rather
 * than through the mcfg path -- no mcfg lock, since node_mgr_seed_mac
 * serializes on nmgr_lock() internally. -1 is an out-of-range zone, which
 * master_cmds reports as ERR ZONE_UNKNOWN. */
static int net_seed_mac(uint8_t zone, const uint8_t mac[6]) {
    return node_mgr_seed_mac(zone, mac);
}

static const net_ops_t MASTER_NET_OPS = {
    .get_mcfg         = net_get_mcfg,
    .set_sta          = net_set_sta,
    .set_ap           = net_set_ap,
    .set_web_password = master_web_set_password,
    .set_tz           = net_set_tz,
    .wifi_status      = wifi_mgr_status,
    .seed_mac         = net_seed_mac,
};

const net_ops_t *master_net_ops(void) {
    return &MASTER_NET_OPS;
}
