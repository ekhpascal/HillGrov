#include <string.h>
#include "esp_log.h"
#include "cmd_core.h"
#include "cmd_common.h"
#include "zone_cmds.h"

static const char *TAG = "cmd_table_zone";
static cmd_entry_t s_table[64];
static int         s_n;
static int         s_init;

const cmd_entry_t *zone_table(int *n) {
    if (!s_init) {
        int total = CMD_COMMON_ROWS_N + ZONE_CMD_ROWS_N;
        if (total > 64) {
            ESP_LOGE(TAG, "table overflow: %d rows > 64 capacity, clamping", total);
            total = 64;
        }
        int n_common = CMD_COMMON_ROWS_N < total ? CMD_COMMON_ROWS_N : total;
        int n_zone = total - n_common;
        memcpy(s_table, CMD_COMMON_ROWS, (size_t)n_common * sizeof(cmd_entry_t));
        memcpy(s_table + n_common, ZONE_CMD_ROWS, (size_t)n_zone * sizeof(cmd_entry_t));
        s_n = total;
        s_init = 1;
    }
    if (n) *n = s_n;
    return s_table;
}
