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
void nmgr_cfg_invalidate(uint8_t zone);                          /* node_mgr task only: drop cfg+hw cache
                                                                    and failure latch, keep the fresh-HB
                                                                    flag -- master-originated edit (§4.4) */
void nmgr_cfg_request_clear(uint8_t zone);                       /* foreign-task-safe: queues for the tick */
void nmgr_cfg_note_fresh_hb(uint8_t zone);                       /* node_mgr task only (called from update_telemetry) */
void nmgr_cfg_tick_1s(uint32_t now);
void nmgr_cfg_on_chunk(const ring_frame_t *f, uint32_t now);
void nmgr_cfg_on_ev(const ring_trk_ev_t *ev);

/* fleet_seq.c: pure per-zone fleet OTA sequencer state machine (Task 15).
 * No IDF headers anywhere in this section or in fleet_seq.c itself -- it
 * touches no globals and calls nothing outside its own translation unit;
 * every side effect comes back as one fleet_act_t per call and the caller
 * (node_mgr_fleet.c, the glue half) executes it. Same "return one ev, let
 * the caller do IO" shape ring_trk.c uses for the tracker. Host-tested
 * directly (test_fleet_seq.c links only fleet_seq.c), mirroring how
 * ota_trial/trial_eval.c sits pure and host-tested beside ota_trial.c's
 * IDF glue in that component.
 *
 * Ladder per zone (controller ruling #3): PRECHECK (image_ok && online) ->
 * FA_START (mark_updating + NOTIFY UPDATING + submit tracked FW_UPDATE,
 * glue's job) -> ack ok -> WAIT_HB (an HB that ARRIVED AFTER the ack, with
 * a new fw triple or uptime < 60 s, within 180 s -- fix round ruling, see
 * fleet_tick's own comment) -> FA_DONE; anything else -> FA_FAILED and the
 * whole run stops (fw_all: "stop on first failure" resolves the brief's
 * own "target ONLINE else skip/fail" ambiguity toward "fail"). */
typedef enum { FZS_IDLE = 0, FZS_PRECHECK, FZS_WAIT_ACK, FZS_WAIT_HB } fz_state_t;
typedef struct {
    uint8_t    zones[HG_MAX_ZONES];
    uint8_t    n, idx;
    uint8_t    active, cancel_req;
    fz_state_t st;
    uint32_t   deadline_ms;        /* WAIT_HB expiry */
    uint32_t   ack_ms;             /* WAIT_HB entry time -- the freshness gate's reference point */
    uint16_t   seq;                /* tracked FW_UPDATE seq, WAIT_ACK/WAIT_HB */
    uint8_t    pre_fw[3];
} fleet_t;

typedef enum { FA_NONE = 0, FA_START, FA_FAILED, FA_DONE } fleet_act_kind_t;
typedef struct { fleet_act_kind_t kind; uint8_t zone; uint8_t fw[3]; } fleet_act_t;

void fleet_init(fleet_t *s);
int  fleet_start(fleet_t *s, const uint8_t *zones, uint8_t n);   /* 0 ok, -2 already active, -1 invalid n */
void fleet_cancel(fleet_t *s);                                    /* "between zones": in-flight zone finishes on its own */
/* 1 Hz-ish tick: PRECHECK judged from image_ok/online (current zone);
   WAIT_HB judged from hb_valid/hb_fw/hb_uptime/hb_arrived_ms (current
   zone's HB snapshot + the ms timestamp it last arrived at) vs now_ms and
   the recorded deadline. Returns 1 with *out on FA_START/FA_DONE/
   FA_FAILED, else 0 (WAIT_ACK: nothing to do here, see fleet_on_ack). */
int  fleet_tick(fleet_t *s, uint32_t now_ms, int image_ok, int online,
                int hb_valid, const uint8_t hb_fw[3], uint32_t hb_uptime, uint32_t hb_arrived_ms,
                fleet_act_t *out);
/* the tracked FW_UPDATE's ACK (or ZONE_TIMEOUT/ZONE_UNKNOWN, ok=0 either
   way) arrived; ignored outside WAIT_ACK or for a stale/foreign seq. */
int  fleet_on_ack(fleet_t *s, uint16_t seq, int ok, uint32_t now_ms, fleet_act_t *out);
/* records the glue's actual nmgr_submit() outcome for an FA_START just
   issued; submitted=0 (tracker full/busy) fails the zone right there. */
int  fleet_note_submitted(fleet_t *s, int submitted, uint16_t seq, uint32_t now_ms,
                           const uint8_t pre_fw[3], fleet_act_t *out);
void fleet_status(const fleet_t *s, char *buf, size_t cap);       /* GET FW ZONE rendering, no leading "ZONE" */

/* node_mgr_fleet.c: glue driving fleet_seq.c from node_mgr.c's task loop
   (mirrors nmgr_cfg_tick_1s/nmgr_cfg_on_ev's wiring). node_mgr_fw_zone/
   fw_all/fw_abort/fw_status (node_mgr.h) reach the SAME fleet_t instance
   from cmd_task -- node_mgr_fleet.c guards it with its own dedicated mutex
   (created here), the same "new foreign-task-shared state gets its own
   lock" pattern node_mgr_fwd.c's s_busy uses, rather than reusing
   nmgr_lock() (whose critical sections already call notify_emit(), so
   nesting it under a second lock risks non-obvious ordering). */
void nmgr_fleet_init(void);
void nmgr_fleet_tick_1s(uint32_t now);
void nmgr_fleet_on_ev(const ring_trk_ev_t *ev);
