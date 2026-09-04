#pragma once
#include <stdint.h>
#include "ring_proto.h"   /* hg_node_t, ring_status_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Master's ring orchestration task (spec §6.1): tracker/TIME_SYNC glue,
 * enrolment, health, §4.4 config reconciliation, CLI forward path. Owns the
 * node table (hg_node_t[HG_MAX_ZONES]), the ztab, the pending tracker and the
 * RAM config/HW caches -- everything else (master_cmds, fleet) goes through
 * this API. */

void node_mgr_start(void);

/* ZONE-prefix forward path: validates against the node table/health/ring
 * state, submits a CMD frame via the tracker (BUSY when the tracker/forward
 * slot is full/busy), blocks up to timeout_ms on a per-call semaphore, then
 * copies the ACK detail verbatim into resp (already CLI-shaped) or an
 * "ERR <token>\n" line. Returns 0/-1 exactly like cmd_dispatch.
 * timeout_ms is clamped to 5000 ms internally (minor #8) -- a caller cannot
 * block this call, or the forward slot it holds, past that regardless of
 * what it passes. RING_DOWN before node_mgr_start() has run (boot window).
 * At-most-once: the ring's own dup cache means a forwarded command is never
 * re-executed at the zone even across a client-side retry; a ZONE_TIMEOUT
 * here only means the ACK was lost/late, not that the zone didn't run it --
 * this layer never retries automatically (spec §5.3: doses aren't
 * idempotent), so a caller that itself retries after ZONE_TIMEOUT can cause
 * a real double-execution and must not do so blindly. */
int  node_mgr_forward(uint8_t zone, const char *line, char *resp, int resp_len, uint32_t timeout_ms);

int  node_mgr_node_count(void);                                /* count of used/assigned table slots */
int  node_mgr_get(int slot, hg_node_t *out);                   /* raw table index 0..HG_MAX_ZONES-1 (id = slot+1);
                                                                    0 + *out on a used slot, else -1 */
void node_mgr_ring_status(ring_status_t *out);
int  node_mgr_set_name(uint8_t zone, const char *name);         /* -> ztab + node_store_save */
int  node_mgr_clear(uint8_t zone);                              /* CLEAR NODE */
int  node_mgr_unassigned(uint8_t macs[][6], int cap);           /* 0xFE heartbeaters awaiting slots (table full case) */

/* Async (important #3): queues an operator/reconciliation push of the
 * cached cfg blob for the node_mgr task's next 1 Hz tick -- it does not
 * push, block, or touch the ring itself. 0 = queued for the tick (the row
 * handler should phrase this to the operator as "push scheduled", not
 * "pushed" -- the tick may still find the transfer slot busy at processing
 * time and silently drop this request, single-slot/last-caller-wins, same
 * as any other in-flight reconciliation); -1 = no cache for this zone yet,
 * or zone out of range -- checked with an unlocked, best-effort read, so an
 * occasional stale -1/0 near a concurrent cache change is possible and
 * harmless (retry). */
int  node_mgr_push_cfg(uint8_t zone);
int  node_mgr_time_valid(void);                                 /* for GET RING display */
void node_mgr_mark_updating(uint8_t zone, uint32_t hold_ms);    /* fleet sequencer (Task 15) */

/* master's SET TIME hook (app_if_master.c's time_set wrapper): time_valid in
 * the TIME_SYNC broadcast is 0 until this has been called once. */
void node_mgr_time_was_set(void);

#ifdef __cplusplus
}
#endif
