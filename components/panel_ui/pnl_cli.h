#pragma once
/* The panel's own CLI rows, merged into the P4 master's command table (master/main/cmd_table_master.c, P4 only: the
 * rows exist only where the panel does). */
#include "cmd_core.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const cmd_entry_t PANEL_CMD_ROWS[];   /* 1 row: CLEAR PANEL CONFIRM */
extern const int         PANEL_CMD_ROWS_N;

#ifdef __cplusplus
}
#endif
