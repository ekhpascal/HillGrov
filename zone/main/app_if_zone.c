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
#include "bootloader_common.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "hg_model.h"
#include "hg_store.h"
#include "rescue_handover.h"
#include "board.h"
#include "cli.h"

static const char *TAG = "app_if_zone";

/* Reboots into the rescue image; the bootloader side of this handshake lands
 * in Task 15, which is expected to declare and consume this prototype from a
 * shared header once it exists. */
void hg_reboot_to_rescue(void);

/* ---- id / uptime / status ---- */

static void zone_get_mac(uint8_t mac[6]) {
    /* real header returns esp_err_t (app_if_t wants void); ignore the result -- a
     * failed efuse read leaves mac[] as whatever esp_efuse_mac_get_default set it to. */
    (void)esp_efuse_mac_get_default(mac);
}

static const char *zone_node_name(void) {
    static hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    return cfg.name[0] ? cfg.name : "-";
}

static uint32_t zone_uptime_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static int zone_status_lines(char *resp, int len) {
    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    cmd_linef(resp, len, "  Uptime : %u s", (unsigned)zone_uptime_s());
    cmd_linef(resp, len, "  Heap min : %u", (unsigned)esp_get_minimum_free_heap_size());
    cmd_linef(resp, len, "  Log drops : %u", (unsigned)cli_log_drops());
    cmd_linef(resp, len, "  Cfg gen : %u", (unsigned)cfg.generation);
    cmd_linef(resp, len, "  Restart pending : %d", hg_model_restart_pending());
    return 0;
}

/* ---- log level ---- */

static const char *const LOG_NAMES[] = { "NONE", "ERROR", "WARN", "INFO", "DEBUG", "VERBOSE" };

static int zone_log_set(const char *level, const char *tag, char *eff, size_t n) {
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

static int zone_time_get(char *buf, size_t n) {
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

static int zone_time_set(int y, int mo, int d, int h, int mi, int s) {
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

static int zone_fw_info(char *buf, size_t n) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running) esp_ota_get_state_partition(running, &state);
    const char *state_str = (state == ESP_OTA_IMG_PENDING_VERIFY) ? "PENDING" : "VALID";
    snprintf(buf, n, "%s %s %s %s", esp_app_get_description()->version,
             running ? running->label : "?", state_str, "NONE");
    return 0;
}

/* spec 4.3: flush before every restart. factory_reset (hg_store_factory_reset)
 * is unaffected -- it erases the "hg" NVS namespace outright, so there is
 * nothing worth flushing first. */
static void zone_reboot(void) {
    hg_store_flush(2000);
    esp_restart();
}

static int zone_fw_rollback(void) {
    esp_err_t err = esp_ota_mark_app_invalid_rollback_and_reboot();
    /* only reached on failure -- success reboots inside the call above */
    ESP_LOGE(TAG, "fw rollback failed: %s", esp_err_to_name(err));
    return -1;
}

static int zone_fw_update(const char *ssid, const char *pass, const char *url) {
    /* spec 4.3: flush before every restart -- this call reboots into rescue
     * on success, so any dirty config must hit NVS before the handover write. */
    hg_store_flush(2000);
    hg_handover_t h;
    memset(&h, 0, sizeof h);
    /* Belt-and-braces: cmd_common's A_FWUP arg maxes already reject an
     * oversized field at dispatch, but never silently truncate here too --
     * a truncated URL would reboot the node into rescue with an unfetchable
     * address. */
    if (strlen(ssid) >= sizeof h.ssid || strlen(pass) >= sizeof h.pass || strlen(url) >= sizeof h.url)
        return -1;
    h.expect_link = 0;
    memcpy(h.ssid, ssid, strlen(ssid) + 1);
    memcpy(h.pass, pass, strlen(pass) + 1);
    memcpy(h.url,  url,  strlen(url)  + 1);
    if (hg_handover_write(&h) != 0) return -1;
    hg_reboot_to_rescue();
    return -1; /* unreachable: hg_reboot_to_rescue() reboots */
}

/* ---- table ---- */

const app_if_t APP_IF_ZONE = {
    .role_name     = HG_ROLE_NAME,
    .zone_id       = hg_store_zid,
    .get_mac       = zone_get_mac,
    .node_name     = zone_node_name,
    .uptime_s      = zone_uptime_s,
    .status_lines  = zone_status_lines,
    .log_set       = zone_log_set,
    .time_get      = zone_time_get,
    .time_set      = zone_time_set,
    .save_flush    = hg_store_flush,
    .fw_info       = zone_fw_info,
    .fw_rollback   = zone_fw_rollback,
    .fw_update     = zone_fw_update,
    .reboot        = zone_reboot,
    .factory_reset = hg_store_factory_reset,
};

/* SP1 placeholder rescue-reboot handshake: stash a magic word in the RTC
 * retain-memory scratch area and restart; Task 15's bootloader reads it back
 * to decide whether to boot the rescue image. custom[] is a byte array, so
 * the magic is written as a full 32-bit little-endian word at custom[0..3]
 * rather than truncated into a single byte. */
void hg_reboot_to_rescue(void) {
    rtc_retain_mem_t *rtc = bootloader_common_get_rtc_retain_mem();
    uint32_t magic = 0xB0FAAF0Bu;
    memcpy(rtc->custom, &magic, sizeof magic);   /* avoid strict-aliasing on the uint8_t[] buffer */
    bootloader_common_update_rtc_retain_mem(NULL, false);
    esp_restart();
}
