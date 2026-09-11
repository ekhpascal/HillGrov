#pragma once
#include <stdint.h>
#include "ring_proto.h"     /* hg_node_t, ring_status_t */
#include "hg_cfg_types.h"   /* hg_zone_cfg_t, hg_zone_hw_t (node_mgr_cfg_get/set) */

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
/* ---- §4.4 config primitives (node_mgr_cfg_api.c) ----
 * The web UI's zone config page and, later, the JSON rows are built on these
 * three. All are foreign-task safe (httpd workers, cmd_task).
 *
 * get: unwrapped COPIES of this zone's two cached planes. 0 = the CFG plane is
 *   cached; an absent HW plane is zeroed with *hw_gen 0 (a zone is adopted CFG
 *   first, so that window is real). -1 = zone out of range, or nothing cached
 *   yet -- the master has not finished adopting the zone. cfg is required,
 *   hw/cfg_gen/hw_gen may be NULL.
 * set: ASYNCHRONOUS. The payload is queued for the node_mgr task's next 1 Hz
 *   tick, which stamps generation = max(heartbeat, cache) + 1 and
 *   source = MASTER, adopts it into the cache and pushes it (chunks + a
 *   tracked CFG_COMMIT). 0 = queued -- phrase it to an operator as "saved",
 *   since the master's cache IS the authority from here on and the reconciler
 *   keeps re-pushing until the zone matches; a zone that REFUSES the config
 *   (CFG_VERSION / INVALID_FIELD) is reported through
 *   NOTIFY NODE <z> CFG_SYNC_FAILED and node_mgr_cfg_sync_failed(), not
 *   through this return value. -1 = zone unknown or not ONLINE, -2 = a write
 *   or a transfer for this zone is already in flight (answer HTTP 409).
 * busy: 1 while that is the case -- poll it to know when a save has landed. */
int  node_mgr_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw,
                      uint32_t *cfg_gen, uint32_t *hw_gen);
int  node_mgr_cfg_set(uint8_t zone, const hg_zone_cfg_t *cfg);
int  node_mgr_cfg_busy(uint8_t zone);

/* SET NODE <z> MAC (node_mgr_enrol.c): pre-seed the id -> MAC binding so a
 * replacement board is adopted straight into zone z on its first heartbeat
 * instead of landing in GET UNASSIGNED. The ztab row for id z takes this MAC
 * with flags ASSIGNED|UNCONFIGURED, replacing whatever MAC held the id (that
 * board becomes unknown and re-enrols elsewhere), and any row this MAC held
 * before is released. Persisted immediately; the RAM row is reset to
 * "assigned, never heard" and the id's cached config is dropped, since the
 * board answering on it is now a different one. 0 = stored (or already the
 * case), -1 = zone out of 1..8. */
int  node_mgr_seed_mac(uint8_t zone, const uint8_t mac[6]);

int  node_mgr_time_valid(void);                                 /* for GET RING display */
int  node_mgr_cfg_sync_failed(uint8_t zone);                    /* 1 = latched §4.4 CFG_SYNC
                                                                    failure; GET NODE display */
void node_mgr_mark_updating(uint8_t zone, uint32_t hold_ms);    /* fleet sequencer (Task 15) */

/* Fleet OTA sequencer (Task 15, node_mgr_fleet.c/fleet_seq.c) -- signatures
 * match node_ops_t's fw_* members exactly (master_cmds.h); cmd_table_master.c
 * wires these in verbatim, replacing Task 14's NULL-safe -1 stubs. 0 = ok
 * (fw_zone/fw_all: sequence accepted -- async, "OK QUEUED"+NOTIFY FW per
 * spec §5.3; fw_abort: a running sequence was cancelled), -1 = rejected
 * (already active, no assigned zones, or nothing to abort). */
int  node_mgr_fw_zone(uint8_t zone);
int  node_mgr_fw_all(void);
int  node_mgr_fw_abort(void);
int  node_mgr_fw_status(char *buf, size_t n);

/* master's SET TIME hook (app_if_master.c's time_set wrapper): time_valid in
 * the TIME_SYNC broadcast is 0 until this has been called once. */
void node_mgr_time_was_set(void);

#ifdef __cplusplus
}
#endif
