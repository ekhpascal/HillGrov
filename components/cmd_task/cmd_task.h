#pragma once
#include "cmd_core.h"
void cmd_task_start(const cmd_core_t *core);   /* creates the task: core 0, prio 5, 6144, TWDT-subscribed */
int  cmd_task_execute(cmd_session_t *ses, const char *line, char *resp, int resp_len, uint32_t timeout_ms);
