#pragma once
#include <stdint.h>
#include "ring_proto.h"
#include "ring_link.h"

/* Private to node_mgr.c / node_mgr_enrol.c / node_mgr_fwd.c / node_mgr_cfg.c
 * -- not part of the component's public surface (node_mgr.h). Split purely
 * to keep each file under the ~300-line cap, the same pattern zone_ring used
 * in Task 12. */

/* node_mgr.c: shared time/seq/table/tracker plumbing. The ring_trk_t
 * instance and its guarding mutex are private to node_mgr.c -- nmgr_submit()
 * is the only way in, so a foreign-task caller (node_mgr_forward, off the
 * cmd_task thread) can never touch it unlocked. */
uint32_t   nmgr_now_ms(void);
uint16_t   nmgr_next_seq(void);
uint8_t    nmgr_ring_size(void);                                /* assigned-node count, for ack timeout */
hg_node_t *nmgr_node_by_id(uint8_t id);                         /* id 1..8 -> &table[id-1]; NULL out of range */
hg_node_t *nmgr_table(void);                                    /* base of the HG_MAX_ZONES table (slot = id-1) */

/* Guards every cross-task-visible mutable bit node_mgr owns: the node table
 * (s_tab), ring_status, the ztab, and node_mgr_cfg.c's pending-request flags
 * (fix round: important #3 / minor #7). A plain FreeRTOS mutex (not a
 * portMUX critical section) because some sections held under it call
 * notify_emit(), which is not critical-section-safe. Only ever taken for
 * short, non-blocking-beyond-NVS sections; never held across a ring send or
 * a wait on another primitive. */
void nmgr_lock(void);
void nmgr_unlock(void);

/* Builds an ACK_REQ'd header (src=master, ttl=RING_TTL_INIT, fresh seq) and
 * queues it on the shared tracker. 0 + *seq_out on success, -1 = BUSY (queue
 * full -- ring_trk_submit's own return). */
int  nmgr_submit(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len, uint16_t *seq_out);
/* Unicast, unACKed, sent straight to the wire with a fresh seq (TIME_SYNC,
 * ASSIGN_ID, and push's CFG_CHUNKs are never tracked -- spec §2.9). */
void nmgr_send_raw(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len);

/* node_mgr_enrol.c: ztab ownership, HB-driven enrolment, TIME_SYNC broadcast,
 * and ring_health_eval's line-formatting callback. All the mutating entry
 * points below assume the caller already holds nmgr_lock() (they're called
 * only from node_mgr.c's own already-locked sections, or from within the
 * node_mgr task -- see node_mgr_enrol.c's nmgr_enrol_handle_hb). */
void nmgr_enrol_boot_init(void);
void nmgr_enrol_handle_hb(const ring_frame_t *f);
void nmgr_broadcast_time_sync(uint32_t now);
void nmgr_health_cb(void *ctx, const char *line);               /* ring_health_ev_cb-shaped */
int  nmgr_ztab_set_name(uint8_t zone, const char *name);         /* caller holds nmgr_lock() */
int  nmgr_ztab_clear(uint8_t zone);                              /* caller holds nmgr_lock() */
int  nmgr_unassigned_copy(uint8_t macs[][6], int cap);           /* locks internally */

/* node_mgr_fwd.c: node_mgr_forward()'s busy slot + completion hook, called
 * from node_mgr.c's tick loop whenever a tracker event's type is RING_T_CMD. */
void nmgr_fwd_init(void);
void nmgr_fwd_on_ev(const ring_trk_ev_t *ev);

/* node_mgr_cfg.c: cache init, the 1 Hz reconciliation/retry driver, the
 * CFG_CHUNK feed (pull assembler) and the tracker completion hook for
 * RING_T_CFG_GET / RING_T_CFG_COMMIT. nmgr_cfg_clear() touches s_cx/s_cfg/
 * s_hw/s_casm directly and must only ever run on the node_mgr task (via the
 * 1 Hz tick, itself, or boot) -- node_mgr_clear() queues it through
 * nmgr_cfg_request_clear() instead of calling it directly (important #3). */
void nmgr_cfg_init(void);
void nmgr_cfg_clear(uint8_t zone);                               /* node_mgr task only */
void nmgr_cfg_request_clear(uint8_t zone);                       /* foreign-task-safe: queues for the tick */
void nmgr_cfg_note_fresh_hb(uint8_t zone);                       /* node_mgr task only (called from update_telemetry) */
void nmgr_cfg_tick_1s(uint32_t now);
void nmgr_cfg_on_chunk(const ring_frame_t *f, uint32_t now);
void nmgr_cfg_on_ev(const ring_trk_ev_t *ev);
