#include <string.h>
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
 * serialized against other commits, so nothing here needs a lock of its own.
 *
 * mcfg_get() hands back a pointer into the live RAM buffer; every function
 * here copies it into a local the moment it is called and never touches that
 * pointer again after a commit, per mcfg_store.h's RAM contract.
 *
 * rc convention (master_cmds.h): 0 ok, -1 "the caller asked for something
 * invalid". mcfg_commit()'s -2 (NVS or mutex unavailable) also comes back as
 * -1, i.e. the CLI says ERR INVALID for what is really a storage fault; it is
 * logged distinctly here so the console tells the true story. Widening the ops
 * contract to carry -2 would need an ERR token the CLI grammar does not have. */

static int commit_and_log(hg_mcfg_t *m, const char *what) {
    int rc = mcfg_commit(m);
    if (rc == 0) return 0;
    if (rc == -2) ESP_LOGE(TAG, "%s: mcfg_commit failed to store (NVS/mutex)", what);
    else          ESP_LOGW(TAG, "%s: rejected by mcfg validation", what);
    return -1;
}

static void net_get_mcfg(hg_mcfg_t *out) { *out = *mcfg_get(); }

static int net_set_sta(const char *ssid, const char *pass) {
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.sta_ssid, sizeof m.sta_ssid, "%s", ssid);
    snprintf(m.sta_pass, sizeof m.sta_pass, "%s", pass);
    if (commit_and_log(&m, "SET WIFI STA") != 0) return -1;
    /* The credentials are already persisted at this point, so a failed
     * re-apply is a warning, not a rejection: the next boot joins anyway. */
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; STA change takes effect on reboot");
    return 0;
}

static int net_set_ap(const char *ssid, const char *pass) {
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.ap_ssid, sizeof m.ap_ssid, "%s", ssid);
    snprintf(m.ap_pass, sizeof m.ap_pass, "%s", pass);
    m.flags &= (uint8_t)~MCFG_F_AP_DEFAULT;   /* no longer the shipped HillGrow/hillgrow1 pair */
    if (commit_and_log(&m, "SET WIFI AP") != 0) return -1;
    if (wifi_mgr_apply() != 0) ESP_LOGW(TAG, "wifi_mgr_apply failed; AP change takes effect on reboot");
    return 0;
}

static int net_set_tz(const char *tz) {
    hg_mcfg_t m = *mcfg_get();
    snprintf(m.tz, sizeof m.tz, "%s", tz);
    if (commit_and_log(&m, "SET TZ") != 0) return -1;   /* bad POSIX TZ fails tz_check here */
    time_svc_apply_mcfg();
    return 0;
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

static void sha256_fn(const uint8_t *in, size_t n, uint8_t out[32]) {
    size_t olen = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &olen);
    if (st != PSA_SUCCESS || olen != 32) {
        /* Never silently hand web_auth a half-filled digest: a zeroed hash
         * would be a password nobody can ever match, which is the safe
         * direction, and the commit below still stores it consistently. */
        ESP_LOGE(TAG, "psa_hash_compute failed (%d)", (int)st);
        memset(out, 0, 32);
    }
}

static void rand_fn(uint8_t *out, size_t n) { esp_fill_random(out, n); }

int master_web_set_password(const char *pw) {
    if (!s_wa_ready) {
        psa_status_t st = psa_crypto_init();
        if (st != PSA_SUCCESS) {
            ESP_LOGE(TAG, "psa_crypto_init failed (%d)", (int)st);
            return -1;
        }
        web_auth_init(&s_wa, sha256_fn, rand_fn);
        s_wa_ready = 1;
    }
    hg_mcfg_t m = *mcfg_get();
    /* Length rule (8..63) and the salt+hash+clear-MCFG_F_WEB_DEFAULT work
     * all live in web_auth; -1 here is "too short/too long". */
    if (web_auth_set_password(&s_wa, &m, pw) != 0) {
        ESP_LOGW(TAG, "SET WEB PASSWORD: length must be 8..63");
        return -1;
    }
    return commit_and_log(&m, "SET WEB PASSWORD");
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

const net_ops_t *master_net_ops(void) { return &MASTER_NET_OPS; }
