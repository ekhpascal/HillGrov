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
 * "ERR <token>\n" line. Returns 0/-1 exactly like cmd_dispatch. */
int  node_mgr_forward(uint8_t zone, const char *line, char *resp, int resp_len, uint32_t timeout_ms);

int  node_mgr_node_count(void);                                /* count of used/assigned table slots */
int  node_mgr_get(int slot, hg_node_t *out);                   /* raw table index 0..HG_MAX_ZONES-1 (id = slot+1);
                                                                    0 + *out on a used slot, else -1 */
void node_mgr_ring_status(ring_status_t *out);
int  node_mgr_set_name(uint8_t zone, const char *name);         /* -> ztab + node_store_save */
int  node_mgr_clear(uint8_t zone);                              /* CLEAR NODE */
int  node_mgr_unassigned(uint8_t macs[][6], int cap);           /* 0xFE heartbeaters awaiting slots (table full case) */
int  node_mgr_push_cfg(uint8_t zone);                           /* operator/reconciliation push of cached blob; -1 no cache */
int  node_mgr_time_valid(void);                                 /* for GET RING display */
void node_mgr_mark_updating(uint8_t zone, uint32_t hold_ms);    /* fleet sequencer (Task 15) */

/* master's SET TIME hook (app_if_master.c's time_set wrapper): time_valid in
 * the TIME_SYNC broadcast is 0 until this has been called once. */
void node_mgr_time_was_set(void);

#ifdef __cplusplus
}
#endif
