#include "cmd_core.h"
#include "cmd_common.h"

/* Master carries no config-model command rows in SP1 -- CMD_COMMON_ROWS is
 * the whole table, so core.table can point straight at it (no memcpy needed
 * the way zone_table() merges two row sets). */
const cmd_entry_t *master_table(int *n) {
    if (n) *n = CMD_COMMON_ROWS_N;
    return CMD_COMMON_ROWS;
}
