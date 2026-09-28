#pragma once
/* Pure (host-tested, tests/host/test_pnl_console.c): the zone console's model -- the forward rewrite (web/app.js:582-590),
 * the 20-entry sent/reply log, the history of ORIGINAL lines and the idle operator-state wipe (D18). No LVGL, no IDF.
 * Ownership rule: an entry with pending != 0 belongs to the panel worker (its sent line and reply buffer are being used
 * by a job) until the job's done() calls pnl_con_reply(); nothing here writes into a pending entry.
 * Every buffer that held a line or a reply is cleared with pnl_zero() before reuse (a console line can carry a secret). */
#include <stddef.h>
#include <stdint.h>
#include "cmd_core.h"   /* CMD_LINE_MAX, CMD_RESP_MAX -- a pure header */

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_CON_LOG  20
#define PNL_CON_HIST 20
typedef struct { char sent[CMD_LINE_MAX]; char *reply; uint8_t pending, used; } pnl_con_entry_t;  /* reply -> CMD_RESP_MAX */
typedef struct { pnl_con_entry_t log[PNL_CON_LOG]; int head, n_log; char hist[PNL_CON_HIST][CMD_LINE_MAX]; int n_hist, hist_pos;
                 uint8_t forward; char draft[CMD_LINE_MAX]; } pnl_console_t;

void pnl_con_init(pnl_console_t *c, char (*reply_store)[CMD_RESP_MAX]);   /* PNL_CON_LOG buffers (PSRAM, caller-owned) */
int  pnl_con_forward(const char *line, uint8_t zone, char *out, size_t cap);
     /* app.js:582-590: split on whitespace; tok2 == "ZONE" (case-insensitive) -> unchanged; else "VERB ZONE <z> rest"
        joined with single spaces; 0 / -1 result would exceed CMD_LINE_MAX-1 (or cap-1) */
const char *pnl_con_span(const char *line, size_t *len);
     /* the web's .trim(): skips leading and drops trailing space/tab/CR/LF; returns the first kept byte (never NULL; ""
        for a NULL line) and its length in *len (0 = nothing to send). Inner whitespace is kept. */
int  pnl_con_line_ok(const char *line);          /* pnl_con_span length is 1..CMD_LINE_MAX-1 bytes */
pnl_con_entry_t *pnl_con_push(pnl_console_t *c, const char *sent);
     /* newest entry, pending; NULL when the slot it would reuse is still pending (never overwrites the worker's buffer) */
void pnl_con_hist_add(pnl_console_t *c, const char *line);            /* the ORIGINAL line; keeps the newest PNL_CON_HIST */
const pnl_con_entry_t *pnl_con_log_at(const pnl_console_t *c, int i); /* 0 = oldest .. n_log-1 = newest; NULL outside */
void pnl_con_reply(pnl_con_entry_t *e, const char *reply);           /* verbatim, clipped to CMD_RESP_MAX-1; clears pending */
const char *pnl_con_hist_prev(pnl_console_t *c);   /* NULL when there is no history */
const char *pnl_con_hist_next(pnl_console_t *c);   /* "" past the newest */
void pnl_con_wipe(pnl_console_t *c);              /* pnl_zero log/history/draft; pending entries are left to the worker */

#ifdef __cplusplus
}
#endif
