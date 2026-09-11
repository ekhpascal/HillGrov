#include <string.h>
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"
#include "node_mgr_cfg_internal.h"

/* §4.4's FAILURE MEMORY: the two throttles that stop a zone the reconciler
 * cannot satisfy from owning it, and the success/failure reporting the transfer
 * half calls in with. Split out of node_mgr_cfg.c (the decision half) to keep
 * both files inside the ~300-line cap -- the decision logic reaches this state
 * only through the functions below.
 *
 *   TERMINAL LATCH -- an ACK token that is never retried (CFG_VERSION /
 *     INVALID_FIELD), or an envelope this master cannot parse at all
 *     (E_VERSION_NEWER), records the exact (heartbeat, cache) identity that
 *     produced it; the automatic reconciler then skips that plane until one of
 *     the two identities changes. A manual push bypasses it, but can set a new
 *     one on the same failure.
 *   COOLDOWN -- a transfer that failed NON-terminally (the retry ladder ran
 *     out) parks the zone for CFG_COOLDOWN_MS rather than flapping
 *     CFG_SYNC_FAILED every tick. Per ZONE, not per plane: it throttles the
 *     zone's whole turn at the reconciler.
 *
 * ONE LATCH PER PLANE (fix round 2). With a single slot per zone, a zone whose
 * CFG *and* HW envelopes were both too new flapped for ever: the CFG latch let
 * the HW pull through (which is correct -- separate blobs, separate versions),
 * the HW failure then overwrote the CFG latch, the next heartbeat re-pulled
 * CFG, and round it went -- one pull and one CFG_SYNC_FAILED per heartbeat,
 * with cmd_timeouts pinned at 3 so the zone sat DEGRADED. Indexing by kind
 * makes each plane's verdict independent, which is what the fall-through in
 * try_start already assumed.
 *
 * Locking: the latch is written here and read by node_mgr_cfg_sync_failed from
 * cmd_task/httpd, so every write AND that read take nmgr_lock(). The cooldown
 * is node_mgr-task-only (written and read on the 1 Hz tick) and stays
 * unlocked, like s_fresh and the caches' own gen/crc fields. */

typedef struct { uint8_t valid; uint32_t hb_gen, hb_crc, cache_gen, cache_crc; } nmgr_latch_t;
static nmgr_latch_t s_latch[HG_MAX_ZONES][2];      /* [zone-1][kind-1]; kind 1 CFG, 2 HW */

#define CFG_COOLDOWN_MS 30000u
static uint32_t s_cooldown_until[HG_MAX_ZONES];    /* 0 = not cooling down */

static nmgr_latch_t *slot(uint8_t zone, uint8_t kind) {
    if (zone < 1 || zone > HG_MAX_ZONES || kind < 1 || kind > 2) return NULL;
    return &s_latch[zone - 1][kind - 1];
}

void nmgr_cfg_latch_init(void) {
    nmgr_lock();
    memset(s_latch, 0, sizeof s_latch);
    nmgr_unlock();
    memset(s_cooldown_until, 0, sizeof s_cooldown_until);
}

void nmgr_cfg_latch_clear(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    nmgr_lock();
    memset(s_latch[zone - 1], 0, sizeof s_latch[0]);   /* both planes */
    nmgr_unlock();
}

void nmgr_cfg_cooldown_clear(uint8_t zone) {
    if (zone >= 1 && zone <= HG_MAX_ZONES) s_cooldown_until[zone - 1] = 0;
}

/* 1 = skip this zone this turn. Wrap-safe (minor #5), and an expired cooldown
   is cleared on the way past so the arithmetic cannot drift. */
int nmgr_cfg_cooling_down(uint8_t zone, uint32_t now) {
    if (zone < 1 || zone > HG_MAX_ZONES || !s_cooldown_until[zone - 1]) return 0;
    if ((int32_t)(now - s_cooldown_until[zone - 1]) < 0) return 1;
    s_cooldown_until[zone - 1] = 0;
    return 0;
}

int nmgr_cfg_latched(uint8_t zone, uint8_t kind, uint32_t hb_gen, uint32_t hb_crc,
                     uint32_t cache_gen, uint32_t cache_crc) {
    const nmgr_latch_t *L = slot(zone, kind);
    return L && L->valid &&
           L->hb_gen == hb_gen && L->hb_crc == hb_crc &&
           L->cache_gen == cache_gen && L->cache_crc == cache_crc;
}

void nmgr_cfg_note_synced(uint8_t zone, uint8_t kind) {
    nmgr_latch_t *L = slot(zone, kind);
    if (!L) return;
    /* This plane's cache identity just changed, so its latch could no longer
     * match anyway -- clearing it is what puts GET NODE's CfgSync back to OK.
     * The other plane's latch stands. */
    nmgr_lock();
    memset(L, 0, sizeof *L);
    nmgr_unlock();
    /* A completed transfer is a successful master->zone exchange, so the
     * consecutive-failure count starts over -- including the 3 note_failed
     * sets below to force DEGRADED. Heartbeats no longer clear it (see
     * node_mgr_enrol.c), so a success has to. */
    hg_node_t *nd = nmgr_node_by_id(zone);
    if (nd) { nmgr_lock(); nd->cmd_timeouts = 0; nmgr_unlock(); }
}

void nmgr_cfg_note_failed(uint8_t zone, uint8_t kind, int terminal,
                          uint32_t hb_gen, uint32_t hb_crc,
                          uint32_t cache_gen, uint32_t cache_crc) {
    nmgr_latch_t *L = slot(zone, kind);
    if (!L) return;
    notify_emit_as(zone, NTF_NODE, zone, "CFG_SYNC_FAILED");
    hg_node_t *nd = nmgr_node_by_id(zone);
    if (nd) {
        nmgr_lock();
        if (nd->cmd_timeouts < 3) nd->cmd_timeouts = 3;   /* DEGRADED via ring_health_eval's own rule */
        nmgr_unlock();
    }
    if (!terminal) {
        s_cooldown_until[zone - 1] = nmgr_now_ms() + CFG_COOLDOWN_MS;
        return;                    /* retryable: no identity is written off */
    }
    nmgr_lock();
    L->valid = 1;
    L->hb_gen = hb_gen; L->hb_crc = hb_crc;
    L->cache_gen = cache_gen; L->cache_crc = cache_crc;
    nmgr_unlock();
}

/* GET NODE's CfgSync field: FAILED while EITHER plane is latched -- the
 * operator wants to know the zone is not converging, whichever blob is at
 * fault, and the NOTIFY line that came with it named the plane. */
int node_mgr_cfg_sync_failed(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return 0;
    nmgr_lock();
    int v = s_latch[zone - 1][0].valid || s_latch[zone - 1][1].valid;
    nmgr_unlock();
    return v;
}
