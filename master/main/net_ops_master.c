#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "psa/crypto.h"
#include "mcfg_store.h"
#include "web_auth.h"
#include "wifi_mgr.h"
#include "time_svc.h"
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
 * web_auth_set_password() needs the runtime wa_state_t for its sha/rand hooks.
 * Task 11's http_srv owns the real one (it also holds the live login
 * sessions); until it exists, this file-static stands in -- it is only ever
 * used for its two function pointers here, so the sessions it carries are
 * irrelevant. TASK 11: delete s_wa/s_wa_ready and take http_srv's shared
 * wa_state_t instead, so a CLI password change also invalidates web sessions. */

static wa_state_t s_wa;
static uint8_t    s_wa_ready;
static uint8_t    s_sha_failed;   /* set by sha256_fn; checked before we commit a hash */

static void sha256_fn(const uint8_t *in, size_t n, uint8_t out[32]) {
    size_t olen = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &olen);
    if (st != PSA_SUCCESS || olen != 32) {
        /* web_auth's signature has no way to report this, so the failure is
         * latched for master_web_set_password() to refuse the commit on.
         * Zeroing the buffer as well keeps the digest deterministic rather
         * than half-written, in case anything ever ignores the latch. */
        ESP_LOGE(TAG, "psa_hash_compute failed (%d)", (int)st);
        memset(out, 0, 32);
        s_sha_failed = 1;
    }
}

static void rand_fn(uint8_t *out, size_t n) { esp_fill_random(out, n); }

int master_web_set_password(const char *pw) {
    if (!lock_take()) return -2;
    int rc = -1;
    if (!s_wa_ready) {
        psa_status_t st = psa_crypto_init();
        if (st != PSA_SUCCESS) {
            /* No crypto provider at all: an internal fault, not a storage
             * problem -- nothing the operator can retry their way out of. */
            ESP_LOGE(TAG, "psa_crypto_init failed (%d)", (int)st);
            lock_give();
            return -3;
        }
        web_auth_init(&s_wa, sha256_fn, rand_fn);
        s_wa_ready = 1;
    }
    hg_mcfg_t m = *mcfg_get();
    s_sha_failed = 0;
    /* Length rule (8..63) and the salt+hash+clear-MCFG_F_WEB_DEFAULT work
     * all live in web_auth; -1 here is "too short/too long". */
    if (web_auth_set_password(&s_wa, &m, pw) != 0) {
        ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
    } else if (s_sha_failed) {
        /* Committing now would store a hash nothing can ever match and lock
         * the web UI out permanently -- refuse and leave the old one intact. */
        ESP_LOGE(TAG, "SET WEB PASSWORD: SHA-256 unavailable, password NOT changed");
        rc = -3;
    } else {
        rc = commit_and_log(&m, "SET WEB PASSWORD");
    }
    lock_give();
    return rc;
}

/* Task 9 replaces this with node_mgr_seed_mac(): until then every
 * SET NODE <z> MAC answers ERR ZONE_UNKNOWN rather than pretending to have
 * stored a binding. */
static int net_seed_mac(uint8_t zone, const uint8_t mac[6]) {
    (void)mac;
    ESP_LOGW(TAG, "SET NODE %u MAC: node_mgr_seed_mac arrives in Task 9", (unsigned)zone);
    return -1;
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
