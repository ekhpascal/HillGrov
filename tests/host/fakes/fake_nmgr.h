#pragma once
#include <stdint.h>
#include "ring_proto.h"

/* Host fake of node_mgr's plumbing seam (node_mgr_internal.h's first block):
 * time, seq, the hg_node_t table, the state lock and the three ring exits.
 * With this in place node_mgr_cfg.c + node_mgr_cfgx.c + node_mgr_cfg_api.c --
 * the whole of the spec section 4.4 reconciliation decision table -- link and
 * run on the host with no FreeRTOS, no UART and no NVS anywhere near them.
 *
 * Every frame the two halves push out is recorded rather than sent: tracked
 * frames (nmgr_submit: CFG_GET / CFG_COMMIT) in .sub with the seq the fake
 * handed back, untracked ones (nmgr_send_raw: the CFG_CHUNK burst) in .raw.
 * A test drives the decision half, then asserts on those two logs. */

typedef struct {
    uint8_t  dst, type, len;
    uint16_t seq;                    /* .sub only; .raw entries get one too, unused */
    uint8_t  payload[RING_MAX_PAYLOAD];
} fnm_rec_t;

#define FNM_MAX 64

typedef struct {
    hg_node_t tab[HG_MAX_ZONES];            /* the fixture table, slot = id-1 */
    fnm_rec_t sub[FNM_MAX];  int n_sub;     /* tracked submits, in order */
    fnm_rec_t raw[FNM_MAX];  int n_raw;     /* untracked sends, in order */
    uint16_t  cancel[FNM_MAX]; int n_cancel;
    int       submit_rc;                    /* set to -1 to play "tracker full" */
    uint16_t  next_seq;
    int       lock_depth, lock_max;         /* nmgr_lock() is a plain mutex on the
                                               target: a nested take deadlocks */
} fnm_t;

extern fnm_t g_nmgr;

void fnm_reset(void);
/* A used, ONLINE row heartbeating the given identity (the only hg_node_t
   fields the decision table reads). */
void fnm_node(uint8_t id, uint32_t cfg_gen, uint32_t cfg_crc, uint32_t hw_crc);
int  fnm_count(const fnm_rec_t *v, int n, uint8_t type);   /* records of this ring type */
