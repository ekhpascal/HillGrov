#pragma once
/* The panel's own command sessions: http_cmd.c's shape (components/http_srv/http_cmd.c:21-99) with web semantics (D1):
 * source CMD_SRC_HTTP, echo 0, notify_mask 0, unlock_until_ms 0 -- NOT_LOCAL for CMDF_SESSION rows, no DEBUG ENABLE
 * from the glass. SSIDs and passwords never travel as lines (a line cannot carry spaces): the panel calls psvc_* for those.
 * RESCUE CONFIRM (recovery design 6.3) reaches the panel only as a line typed into the zone console -- no button. */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PNL_CMD_SLOTS      2
#define PNL_CMD_TIMEOUT_MS 4000u   /* > the 3500 ms forward budget (http_cmd.c:23-30) */
void    pnl_cmd_init(void);        /* slots: CMD_SRC_HTTP semantics, own CMD_RESP_MAX buffers, 100 ms claim mutex */
int     pnl_cmd_run(const char *line, char *reply, size_t cap);
        /* [WORKER] 0 OK / -1 ERR (reply = the ERR line) / -2 orphaned (slot quarantined forever, reply "ERR INTERNAL") /
           -3 no free slot (reply "ERR BUSY") / -4 line empty or > CMD_LINE_MAX-1 (reply "ERR TOO_LONG"); the slot's own
           buffer is what cmd_task writes -- reply is a copy, so an orphan can never scribble on caller memory */
uint8_t pnl_cmd_quarantined(void); /* [ANY] */

#ifdef __cplusplus
}
#endif
