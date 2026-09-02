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
#include "nvs_flash.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "board.h"
#include "cli.h"

static const char *TAG = "app_if_master";

/* ---- id / uptime / status ---- */

static uint8_t master_zone_id(void) { return 0; }

static void master_get_mac(uint8_t mac[6]) {
    /* real header returns esp_err_t (app_if_t wants void); ignore the result -- a
     * failed efuse read leaves mac[] as whatever esp_efuse_mac_get_default set it to. */
    (void)esp_efuse_mac_get_default(mac);
}

static const char *master_node_name(void) { return "master"; }

static uint32_t master_uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* SP1 has no config model on master (arrives in SP3/SP4), so STATUS carries
 * only the fields app_if can answer without one -- no Cfg gen / Restart
 * pending lines. */
static int master_status_lines(char *resp, int len) {
    cmd_linef(resp, len, "  Uptime : %u s", (unsigned)master_uptime_s());
    cmd_linef(resp, len, "  Heap min : %u", (unsigned)esp_get_minimum_free_heap_size());
    cmd_linef(resp, len, "  Log drops : %u", (unsigned)cli_log_drops());
    return 0;
}

/* ---- log level ---- */

static const char *const LOG_NAMES[] = { "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE" };

static int master_log_set(const char *level, const char *tag, char *eff, size_t n) {
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

static int master_time_get(char *buf, size_t n) {
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

static int master_time_set(int y, int mo, int d, int h, int mi, int s) {
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 59)
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

static int master_fw_info(char *buf, size_t n) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    const char *state_str = (state == ESP_OTA_IMG_PENDING_VERIFY) ? "PENDING" : "VALID";
    snprintf(buf, n, "%s %s %s %s", esp_app_get_description()->version,
             running ? running->label : "?", state_str, "NONE");
    return 0;
}

static int master_fw_rollback(void) {
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* only reached on failure -- success reboots inside the call above */
    ESP_LOGE(TAG, "fw rollback failed: %s", esp_err_to_name(err));
    return -1;
}

/* SP1: master-initiated OTA (fetch + rescue handover) arrives in SP3/SP4.
 * The CMDF_ZONE gate on the FW UPDATE row answers ERR ZONE_ONLY before this
 * handler is ever reached on master, so this is a pure stub -- no handover
 * write, no reboot. */
static int master_fw_update(const char *ssid, const char *pass, const char *url) {
    (void)ssid; (void)pass; (void)url;
    return -1;
}

/* ---- save / factory reset ---- */

/* SP1: master holds no persisted config yet, so there is nothing to flush. */
static int master_save_flush(uint32_t timeout_ms) {
    (void)timeout_ms;
    return 0;
}

/* Master never calls nvs_flash_init() in SP1 (no config model to load), so
 * there is no open "hg" namespace handle to erase selectively. nvs_flash_erase()
 * is the simpler of the two ruling-offered options here: it de-inits the
 * default NVS partition first if it happens to be initialized, then erases
 * it outright -- no prior nvs_open() required. */
static int master_factory_reset(void) {
    esp_err_t err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset erase failed: %s", esp_err_to_name(err));
        return -1;
    }
    esp_restart();
    return -1; /* unreachable: esp_restart() never returns */
}

/* ---- table ---- */

const app_if_t APP_IF_MASTER = {
    .role_name     = HG_ROLE_NAME,
    .zone_id       = master_zone_id,
    .get_mac       = master_get_mac,
    .node_name     = master_node_name,
    .uptime_s      = master_uptime_s,
    .status_lines  = master_status_lines,
    .log_set       = master_log_set,
    .time_get      = master_time_get,
    .time_set      = master_time_set,
    .save_flush    = master_save_flush,
    .fw_info       = master_fw_info,
    .fw_rollback   = master_fw_rollback,
    .fw_update     = master_fw_update,
    .reboot        = esp_restart,
    .factory_reset = master_factory_reset,
};
