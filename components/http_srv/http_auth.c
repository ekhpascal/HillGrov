#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "psa/crypto.h"
#include "app_if_common.h"
#include "mcfg_store.h"
#include "time_svc.h"
#include "http_srv_internal.h"

static const char *TAG = "http_auth";

/* The ONE wa_state_t in the firmware: the live login sessions plus the
 * sha/rand hooks web_auth needs. Both the httpd task (login/logout/password/
 * cookie checks) and the CLI task (SET WEB PASSWORD, through
 * net_ops_master.c) reach it, so every access goes through s_lock and nothing
 * outside this file ever sees the struct -- the rest of http_srv uses the
 * locked facade in http_srv_internal.h.
 *
 * Sharing it is the point: a password changed from the console drops the web
 * sessions too, because there is only one set of them. */
static wa_state_t       s_wa;
static uint8_t          s_ready;
static uint8_t          s_sha_failed;   /* latched by sha256_fn, read under s_lock */
static SemaphoreHandle_t s_lock;

#define NVS_NS   "hg"
#define NVS_KEY  "sess"

/* ---- session time base (controller ruling) ----
 * web_auth expresses both the 30-day session TTL and the 60 s brute-force
 * lockout as deadlines on one caller-supplied clock. Uptime cannot carry a
 * 30-day expiry across a reboot, and the wall clock is meaningless until
 * something has actually set it, so:
 *
 *   time quality >= SET (a console SET TIME, or an NTP sync this boot)
 *       -> now_s = time(NULL). Real 30-day expiry, persisted across reboots.
 *   otherwise
 *       -> now_s = AUTH_FROZEN, a constant chosen so web_auth's
 *          now_s + WA_TTL_S lands exactly on 0xFFFFFFFF: the session lives
 *          until the next reboot and is deliberately NOT persisted (see
 *          sessions_save -- an immortal cookie in NVS would outlive every
 *          reboot, which is the opposite of the intent).
 *
 * Consequence, accepted: when the clock becomes valid mid-run the base jumps
 * from AUTH_FROZEN (~4.29e9) down to ~1.8e9, so a cookie minted before the
 * sync keeps working while cookies restored from NVS start working. The other
 * direction fails closed -- sessions restored from NVS look expired until a
 * clock arrives, and the operator logs in again. Never the reverse. */
#define AUTH_FROZEN  (0xFFFFFFFFu - WA_TTL_S)

/* 1 once the wall clock has been set this boot. Two cheap boolean reads, no
 * string parsing: time_svc_is_ntp() is the direct signal for an SNTP sync (the
 * source the ruling names), and hg_app_time_is_set() covers a console SET TIME
 * and any noted external source. */
static int wall_clock_ok(void) {
    return time_svc_is_ntp() || hg_app_time_is_set();
}

static uint32_t auth_now(void) { return wall_clock_ok() ? (uint32_t)time(NULL) : AUTH_FROZEN; }

/* web_auth's lockout is a deadline on the same caller-supplied clock as
 * session expiry, which this file switches between two bases -- so the
 * deadline needs looking after on both of them. Called with s_lock held.
 *
 * (a) A deadline minted on the FROZEN base survives the switch to the wall
 *     clock: AUTH_FROZEN + 60 is ~4.29e9, every wall-clock now_s is ~1.8e9,
 *     so "now < lock_until" would stay true forever and EVERY login -- the
 *     correct one included -- would answer 429 until the next reboot. That is
 *     an unauthenticated denial of service reachable from the house LAN by
 *     anyone willing to guess five times before the master gets its clock.
 *     Fix: a legitimate deadline is never more than WA_LOCK_S ahead of now on
 *     the base that produced it, so a deadline further out than that proves
 *     the base changed underneath it, and it is dropped.
 * (b) On the frozen base the deadline can never elapse on its own, because
 *     now_s never advances -- five fat-fingered logins would lock the web UI
 *     out until the next reboot. It is aged out against monotonic uptime
 *     instead.
 * Both bases are far from UINT32_MAX at the point of the (a) comparison
 * (AUTH_FROZEN + WA_LOCK_S is still below it), so the addition cannot wrap. */
static uint32_t s_lock_up;   /* uptime_s when the frozen lockout was first seen; 0 = none */

static void frozen_lock_tick(uint32_t now) {
    if (s_wa.lock_until_s > now + WA_LOCK_S) {   /* (a) the clock base changed under it */
        ESP_LOGW(TAG, "dropping a lockout deadline minted on the other clock base");
        s_wa.fails = 0;
        s_wa.lock_until_s = 0;
        s_lock_up = 0;
        return;
    }
    if (now != AUTH_FROZEN || s_wa.lock_until_s == 0) { s_lock_up = 0; return; }
    uint32_t up = hg_app_uptime_s();
    if (s_lock_up == 0) { s_lock_up = up ? up : 1; return; }
    if (up - s_lock_up >= WA_LOCK_S) {
        s_wa.fails = 0;
        s_wa.lock_until_s = 0;
        s_lock_up = 0;
    }
}

/* ---- crypto hooks ---- */

static void sha256_fn(const uint8_t *in, size_t n, uint8_t out[32]) {
    /* mbedtls 4.x dropped the public mbedtls_sha256() one-shot, so this goes
     * through PSA. web_auth's hook signature cannot report a failure, so one
     * is latched for http_auth_hash_password() to refuse the commit on --
     * committing an all-zero digest would lock the web UI out for good. */
    size_t olen = 0;
    psa_status_t st = psa_hash_compute(PSA_ALG_SHA_256, in, n, out, 32, &olen);
    if (st != PSA_SUCCESS || olen != 32) {
        ESP_LOGE(TAG, "psa_hash_compute failed (%d)", (int)st);
        memset(out, 0, 32);
        s_sha_failed = 1;
    }
}

static void rand_fn(uint8_t *out, size_t n) { esp_fill_random(out, n); }

/* ---- session persistence (NVS "hg"/"sess", 80 B) ---- */

static void sessions_load(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t buf[128];
    size_t len = sizeof buf;
    esp_err_t rc = nvs_get_blob(h, NVS_KEY, buf, &len);
    nvs_close(h);
    if (rc != ESP_OK) return;
    if (web_auth_unpack(&s_wa, buf, len) != 0) {
        ESP_LOGW(TAG, "stored sessions too short (%u B) -- ignored", (unsigned)len);
        return;
    }
    int live = 0;
    for (int i = 0; i < WA_SESSIONS; i++) {
        /* An "until reboot" session must never come back from flash; the save
         * path drops them, this is the belt for an older/corrupt blob. */
        int drop = (s_wa.s[i].expires_s == 0xFFFFFFFFu);
        /* An all-zero token with a non-zero expiry is not a session anybody can
         * hold -- web_auth_unpack marks a slot used purely on its expiry, so a
         * blob from an erased/partially-written page could otherwise present a
         * live slot whose token is 16 zero bytes, which is exactly the token a
         * forged "hg_sess=000...0" cookie carries. */
        uint8_t any = 0;
        for (int b = 0; b < WA_TOKEN_LEN; b++) any |= s_wa.s[i].token[b];
        if (!any) drop = 1;
        if (drop) memset(&s_wa.s[i], 0, sizeof s_wa.s[i]);
        if (s_wa.s[i].used) live++;
    }
    ESP_LOGI(TAG, "%d stored session(s) restored", live);
}

/* Called with s_lock held, after every login/logout/password change (rare). */
static void sessions_save(void) {
    wa_state_t tmp = s_wa;
    for (int i = 0; i < WA_SESSIONS; i++)
        if (tmp.s[i].expires_s == 0xFFFFFFFFu) memset(&tmp.s[i], 0, sizeof tmp.s[i]);

    uint8_t buf[128];
    int n = web_auth_pack(&tmp, buf, sizeof buf);
    if (n < 0) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed -- sessions not persisted");
        return;
    }
    if (nvs_set_blob(h, NVS_KEY, buf, (size_t)n) == ESP_OK) nvs_commit(h);
    else ESP_LOGW(TAG, "nvs_set_blob failed -- sessions not persisted");
    nvs_close(h);
}

/* ---- init + locking ---- */

int http_auth_init(void) {
    if (s_ready) return 0;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) {
            ESP_LOGE(TAG, "auth mutex unavailable");
            return -1;
        }
    }
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed (%d) -- no password can be hashed", (int)st);
        return -1;
    }
    web_auth_init(&s_wa, sha256_fn, rand_fn);
    sessions_load();
    s_ready = 1;
    return 0;
}

/* 3000 ms: every critical section in this file is memory-only work plus at
 * most one NVS write (~50 ms), so waiting this long means something is badly
 * wrong and answering 500 beats blocking the one httpd task forever. */
static int lock_take(void) {
    if (!s_ready || !s_lock) return 0;
    return xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) == pdTRUE;
}

static void lock_give(void) { xSemaphoreGive(s_lock); }

/* ---- locked facade ---- */

int http_auth_check(const char *cookie_hdr) {
    if (!lock_take()) return -1;
    int rc = web_auth_check(&s_wa, cookie_hdr, auth_now());
    lock_give();
    return rc;
}

int http_auth_login(const char *pw, char cookie_out[2 * WA_TOKEN_LEN + 1]) {
    if (!lock_take()) return -1;
    uint32_t now = auth_now();
    frozen_lock_tick(now);
    int rc = web_auth_login(&s_wa, mcfg_get(), pw, now, cookie_out);
    if (rc == 0) sessions_save();
    lock_give();
    return rc;
}

int http_auth_verify_password(const char *pw) {
    if (!lock_take()) return -1;
    int rc = web_auth_verify(&s_wa, mcfg_get(), pw);
    lock_give();
    return rc;
}

void http_auth_logout_cookie(const char *cookie_hdr) {
    if (!lock_take()) return;
    web_auth_logout(&s_wa, cookie_hdr);
    sessions_save();
    lock_give();
}

int http_auth_hash_password(hg_mcfg_t *m, const char *pw) {
    if (!lock_take()) return -3;   /* no usable auth state == the board's crypto is broken */
    s_sha_failed = 0;
    int rc;
    if (web_auth_set_password(&s_wa, m, pw) != 0) {
        rc = -1;                   /* outside 8..63 */
    } else if (s_sha_failed) {
        rc = -3;                   /* *m now holds an all-zero digest: the caller must NOT commit */
        ESP_LOGE(TAG, "SHA-256 unavailable -- password NOT changed");
    } else {
        rc = 0;
    }
    lock_give();
    return rc;
}

void http_auth_sessions_drop(void) {
    if (!lock_take()) return;
    for (int i = 0; i < WA_SESSIONS; i++) memset(&s_wa.s[i], 0, sizeof s_wa.s[i]);
    s_wa.fails = 0;
    s_wa.lock_until_s = 0;
    s_lock_up = 0;
    sessions_save();
    lock_give();
    ESP_LOGI(TAG, "all web sessions invalidated (password changed)");
}
