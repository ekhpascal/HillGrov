#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "ota_trial.h"
#include "ring_link.h"
#include "node_mgr.h"
#include "master_cmds.h"

static const char *TAG = "cmd_table_master";
static cmd_entry_t s_table[64];
static int         s_n;
static int         s_init;

/* Task 15's fleet sequencer hasn't landed -- these three stay NULL-safe -1
 * stubs (ruling #5); master_cmds.c's fw rows map any non-zero rc to
 * ERR NOT_IMPLEMENTED until then. */
static int stub_fw_zone(uint8_t zone)  { (void)zone; return -1; }
static int stub_fw_all(void)           { return -1; }
static int stub_fw_abort(void)         { return -1; }
static int stub_fw_status(char *buf, size_t n) { snprintf(buf, n, "NOT_IMPLEMENTED"); return 0; }

/* node_mgr's public API was shaped to match node_ops_t 1:1 (master_cmds.h),
 * so every member but the fw_* stubs above wires straight through. */
static const node_ops_t MASTER_NODE_OPS = {
    .node_count      = node_mgr_node_count,
    .get             = node_mgr_get,
    .ring_status     = node_mgr_ring_status,
    .set_name        = node_mgr_set_name,
    .clear           = node_mgr_clear,
    .unassigned      = node_mgr_unassigned,
    .trace           = ring_link_trace,
    .time_valid      = node_mgr_time_valid,
    .cfg_sync_failed = node_mgr_cfg_sync_failed,
    .fw_zone         = stub_fw_zone,
    .fw_all          = stub_fw_all,
    .fw_abort        = stub_fw_abort,
    .fw_status       = stub_fw_status,
};

/* Master picked up its first non-common rows in SP3 (OTA_TRIAL_ROWS, then
 * MASTER_CMD_ROWS in Task 14), so it now merges the same way zone_table()
 * does rather than pointing straight at CMD_COMMON_ROWS. */
const cmd_entry_t *master_table(int *n) {
    if (!s_init) {
        master_cmds_init(&MASTER_NODE_OPS);
        int total = CMD_COMMON_ROWS_N + MASTER_CMD_ROWS_N + OTA_TRIAL_ROWS_N;
        if (total > 64) {
            ESP_LOGE(TAG, "table overflow: %d rows > 64 capacity, clamping", total);
            total = 64;
        }
        int n_common = CMD_COMMON_ROWS_N < total ? CMD_COMMON_ROWS_N : total;
        int remaining = total - n_common;
        int n_master = MASTER_CMD_ROWS_N < remaining ? MASTER_CMD_ROWS_N : remaining;
        int n_trial = remaining - n_master;
        memcpy(s_table, CMD_COMMON_ROWS, (size_t)n_common * sizeof(cmd_entry_t));
        memcpy(s_table + n_common, MASTER_CMD_ROWS, (size_t)n_master * sizeof(cmd_entry_t));
        memcpy(s_table + n_common + n_master, OTA_TRIAL_ROWS, (size_t)n_trial * sizeof(cmd_entry_t));
        s_n = total;
        s_init = 1;
    }
    if (n) *n = s_n;
    return s_table;
}
