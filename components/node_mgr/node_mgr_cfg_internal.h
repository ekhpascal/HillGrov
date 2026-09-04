#pragma once
#include <stdint.h>
#include "hg_blob.h"
#include "hg_cfg_types.h"
#include "node_mgr_internal.h"

/* Private seam between the two halves of §4.4 config reconciliation (the split
 * the final review named; node_mgr_cfg.c had grown past the line cap):
 *
 *  - node_mgr_cfg.c   LATCH/DECISION half -- the per-zone RAM caches, the
 *                     terminal-failure latch, the fresh-HB gate, and the 1 Hz
 *                     decision (adopt / push / already in sync).
 *  - node_mgr_cfgx.c  TRANSFER half -- the single current-transfer slot: pull
 *                     assembler, push chunker, tracker hook, retry ladder.
 *
 * The seam is exactly that: the decision half never touches the transfer slot
 * (it drives it through the nmgr_cx_* calls below), and the transfer half never
 * decides anything -- it reports each outcome back through note_synced /
 * note_failed. Both halves run ONLY on the node_mgr task (see node_mgr_cfg.c's
 * own header comment on single-writer ownership). */

typedef struct {
    uint8_t  blob[HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t)];   /* sized for the larger (CFG) plane */
    uint32_t gen, crc;                                        /* crc = hg_crc32 over the UNWRAPPED payload */
    uint8_t  valid;
} nmgr_cache_t;

/* ---- decision half, called by the transfer half ---- */

/* kind 1 = CFG (pushed and pulled), kind 2 = HW (pulled only, spec §4.4).
   NULL for a zone or kind out of range. */
nmgr_cache_t *nmgr_cfg_cache(uint8_t zone, uint8_t kind);

/* A transfer completed: the cached identity for this zone has just changed, so
   any terminal-failure latch on it is stale. */
void nmgr_cfg_note_synced(uint8_t zone);

/* A transfer gave up: emits CFG_SYNC_FAILED and degrades the node; terminal
   (an ACK token that is never retried -- CFG_VERSION / INVALID_FIELD) also
   latches the (heartbeat, cache) identity that produced it so the automatic
   reconciler leaves that zone alone until either identity changes. */
void nmgr_cfg_note_failed(uint8_t zone, uint8_t kind, int terminal,
                          uint32_t hb_gen, uint32_t hb_crc,
                          uint32_t cache_gen, uint32_t cache_crc);

/* ---- transfer half, called by the decision half ---- */

void nmgr_cx_init(void);
int  nmgr_cx_idle(void);                 /* 1 = nothing in flight, a new decision may start */
int  nmgr_cx_tick_1s(uint32_t now);      /* 1 = the tick belonged to a transfer (stream/retry/awaiting ACK) */
void nmgr_cx_pull(uint8_t zone, uint8_t kind);
void nmgr_cx_push(uint8_t zone, uint32_t gen,
                  uint32_t hb_gen, uint32_t hb_crc, uint32_t cache_gen, uint32_t cache_crc);
/* Drop this zone's transfer, withdrawing its tracked frame if it has not been
   sent yet (the §4.4 revert race -- see nmgr_cfg_invalidate). */
void nmgr_cx_abort(uint8_t zone);
