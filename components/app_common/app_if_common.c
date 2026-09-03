#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"    /* declared by the esp_app_format component */
#include "esp_mac.h"
#include "cmd_core.h"        /* cmd_ci_eq() used by hg_app_log_set */
#include "app_if_common.h"

static const char *TAG = "app_common";

/* ---- id / uptime ---- */

void hg_app_get_mac(uint8_t mac[6]) {
    /* real header returns esp_err_t (app_if_t wants void); ignore the result -- a
     * failed efuse read leaves mac[] as whatever esp_efuse_mac_get_default set it to. */
    (void)esp_efuse_mac_get_default(mac);
}

uint32_t hg_app_uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* ---- log level ---- */

static const char *const LOG_NAMES[] = { "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE" };

int hg_app_log_set(const char *level, const char *tag, char *eff, size_t n) {
    const char *t = tag ? tag : "*";
    if (level) {
        esp_log_level_t lvl = ESP_LOG_NONE;
        for (int i = 0; i < (int)(sizeof LOG_NAMES / sizeof LOG_NAMES[0]); i++)
            if (cmd_ci_eq(LOG_NAMES[i], level)) { lvl = (esp_log_level_t)i; break; }
        esp_log_level_set(t, lvl);
    }
    esp_log_level_t cur = esp_log_level_get(t);
    if (cur < ESP_LOG_NONE || cur > ESP_LOG_VERBOSE) cur = ESP_LOG_NONE;
    snprintf(eff, n, "%s", LOG_NAMES[cur]);
    return 0;
}

/* ---- time: SP1 has no RTC/NTP/RING source, so the reported source is
 * always NONE (never set this boot) or SET (set once, age tracked locally) ---- */

static uint8_t s_time_is_set;
static time_t  s_time_set_at;

int hg_app_time_get(char *buf, size_t n) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    uint32_t age = s_time_is_set ? (uint32_t)(t - s_time_set_at) : 0;
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d %s %u",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             s_time_is_set ? "SET" : "NONE", (unsigned)age);
    return 0;
}

int hg_app_time_set(int y, int mo, int d, int h, int mi, int s) {
    /* days-in-month, index 0 = January; Feb bumped to 29 below on a leap year */
    static const uint8_t days_in_month[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 2020 || y > 2099 || mo < 1 || mo > 12) return -1;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    int dmax = days_in_month[mo - 1] + ((mo == 2 && leap) ? 1 : 0);
    if (d < 1 || d > dmax || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
        return -1;
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900; tmv.tm_mon = mo - 1; tmv.tm_mday = d;
    tmv.tm_hour = h; tmv.tm_min = mi; tmv.tm_sec = s;
    time_t t = mktime(&tmv);
    if (t == (time_t)-1) return -1;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return -1;
    s_time_is_set = 1;
    s_time_set_at = t;
    return 0;
}

/* ---- firmware ---- */

int hg_app_fw_info(char *buf, size_t n) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    const char *state_str = (state == ESP_OTA_IMG_PENDING_VERIFY) ? "PENDING" : "VALID";
    snprintf(buf, n, "%s %s %s %s", esp_app_get_description()->version,
             running ? running->label : "?", state_str, "NONE");
    return 0;
}

int hg_app_fw_rollback(void) {
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* only reached on failure -- success reboots inside the call above */
    ESP_LOGE(TAG, "fw rollback failed: %s", esp_err_to_name(err));
    return -1;
}
