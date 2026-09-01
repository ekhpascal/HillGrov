#pragma once
#include "cmd_core.h"
#include "app_if.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Stores app for the lifetime of the process; call once at startup before
 * any command dispatch reaches CMD_COMMON_ROWS. */
void cmd_common_init(const app_if_t *app);

extern const cmd_entry_t CMD_COMMON_ROWS[];
extern const int         CMD_COMMON_ROWS_N;

#ifdef __cplusplus
}
#endif
