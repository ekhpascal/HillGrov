#include <stdio.h>
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

/* 1 once the wall clock has been set by SET TIME or NTP this boot. NTP is a
 * direct query; the SET path has no flag of its own, so it is read back out of
 * the GET TIME line app_if_common already formats ("<date> <time> <SRC>
 * <age>", SRC = NONE until either source has spoken). */
static int wall_clock_ok(void) {
    if (time_svc_is_ntp()) return 1;
    char line[64], src[16] = "";
    if (hg_app_time_get_noted(line, sizeof line) != 0) return 0;
    if (sscanf(line, "%*s %*s %15s", src) != 1) return 0;
    return strcmp(src, "NONE") != 0;
}

static uint32_t auth_now(void) { return wall_clock_ok() ? (uint32_t)time(NULL) : AUTH_FROZEN; }

/* On the frozen base web_auth's own lockout deadline (lock_until_s = now_s +
 * WA_LOCK_S) can never elapse, because now_s never advances -- five
 * fat-fingered logins would lock the web UI out until the next reboot. The
 * deadline is therefore aged out here against monotonic uptime whenever the
 * frozen base is in use. Called with s_lock held. */
static uint32_t s_lock_up;   /* uptime_s when the frozen lockout was first seen; 0 = none */

static void frozen_lock_tick(uint32_t now) {
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
        if (s_wa.s[i].expires_s == 0xFFFFFFFFu) memset(&s_wa.s[i], 0, sizeof s_wa.s[i]);
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
