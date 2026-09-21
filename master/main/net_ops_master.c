#include <stdint.h>
#include <stdio.h>
#include "esp_log.h"
#include "mcfg_ops.h"
#include "mcfg_store.h"
#include "http_srv.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "node_mgr.h"
#include "net_ops_master.h"

static const char *TAG = "net_ops";

/* Production net_ops_t: the one place that owns WIFI.*, WEB.* and TIME.TZ
 * for the CLI rows, on top of components/mcfg_ops, which now owns the
 * mcfg snapshot -> modify -> mcfg_commit() lock and sequence itself (Task 5:
 * moved out of this app file so components/http_srv, and the panel UI next,
 * take the SAME lock instead of reaching in here through extern
 * declarations). mcfg_commit() does the validating (including TZ, through
 * the tz_check time_svc installs) and the NVS write, serialized against
 * other commits; mcfg_ops_edit() serializes the read-modify-write itself.
 *
 * mcfg_get() hands back a pointer into the live RAM buffer; net_get_mcfg
 * below copies it into a local the moment it is called and never touches
 * that pointer again, per mcfg_store.h's RAM contract.
 *
 * rc convention (master_cmds.h): 0 ok, -1 "the caller asked for something
 * invalid", -2 "valid, but it could not be stored" (NVS write failure, a
 * commit-time validation reject, or the mcfg_ops lock timing out -- retry
 * or check the flash), -3 "this board is broken" (the SHA-256 provider is
 * unavailable, so no password can be hashed at all). The rows answer ERR
 * INVALID / ERR STORAGE / ERR INTERNAL. -2 is deliberately reserved for
 * storage: a missing hash function is not something retrying or reflashing
 * NVS will fix.
 *
 * mcfg_ops_edit() itself reserves -1 for "the lock could not be taken" and
 * -2 for "commit failed" (mcfg_ops.h), and does not distinguish a
 * commit-time validation reject (bad TZ, an out-of-range AP/STA field) from
 * a storage failure -- both are -2. Below, mcfg_ops_edit()'s own -1 (lock
 * unavailable) is remapped to this file's -2 so a busy lock still reads as
 * "could not be stored, retry" rather than "invalid", matching the rc
 * convention above exactly as it read before this task. The one thing that
 * changed: a commit-time validation reject used to surface here as -1; it
 * now surfaces as -2 like a storage failure, because only mcfg_ops_edit()'s
 * fn callback (which does not see mcfg_commit()'s own verdict) can tell the
 * two apart, and this file's callbacks below do not. */

static void net_get_mcfg(hg_mcfg_t *out) { *out = *mcfg_get(); }

static int set_sta_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->sta_ssid, sizeof m->sta_ssid, "%s", a[0]);
    snprintf(m->sta_pass, sizeof m->sta_pass, "%s", a[1]);
    return 0;
}

static int net_set_sta(const char *ssid, const char *pass) {
    const char *a[2] = { ssid, pass };
    int rc = mcfg_ops_edit(set_sta_fn, a);
    if (rc == -1) rc = -2;   /* mcfg_ops lock unavailable -> this file's "could not be stored" */
    /* The credentials are already persisted at this point, so a failed
     * re-apply is a warning, not a rejection: the next boot joins anyway. */
    if (rc == 0 && wifi_mgr_apply() != 0)
        ESP_LOGW(TAG, "wifi_mgr_apply failed; STA change takes effect on reboot");
    return rc;
}

static int set_ap_fn(hg_mcfg_t *m, void *ctx) {
    const char **a = (const char **)ctx;   /* [0]=ssid [1]=pass */
    snprintf(m->ap_ssid, sizeof m->ap_ssid, "%s", a[0]);
    snprintf(m->ap_pass, sizeof m->ap_pass, "%s", a[1]);
    m->flags &= (uint8_t)~MCFG_F_AP_DEFAULT;   /* no longer the shipped HillGrow/hillgrow1 pair */
    return 0;
}

static int net_set_ap(const char *ssid, const char *pass) {
    const char *a[2] = { ssid, pass };
    int rc = mcfg_ops_edit(set_ap_fn, a);
    if (rc == -1) rc = -2;
    if (rc == 0 && wifi_mgr_apply() != 0)
        ESP_LOGW(TAG, "wifi_mgr_apply failed; AP change takes effect on reboot");
    return rc;
}

static int set_tz_fn(hg_mcfg_t *m, void *ctx) {
    snprintf(m->tz, sizeof m->tz, "%s", (const char *)ctx);
    return 0;
}

static int net_set_tz(const char *tz) {
    int rc = mcfg_ops_edit(set_tz_fn, (void *)tz);   /* bad POSIX TZ fails tz_check inside mcfg_commit */
    if (rc == -1) rc = -2;
    if (rc == 0) time_svc_apply_mcfg();
    return rc;
}

/* ---- web password ----
 * The wa_state_t that hashes this password lives in http_srv (http_auth.c):
 * it is the same state that holds the live web login sessions, so a password
 * changed from the console invalidates the cookies issued against the old one
 * -- which is the whole reason for there being exactly one of it.
 *
 * Order matters: hash into a LOCAL mcfg copy, commit it, and only then drop
 * the sessions. Dropping them first would log every operator out even when
 * the commit went on to fail with the old password still in force. */

typedef struct { const char *pw; int hash_rc; } pw_edit_ctx_t;

static int set_password_fn(hg_mcfg_t *m, void *ctx_) {
    pw_edit_ctx_t *ctx = (pw_edit_ctx_t *)ctx_;
    /* http_auth_hash_password: 0 ok, -1 outside 8..63, -3 SHA-256
     * unavailable. -1 cannot travel back out through fn's own return value --
     * mcfg_ops.h reserves -1/-2 for its own lock/commit failures -- so a
     * non-zero hash_rc here always answers -9 (just "fn declines to
     * commit"); the real verdict comes back out through ctx->hash_rc
     * instead. */
    ctx->hash_rc = http_auth_hash_password(m, ctx->pw);
    return ctx->hash_rc == 0 ? 0 : -9;
}

int master_web_set_password(const char *pw) {
    pw_edit_ctx_t ctx = { .pw = pw, .hash_rc = 0 };
    int rc = mcfg_ops_edit(set_password_fn, &ctx);
    if (rc == -9) {
        rc = ctx.hash_rc;   /* -1 length, -3 broken board */
        if (rc == -1) ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
    } else if (rc == -1) {
        rc = -2;   /* mcfg_ops lock unavailable -> this file's "could not be stored" */
    } else if (rc == 0) {
        http_auth_sessions_drop();
    }
    return rc;
}

/* node_mgr owns the ztab, so the binding is written there (and to NVS) rather
 * than through the mcfg path the rest of this file uses -- no lock either,
 * since node_mgr_seed_mac serializes on nmgr_lock() internally. -1 is an
 * out-of-range zone, which master_cmds reports as ERR ZONE_UNKNOWN. */
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
