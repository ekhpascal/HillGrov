#pragma once
#include "cmd_core.h"
void cmd_task_start(const cmd_core_t *core);   /* creates the task: core 0, prio 5, 6144, TWDT-subscribed */

/* Runs one command line on the cmd_task worker and waits up to timeout_ms for
 * the reply. `resp` always comes back holding one complete reply line, on
 * every path. Return value:
 *
 *    0   the command succeeded ("OK ...")
 *   -1   the command failed; resp holds the "ERR <token>" line to send on
 *   -2   the wait timed out with the worker still inside cmd_dispatch. resp
 *        holds a synchronous "ERR INTERNAL" so the caller has something to
 *        answer with, BUT the worker was orphaned rather than cancelled and
 *        still owns `resp`: it will overwrite that line with the original
 *        command's real reply whenever the dispatch finally returns, long
 *        after the caller has moved on.
 *
 * A caller that answers once and forgets the buffer (the CLI, the zone's ring
 * ACK) can treat -2 exactly like -1. A caller that hands the same buffer to
 * the NEXT request -- http_srv's pooled /api/cmd sessions -- must not reuse a
 * buffer after -2; it quarantines that slot instead. Callers that lump every
 * non-zero return together therefore keep working unchanged, and only the one
 * that needs the distinction has to look for it. */
int  cmd_task_execute(cmd_session_t *ses, const char *line, char *resp, int resp_len, uint32_t timeout_ms);
