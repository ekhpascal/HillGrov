#include <string.h>
#include "hg_blob.h"
#include "hg_cfg_types.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"
#include "node_mgr_cfg_internal.h"

/* §4.4's public primitives -- the three calls the web UI's zone config page is
 * built on (SP4 Task 9), split out of node_mgr_cfg.c to keep both files inside
 * the ~300-line cap. They are the only part of the config machinery reached
 * from a FOREIGN task (httpd workers, cmd_task), so:
 *
 *   - get COPIES under nmgr_lock() and unwraps afterwards, never handing out a
 *     pointer into the live cache;
 *   - set does not push. It parks the payload in the inbox below for the 1 Hz
 *     tick to carry out (nmgr_cfg_take_set_req), the same shape
 *     node_mgr_push_cfg and node_mgr_clear already use -- nothing outside the
 *     node_mgr task may touch the transfer slot, the caches or the ring
 *     (important #3).
 *
 * Locking, precisely: nmgr_lock() here IS mutual exclusion against the four
 * cache writers, which all take it for their blob/gen/crc/valid update
 * (accept_pull, accept_push, start_set_req, nmgr_cfg_invalidate) -- so a copy
 * taken here is always a whole, self-consistent envelope, at worst one tick
 * stale. The envelope crc the unwrap checks is a second line of defence (a
 * writer that forgets the lock, or memory corruption), not an unreachable
 * branch. What the lock does NOT cover is the transfer slot read by
 * nmgr_cx_busy, which stays node_mgr-task-owned and best-effort (see
 * node_mgr_cfg_busy's own comment). */

/* The write inbox: one slot PER ZONE, rather than the single
 * last-caller-wins slot node_mgr_push_cfg uses, because this request carries
 * the operator's payload -- dropping one would lose an edit they were told was
 * accepted. A second write for the same zone is refused with -2 while one is
 * in flight instead of overwriting it. */
typedef struct { uint8_t pending; hg_zone_cfg_t cfg; } set_req_t;
static set_req_t s_set_req[HG_MAX_ZONES];

int nmgr_cfg_req_pending(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return 0;
    nmgr_lock();
    int v = s_set_req[zone - 1].pending;
    nmgr_unlock();
    return v;
}

int nmgr_cfg_take_set_req(uint8_t *zone, hg_zone_cfg_t *out) {
    int taken = 0;
    nmgr_lock();
    for (uint8_t z = 1; z <= HG_MAX_ZONES && !taken; z++) {
        if (!s_set_req[z - 1].pending) continue;
        *out = s_set_req[z - 1].cfg;
        s_set_req[z - 1].pending = 0;
        *zone = z;
        taken = 1;
    }
    nmgr_unlock();
    return taken;
}

void nmgr_cfg_drop_set_req(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return;
    nmgr_lock();
    s_set_req[zone - 1].pending = 0;
    nmgr_unlock();
}

/* 0 = CFG cached (both planes unwrapped into the caller's structs; an absent
   HW plane is zeroed with *hw_gen 0), -1 = zone out of range or no CFG cache
   yet. cfg is required; hw / cfg_gen / hw_gen are optional. */
int node_mgr_cfg_get(uint8_t zone, hg_zone_cfg_t *cfg, hg_zone_hw_t *hw,
                     uint32_t *cfg_gen, uint32_t *hw_gen) {
    if (zone < 1 || zone > HG_MAX_ZONES || !cfg) return -1;

    /* one buffer for both planes in turn: CFG is the larger of the two, and a
       second nmgr_cache_t worth of stack matters in an httpd worker */
    uint8_t  blob[HG_BLOB_HDR_LEN + sizeof(hg_zone_cfg_t)];
    uint8_t  valid;
    uint32_t gen = 0;

    const nmgr_cache_t *c = nmgr_cfg_cache(zone, 1);
    nmgr_lock();
    valid = c->valid;
    if (valid) memcpy(blob, c->blob, sizeof blob);
    nmgr_unlock();
    if (!valid) return -1;
    hg_blob_rc_t rc = hg_blob_unwrap(HG_MAGIC_CFG, HG_CFG_VER, HG_CFG_VER_MIN, blob, sizeof blob,
                                      cfg, (uint16_t)sizeof *cfg, &gen);
    /* Should not fire -- we wrapped these bytes ourselves and copied them under
       the lock -- so a failure here means a writer skipped the lock or memory
       was corrupted. Refuse rather than hand the caller a half-valid config. */
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) return -1;
    if (cfg_gen) *cfg_gen = gen;

    if (hw_gen) *hw_gen = 0;
    if (!hw) return 0;
    const size_t hlen = HG_BLOB_HDR_LEN + sizeof(hg_zone_hw_t);
    const nmgr_cache_t *h = nmgr_cfg_cache(zone, 2);
    nmgr_lock();
    valid = h->valid;
    if (valid) memcpy(blob, h->blob, hlen);
    nmgr_unlock();
    gen = 0;
    rc = valid ? hg_blob_unwrap(HG_MAGIC_HW, HG_HW_VER, HG_HW_VER_MIN, blob, hlen,
                                 hw, (uint16_t)sizeof *hw, &gen)
               : HG_BLOB_E_SHORT;
    if (rc != HG_BLOB_OK && rc != HG_BLOB_MIGRATED) { memset(hw, 0, sizeof *hw); return 0; }
    if (hw_gen) *hw_gen = gen;
    return 0;   /* the CFG plane is what this call is about; HW is best-effort */
}

/* 1 while a write for this zone is queued or its push/pull is on the wire --
 * the HTTP layer's 409 gate, and what a UI polls to know when a save landed.
 * The two halves are read separately, so a caller sampling in the microsecond
 * between the tick consuming a request and its push starting can see 0; the
 * only consequence is that a second write is accepted instead of refused, and
 * it simply pushes again with a further gen bump (last write wins, which is
 * what a second save means anyway). */
int node_mgr_cfg_busy(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return 0;
    return (nmgr_cfg_req_pending(zone) || nmgr_cx_busy(zone)) ? 1 : 0;
}

/* 0 = queued for the tick, -1 = no such zone (out of range, or no row), -3 =
 * the zone is known but not ONLINE, -2 = a write or a transfer for this zone is
 * already in flight. The -1/-3 split is for Task 12: 404 for a zone that does
 * not exist, 409 for one that does but cannot be written right now.
 * Asynchronous by design: the caller is told "accepted", and
 * NOTIFY NODE <z> CFG_SYNC_FAILED / node_mgr_cfg_sync_failed report a zone
 * that then refused it. */
int node_mgr_cfg_set(uint8_t zone, const hg_zone_cfg_t *cfg) {
    if (zone < 1 || zone > HG_MAX_ZONES || !cfg) return -1;
    nmgr_lock();
    const hg_node_t *nd = nmgr_node_by_id(zone);
    int used   = nd && nd->used;
    int online = used && nd->health == NODE_H_ONLINE;
    nmgr_unlock();
    if (!used) return -1;
    if (!online) return -3;
    if (nmgr_cx_busy(zone)) return -2;
    int rc = -2;
    nmgr_lock();
    if (!s_set_req[zone - 1].pending) {   /* re-checked under the lock: two callers, one slot */
        s_set_req[zone - 1].cfg     = *cfg;
        s_set_req[zone - 1].pending = 1;
        rc = 0;
    }
    nmgr_unlock();
    return rc;
}
