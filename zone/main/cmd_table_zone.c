#include <string.h>
#include "cmd_core.h"
#include "cmd_common.h"
#include "zone_cmds.h"

static cmd_entry_t s_table[64];
static int         s_n;
static int         s_init;

const cmd_entry_t *zone_table(int *n) {
    if (!s_init) {
        memcpy(s_table, CMD_COMMON_ROWS, (size_t)CMD_COMMON_ROWS_N * sizeof(cmd_entry_t));
        memcpy(s_table + CMD_COMMON_ROWS_N, ZONE_CMD_ROWS, (size_t)ZONE_CMD_ROWS_N * sizeof(cmd_entry_t));
        s_n = CMD_COMMON_ROWS_N + ZONE_CMD_ROWS_N;
        s_init = 1;
    }
    if (n) *n = s_n;
    return s_table;
}
