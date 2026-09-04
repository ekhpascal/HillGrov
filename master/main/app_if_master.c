#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "app_if_common.h"
#include "board.h"
#include "cli.h"
#include "node_mgr.h"

static const char *TAG = "app_if_master";

/* ---- id / uptime / status ---- */

static uint8_t master_zone_id(void) { return 0; }

static const char *master_node_name(void) { return "master"; }

/* SP1 has no config model on master (arrives in SP3/SP4), so STATUS carries
 * only the fields app_if can answer without one -- no Cfg gen / Restart
 * pending lines. */
static int master_status_lines(char *resp, int len) {
    cmd_linef(resp, len, "  Uptime : %u s", (unsigned)hg_app_uptime_s());
    cmd_linef(resp, len, "  Heap min : %u", (unsigned)esp_get_minimum_free_heap_size());
    cmd_linef(resp, len, "  Log drops : %u", (unsigned)cli_log_drops());
    return 0;
}

/* ruling #8: TIME_SYNC's time_valid must go true only after a real "SET
 * TIME" on this console -- hg_app_time_set() itself is shared with the
 * zone (app_if_common.c), so the one-line node_mgr hook lives in this
 * master-only wrapper instead. */
static int master_time_set(int y, int mo, int d, int h, int mi, int s) {
    int rc = hg_app_time_set(y, mo, d, h, mi, s);
    if (rc == 0) node_mgr_time_was_set();
    return rc;
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

/* Master has no config model of its own to selectively erase (ztab/OTA-trial
 * are the only "hg" keys it owns), so nvs_flash_erase() -- wiping the whole
 * default NVS partition outright -- stays the simpler option here even now
 * that app_main() calls nvs_flash_init() (SP3 ruling #1): no per-key
 * nvs_open()/erase bookkeeping needed, and it de-inits first if the
 * partition happens to be open. */
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
    .get_mac       = hg_app_get_mac,
    .node_name     = master_node_name,
    .uptime_s      = hg_app_uptime_s,
    .status_lines  = master_status_lines,
    .log_set       = hg_app_log_set,
    .time_get      = hg_app_time_get,
    .time_set      = master_time_set,
    .save_flush    = master_save_flush,
    .fw_info       = hg_app_fw_info,
    .fw_rollback   = hg_app_fw_rollback,
    .fw_update     = master_fw_update,
    .reboot        = esp_restart,
    .factory_reset = master_factory_reset,
};
