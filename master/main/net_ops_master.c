#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "mcfg_store.h"
#include "http_srv.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "node_mgr.h"
#include "net_ops_master.h"

static const char *TAG = "net_ops";

/* Production net_ops_t: the one place that owns the
 *   mcfg snapshot -> modify one group -> mcfg_commit() -> apply
 * sequence for WIFI.*, WEB.* and TIME.TZ. mcfg_commit() does the validating
 * (including TZ, through the tz_check time_svc installs) and the NVS write,
 * serialized against other commits.
 *
 * mcfg_get() hands back a pointer into the live RAM buffer; every function
 * here copies it into a local the moment it is called and never touches that
 * pointer again after a commit, per mcfg_store.h's RAM contract.
 *
 * rc convention (master_cmds.h): 0 ok, -1 "the caller asked for something
 * invalid", -2 "valid, but it could not be stored" (NVS write failure or a
 * commit/ops mutex timeout -- retry or check the flash), -3 "this board is
 * broken" (the SHA-256 provider is unavailable, so no password can be hashed
 * at all). The rows answer ERR INVALID / ERR STORAGE / ERR INTERNAL. -2 is
 * deliberately reserved for storage: a missing hash function is not something
 * retrying or reflashing NVS will fix. */

/* mcfg_commit() serializes commits against each other, but NOT the
 * read-modify-write around them: two ops running concurrently (Task 12 drives
 * these same ops from httpd workers while the CLI can be mid-command) would
 * both snapshot the same mcfg and the second commit would silently drop the
 * first one's field. This mutex makes each snapshot->modify->commit atomic.
 * Created in master_net_ops(), which master_table() calls once from app_main
 * before any task can reach a row -- so no lazy-init race. */
static SemaphoreHandle_t s_lock;

static int lock_take(void) {
    if (!s_lock) return 1;   /* pre-init, i.e. still single-threaded boot */
    /* Longer than mcfg_commit()'s own 5000 ms mutex timeout, so a caller that
     * loses this race reports the commit's verdict rather than ours. */
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(6000)) == pdTRUE;
}

static void lock_give(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* 0 ok / -1 invalid / -2 could not be stored. */
static int commit_and_log(hg_mcfg_t *m, const char *what) {
    int rc = mcfg_commit(m);
    if (rc == 0) return 0;
    if (rc == -2) {
        ESP_LOGE(TAG, "%s: mcfg_commit failed to store (NVS/mutex)", what);
        return -2;
    }
    ESP_LOGW(TAG, "%s: rejected by mcfg validation", what);
    return -1;
}

static void net_get_mcfg(hg_mcfg_t *out) { *out = *mcfg_get(); }

static int net_set_sta(const char *ssid, const char *pass) {
    if (!lock_take()) return -2;
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.sta_ssid, sizeof m.sta_ssid, "%s", ssid);
    snprintf(m.sta_pass, sizeof m.sta_pass, "%s", pass);
    int rc = commit_and_log(&m, "SET WIFI STA");
    /* The credentials are already persisted at this point, so a failed
     * re-apply is a warning, not a rejection: the next boot joins anyway. */
    if (rc == 0 && wifi_mgr_apply() != 0)
        ESP_LOGW(TAG, "wifi_mgr_apply failed; STA change takes effect on reboot");
    lock_give();
    return rc;
}

static int net_set_ap(const char *ssid, const char *pass) {
    if (!lock_take()) return -2;
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.ap_ssid, sizeof m.ap_ssid, "%s", ssid);
    snprintf(m.ap_pass, sizeof m.ap_pass, "%s", pass);
    m.flags &= (uint8_t)~MCFG_F_AP_DEFAULT;   /* no longer the shipped HillGrow/hillgrow1 pair */
    int rc = commit_and_log(&m, "SET WIFI AP");
    if (rc == 0 && wifi_mgr_apply() != 0)
        ESP_LOGW(TAG, "wifi_mgr_apply failed; AP change takes effect on reboot");
    lock_give();
    return rc;
}

static int net_set_tz(const char *tz) {
    if (!lock_take()) return -2;
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.tz, sizeof m.tz, "%s", tz);
    int rc = commit_and_log(&m, "SET TZ");   /* bad POSIX TZ fails tz_check here */
    if (rc == 0) time_svc_apply_mcfg();
    lock_give();
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

int master_web_set_password(const char *pw) {
    if (!lock_take()) return -2;
    hg_mcfg_t m = *mcfg_get();
    /* 0 ok, -1 outside 8..63, -3 SHA-256 unavailable (a broken board: the
     * all-zero digest a failed hash would commit can never be matched again,
     * so nothing is written). */
    int rc = http_auth_hash_password(&m, pw);
    if (rc == -1) {
        ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
    } else if (rc == 0) {
        rc = commit_and_log(&m, "SET WEB PASSWORD");
        if (rc == 0) http_auth_sessions_drop();
    }
    lock_give();
    return rc;
}

/* node_mgr owns the ztab, so the binding is written there (and to NVS) rather
 * than through the mcfg path the rest of this file uses -- no s_lock either,
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
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) ESP_LOGE(TAG, "net ops mutex unavailable -- config writes are unserialized");
    }
    return &MASTER_NET_OPS;
}
