#include <string.h>
#include "hg_blob.h"
#include "hg_cfg_types.h"
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"
#include "node_mgr_cfg_internal.h"

/* §4.4 config reconciliation, LATCH/DECISION half: the per-zone RAM caches,
 * the terminal-failure latch, the fresh-HB gate and the 1 Hz decision (adopt /
 * push / already in sync). Carrying a decision out on the wire is the transfer
 * half's job (node_mgr_cfgx.c) -- see node_mgr_cfg_internal.h for the seam.
 * Single-writer (important #3): s_cfg/s_hw/s_latch/s_fresh are touched ONLY
 * from the node_mgr task; foreign-task entry points just set a request flag
 * under nmgr_lock(), consumed by the 1 Hz tick.
 *
 * Two fairness rules keep one sick zone from owning the reconciler (final
 * review G2 -- the old loop scanned from id 1 every tick and returned on the
 * first zone that started a transfer, so a zone failing non-terminally every
 * ~15 s starved every higher id and flapped CFG_SYNC_FAILED/DEGRADED):
 *   (a) ROUND ROBIN -- the scan resumes after the last zone that got a
 *       decision, so every zone reaches the front within one full turn.
 *   (b) COOLDOWN -- a zone whose transfer failed non-terminally is skipped for
 *       CFG_COOLDOWN_MS. Terminal failures need no cooldown: they latch (see
 *       s_latch) and are not retried at all until an identity changes.
 * Both are per-zone and RAM-only; nmgr_cfg_invalidate clears the cooldown,
 * because a master-originated edit must be adopted on the next heartbeat and
 * not after the remainder of a cooldown. Treating E_VERSION_* on a PULL as
 * terminal is deliberately NOT here -- mixed-version fleets are SP4's
 * scenario (carry). */

static nmgr_cache_t s_cfg[HG_MAX_ZONES];   /* kind 1, slot = id-1 */
static nmgr_cache_t s_hw[HG_MAX_ZONES];    /* kind 2 -- display/export only, never pushed (spec §4.4) */
static uint8_t      s_fresh[HG_MAX_ZONES]; /* important #2: a fresh HB has arrived since the last decision */

/* important #2: a push failing on a never-retried ACK token (CFG_VERSION /
 * INVALID_FIELD) latches the (hb, cache) identity that produced it -- the
 * automatic reconciler won't retry that zone until either identity changes.
 * A manual push bypasses the latch but can set a new one on the same fail. */
typedef struct { uint8_t valid, kind; uint32_t hb_gen, hb_crc, cache_gen, cache_crc; } nmgr_latch_t;
static nmgr_latch_t s_latch[HG_MAX_ZONES];

#define CFG_COOLDOWN_MS 30000u
static uint32_t s_cooldown_until[HG_MAX_ZONES];   /* rule (b); 0 = not cooling down */
static uint8_t  s_rr_next = 1;                    /* rule (a): id the next scan starts at */

/* fix round 2 item 1: push stays single-slot/last-caller-wins -- the transfer
 * half can only run one push at a time regardless, and a superseded manual push
 * is a cheap, operator-retriable no-op. Clear is a per-zone BITMASK: a single
 * slot dropped the second of two CLEAR NODE requests in one tick window, so a
 * replacement board enrolling on that id could inherit the retired board's
 * cached config (§4.4 adopt violated) -- clears are cheap (no ring traffic), so
 * every bit set gets consumed each pass. */
static volatile uint8_t s_push_req_zone;    /* 0 = none pending; set under nmgr_lock() */
static volatile uint8_t s_clear_mask;       /* bit (zone-1) set = clear pending; set under nmgr_lock() */

nmgr_cache_t *nmgr_cfg_cache(uint8_t zone, uint8_t kind) {
    if (zone < 1 || zone > HG_MAX_ZONES) return NULL;
    if (kind == 1) return &s_cfg[zone - 1];
    if (kind == 2) return &s_hw[zone - 1];
    return NULL;
}

void nmgr_cfg_note_synced(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    memset(&s_latch[zone - 1], 0, sizeof s_latch[0]);   /* cache identity changed: any old latch is stale */
}

void nmgr_cfg_note_failed(uint8_t zone, uint8_t kind, int terminal,
                          uint32_t hb_gen, uint32_t hb_crc,
                          uint32_t cache_gen, uint32_t cache_crc) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    notify_emit(NTF_NODE, zone, "%u CFG_SYNC_FAILED", zone);
    hg_node_t *nd = nmgr_node_by_id(zone);
    if (nd && nd->cmd_timeouts < 3) nd->cmd_timeouts = 3;   /* DEGRADED via ring_health_eval's own rule */
    if (!terminal) s_cooldown_until[zone - 1] = nmgr_now_ms() + CFG_COOLDOWN_MS;   /* rule (b) */
    if (terminal) {
        nmgr_latch_t *L = &s_latch[zone - 1];
        L->valid = 1; L->kind = kind;
        L->hb_gen = hb_gen; L->hb_crc = hb_crc;
        L->cache_gen = cache_gen; L->cache_crc = cache_crc;
    }
}

/* one reconciliation decision per fresh-HB'd, reachable zone -- spec §4.4 */
static int try_start(uint8_t zone, const hg_node_t *nd) {
    nmgr_cache_t *cfg = &s_cfg[zone - 1];
    nmgr_cache_t *hw  = &s_hw[zone - 1];
    if (!cfg->valid) { nmgr_cx_pull(zone, 1); return 1; }
    if (!hw->valid || nd->hb.hw_crc != hw->crc) { nmgr_cx_pull(zone, 2); return 1; }

    if (nd->hb.cfg_gen == cfg->gen && nd->hb.cfg_crc == cfg->crc) return 0;   /* in sync */

    nmgr_latch_t *L = &s_latch[zone - 1];
    if (L->valid && L->hb_gen == nd->hb.cfg_gen && L->hb_crc == nd->hb.cfg_crc &&
        L->cache_gen == cfg->gen && L->cache_crc == cfg->crc)
        return 0;   /* this exact identity already failed terminally -- wait for a change */

    uint32_t new_gen = (nd->hb.cfg_gen > cfg->gen ? nd->hb.cfg_gen : cfg->gen) + 1;
    if (nd->hb.cfg_gen == cfg->gen)
        notify_emit(NTF_NODE, zone, "%u CFG_FORK", zone);
    else if (nd->hb.cfg_gen > cfg->gen)
        notify_emit(NTF_NODE, zone, "%u CFG_REVERTED %lu", zone, (unsigned long)new_gen);
    nmgr_cx_push(zone, new_gen, nd->hb.cfg_gen, nd->hb.cfg_crc, cfg->gen, cfg->crc);
    return 1;
}

void nmgr_cfg_init(void) {
    memset(s_cfg, 0, sizeof s_cfg);
    memset(s_hw, 0, sizeof s_hw);
    memset(s_latch, 0, sizeof s_latch);
    memset(s_fresh, 0, sizeof s_fresh);
    memset(s_cooldown_until, 0, sizeof s_cooldown_until);
    s_rr_next = 1;
    nmgr_cx_init();
}

/* Drop everything that makes this zone's cached config authoritative, so
 * try_start's next decision is "no cache -> adopt" (spec §4.4's enrolment
 * branch) instead of a revert push. Also cancels an in-flight transfer for
 * the zone, whose frozen identity is now stale. node_mgr task only. */
void nmgr_cfg_invalidate(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    memset(&s_cfg[zone - 1], 0, sizeof s_cfg[0]);
    memset(&s_hw[zone - 1], 0, sizeof s_hw[0]);
    memset(&s_latch[zone - 1], 0, sizeof s_latch[0]);
    s_cooldown_until[zone - 1] = 0;   /* adopt on the NEXT heartbeat, not after the cooldown */
    nmgr_cx_abort(zone);
}

/* node_mgr task only -- see node_mgr_internal.h. Retiring an id additionally
 * withdraws the pending decision opportunity: the row is gone, and whatever
 * board answers on that id next is a different node whose own heartbeat must
 * grant the next one. */
void nmgr_cfg_clear(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    nmgr_cfg_invalidate(zone);
    s_fresh[zone - 1] = 0;
}

void nmgr_cfg_request_clear(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    nmgr_lock();
    s_clear_mask |= (uint8_t)(1u << (zone - 1));
    nmgr_unlock();
}

void nmgr_cfg_note_fresh_hb(uint8_t zone) {
    if (zone >= 1 && zone <= HG_MAX_ZONES) s_fresh[zone - 1] = 1;
}

void nmgr_cfg_tick_1s(uint32_t now) {
    nmgr_lock();
    uint8_t clear_mask = s_clear_mask;    s_clear_mask     = 0;
    uint8_t push_zone  = s_push_req_zone; s_push_req_zone  = 0;
    nmgr_unlock();

    /* every bit set gets cleared this pass -- clears are cheap (no ring
     * traffic), so there's no reason to throttle to one per tick the way
     * push is (fix round 2 item 1). */
    for (uint8_t id = 1; id <= HG_MAX_ZONES; id++)
        if (clear_mask & (uint8_t)(1u << (id - 1))) nmgr_cfg_clear(id);

    if (nmgr_cx_tick_1s(now)) return;   /* a transfer owns this tick (streaming / retry / awaiting ACK) */

    /* manual push bypasses the fresh-HB gate/latch below (both throttle the
     * *automatic* reconciler); re-checks cache validity from this task,
     * since the public API's own check was an unlocked, best-effort read. */
    if (push_zone && s_cfg[push_zone - 1].valid) {
        hg_node_t *nd = nmgr_node_by_id(push_zone);
        nmgr_cache_t *cfg = &s_cfg[push_zone - 1];
        uint32_t hb_gen = (nd && nd->used) ? nd->hb.cfg_gen : 0;
        uint32_t hb_crc = (nd && nd->used) ? nd->hb.cfg_crc : 0;
        uint32_t new_gen = (hb_gen > cfg->gen ? hb_gen : cfg->gen) + 1;
        nmgr_cx_push(push_zone, new_gen, hb_gen, hb_crc, cfg->gen, cfg->crc);
        return;
    }

    /* rule (a): start where the last decision left off, so no zone can be
     * starved by a lower id that keeps finding work. */
    for (uint8_t i = 0; i < HG_MAX_ZONES; i++) {
        uint8_t id = (uint8_t)((s_rr_next - 1 + i) % HG_MAX_ZONES + 1);
        if (!s_fresh[id - 1]) continue;
        s_fresh[id - 1] = 0;   /* this fresh HB grants exactly one decision opportunity (important #2) */
        hg_node_t *nd = nmgr_node_by_id(id);
        if (!nd || !nd->used) continue;
        if (nd->health == NODE_H_OFFLINE || nd->health == NODE_H_UPDATING) continue;
        /* rule (b): still cooling down after a non-terminal failure (wrap-safe) */
        if (s_cooldown_until[id - 1] && (int32_t)(nmgr_now_ms() - s_cooldown_until[id - 1]) < 0) continue;
        s_cooldown_until[id - 1] = 0;
        s_rr_next = (uint8_t)(id % HG_MAX_ZONES + 1);
        if (try_start(id, nd)) return;
    }
}

int node_mgr_push_cfg(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return -1;
    if (!s_cfg[zone - 1].valid) return -1;   /* unlocked single-word read (disclosed): a stale value
                                                 false-rejects/-accepts; the tick re-checks before pushing */
    nmgr_lock();
    s_push_req_zone = zone;
    nmgr_unlock();
    return 0;   /* accepted for processing -- async; the 1 Hz tick starts the actual push */
}

/* GET NODE's CfgSync field. s_latch stays node_mgr-task-owned (file header);
 * nmgr_lock() here is a read-side rendezvous only, same tolerance as the
 * unlocked reads above -- the writers don't take it either. */
int node_mgr_cfg_sync_failed(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return 0;
    nmgr_lock();
    int v = s_latch[zone - 1].valid;
    nmgr_unlock();
    return v;
}
