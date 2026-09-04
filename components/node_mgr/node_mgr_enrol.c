#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "node_store.h"
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"

/* ztab ownership, HB-driven enrolment (spec §2.8), TIME_SYNC broadcast and
 * ring_health_eval's line formatting -- split out of node_mgr.c purely to
 * keep both files under the ~300-line cap (the natural "identity/time"
 * grouping, mirroring zone_ring_sync.c's split in Task 12).
 *
 * Locking: every function here that mutates s_ztab, a hg_node_t slot, or
 * s_unassigned_* runs under nmgr_lock() -- ztab and the node table are read
 * cross-task by node_mgr_get/set_name/clear (node_mgr.c) and by
 * node_mgr_forward (node_mgr_fwd.c), so the node_mgr task's own writes here
 * must take the same mutex those callers use (fix round important #3 /
 * minor #7). s_last_seq/s_seq_known are node_mgr-task-only (never read
 * cross-task) and stay unlocked. */

static ztab_t   s_ztab;
static uint16_t s_last_seq[HG_MAX_ZONES];      /* HB seq-gap accounting (ruling #6) */
static uint8_t  s_seq_known[HG_MAX_ZONES];

static uint8_t  s_unassigned_mac[HG_MAX_ZONES][6];  /* 0xFE heartbeaters while the table is full */
static int      s_unassigned_n;
static uint8_t  s_unassigned_notified;

void nmgr_enrol_boot_init(void) {
    if (node_store_load(&s_ztab) != 0) memset(&s_ztab, 0, sizeof s_ztab);
    memset(s_last_seq, 0, sizeof s_last_seq);
    memset(s_seq_known, 0, sizeof s_seq_known);
    uint32_t now = nmgr_now_ms();
    nmgr_lock();
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const ztab_ent_t *e = &s_ztab.e[i];
        if (!(e->flags & ZTAB_F_ASSIGNED) || e->id < 1 || e->id > HG_MAX_ZONES) continue;
        hg_node_t *nd = nmgr_node_by_id(e->id);
        nd->used = 1; nd->id = e->id;
        memcpy(nd->mac, e->mac, 6);
        memcpy(nd->name, e->name, sizeof nd->name);
        nd->unconfigured = (e->flags & ZTAB_F_UNCONFIGURED) ? 1 : 0;
        nd->last_hb_ms = now;   /* boot grace: a real OFFLINE alarm needs ~10 s of silence from here */
    }
    nmgr_unlock();
}

/* caller holds nmgr_lock() */
int nmgr_ztab_set_name(uint8_t zone, const char *name) {
    if (ztab_set_name(&s_ztab, zone, name) != 0) return -1;
    return node_store_save(&s_ztab);
}

/* caller holds nmgr_lock() */
int nmgr_ztab_clear(uint8_t zone) {
    if (ztab_clear(&s_ztab, zone) != 0) return -1;
    return node_store_save(&s_ztab);
}

int nmgr_unassigned_copy(uint8_t macs[][6], int cap) {
    nmgr_lock();
    int n = s_unassigned_n < cap ? s_unassigned_n : cap;
    for (int i = 0; i < n; i++) memcpy(macs[i], s_unassigned_mac[i], 6);
    nmgr_unlock();
    return n;
}

/* hg_assign_pack/hg_ts_pack are FIXED-size codecs: they return 0 for OK, not
 * a byte count like hg_hb_pack/hg_ack_pack do. Their payload length is the
 * sizeof of the buffer they filled -- passing the return value as the wire len
 * put len=0 on every ASSIGN_ID and TIME_SYNC, which the receiving zone's
 * hg_assign_parse/hg_ts_parse (both exact-length) then rejected in silence. */
static void send_assign(const uint8_t mac[6], uint8_t zone_id) {
    hg_assign_t a; memcpy(a.mac, mac, 6); a.zone_id = zone_id;
    uint8_t payload[HG_ASSIGN_LEN];
    if (hg_assign_pack(&a, payload) < 0) return;   /* minor #4: "< 0" reads correctly under both
                                                      pack conventions (0 = OK here, a length elsewhere) */
    nmgr_send_raw(RING_ID_UNASSIGNED, RING_T_ASSIGN_ID, payload, (uint8_t)sizeof payload);
}

static void mac_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* caller holds nmgr_lock() */
static void track_unassigned(const uint8_t mac[6]) {
    for (int i = 0; i < s_unassigned_n; i++)
        if (memcmp(s_unassigned_mac[i], mac, 6) == 0) return;
    if (s_unassigned_n < HG_MAX_ZONES) memcpy(s_unassigned_mac[s_unassigned_n++], mac, 6);
    if (!s_unassigned_notified) { s_unassigned_notified = 1; notify_emit(NTF_NODE, 0, "TABLE_FULL"); }
}

/* minor #9: a mac that later gets a real slot must stop showing up in
 * GET UNASSIGNED. caller holds nmgr_lock(). */
static void purge_unassigned(const uint8_t mac[6]) {
    for (int i = 0; i < s_unassigned_n; i++) {
        if (memcmp(s_unassigned_mac[i], mac, 6) == 0) {
            memcpy(s_unassigned_mac[i], s_unassigned_mac[s_unassigned_n - 1], 6);   /* swap-remove: order doesn't matter */
            s_unassigned_n--;
            return;
        }
    }
}

/* caller holds nmgr_lock() */
static void update_telemetry(hg_node_t *nd, uint8_t zone_id, const ring_hdr_t *hdr, const hg_hb_t *hb) {
    /* Measured at the master's RX: 0 = the zone feeding it (undecremented),
     * highest = the first hop after the master's TX. ring_health's blame names
     * the suspect leg from this, and needs hops_valid to tell "real last hop"
     * from "never heard" (both would read 0). */
    nd->hops         = (uint8_t)(RING_TTL_INIT - hdr->ttl);
    nd->hops_valid   = 1;
    nd->link_flags   = hb->link_flags;
    nd->fault_flag   = hb->active_faults != 0;
    nd->cmd_timeouts = 0;                          /* any HB clears forward-failure accounting */
    nd->last_hb_ms   = nmgr_now_ms();

    /* seq-gap accounting (ruling #6 / minor #4): fold into the RAM-only,
     * monotonically-accumulating seq_drop_tally BEFORE the hb copy below --
     * unlike the old rx_drop-splicing approach, this field is never
     * overwritten by the wire hb, so it survives across heartbeats instead
     * of resetting every 2 s. The zone stamps heartbeats from a counter of
     * their own (zone_ring.c's s_hb_seq), so this measures HEARTBEAT loss and
     * nothing else.
     *
     * A zone reboot restarts that counter at 1 while our baseline still holds
     * the pre-reboot value: past 32768 heartbeats the backwards step reads as a
     * ~25 k forward jump and would book ~25 k bogus drops. hb.uptime_s is
     * monotonic within a boot, so a REGRESSION is the reboot signal -- drop the
     * baseline and re-learn it from this heartbeat. */
    uint8_t idx = (uint8_t)(zone_id - 1);
    if (hb->uptime_s < nd->hb.uptime_s) s_seq_known[idx] = 0;
    if (s_seq_known[idx]) {
        uint16_t delta = (uint16_t)(hdr->seq - s_last_seq[idx]);
        if (delta > 1 && delta < 32768u) nd->seq_drop_tally += (uint32_t)(delta - 1);
    }
    s_last_seq[idx] = hdr->seq;
    s_seq_known[idx] = 1;

    nd->hb = *hb;
    nmgr_cfg_note_fresh_hb(zone_id);   /* important #2: gate the reconciler to fresh-HB edges only */
}

void nmgr_enrol_handle_hb(const ring_frame_t *f) {
    hg_hb_t hb;
    if (hg_hb_parse(f->payload, f->hdr.len, &hb) != 0) return;

    uint8_t out_id = 0;
    nmgr_lock();
    ztab_en_t v = ztab_enrol(&s_ztab, hb.mac, f->hdr.src, &out_id);
    if (v == ZTAB_EN_FULL) {
        track_unassigned(hb.mac);
        nmgr_unlock();
        return;
    }
    if (v == ZTAB_EN_CONFLICT) {
        notify_emit(NTF_NODE, out_id, "%u ID_CONFLICT", out_id);
        nmgr_unlock();
        send_assign(hb.mac, RING_ID_UNASSIGNED);       /* intruder reset to 0xFE (spec §2.8) */
        return;                                        /* the real owner's row is untouched */
    }

    hg_node_t *nd = nmgr_node_by_id(out_id);
    if (!nd) { nmgr_unlock(); return; }                 /* defensive: out_id always 1..8 above */

    if (v == ZTAB_EN_ASSIGNED) {
        purge_unassigned(hb.mac);
        node_store_save(&s_ztab);
        memset(nd, 0, sizeof *nd);
        s_seq_known[out_id - 1] = 0;   /* a different board on this id: its counter is unrelated */
        nd->used = 1; nd->id = out_id; nd->unconfigured = 1;
        char ms[18]; mac_str(hb.mac, ms);
        notify_emit(NTF_NODE, out_id, "%u NEW %s", out_id, ms);
    }
    update_telemetry(nd, out_id, &f->hdr, &hb);
    nmgr_unlock();

    /* Assert the id ONLY when the sender isn't already using it. Re-asserting
     * on every heartbeat is what closed the HB<->ASSIGN_ID feedback loop once
     * 2469706 made ASSIGN_ID parseable again: the zone answered each ASSIGN_ID
     * with an immediate heartbeat, which drew the next ASSIGN_ID, at wire
     * speed (measured: 205 forwards per 10 s vs ~15 nominal). A zone whose
     * hdr.src already equals the id the ztab resolved needs no assertion;
     * unassigned (0xFE) and stale-id senders still get one, which is every
     * case that has anything to correct. Spec §2.8. */
    if (f->hdr.src != out_id) send_assign(hb.mac, out_id);
}

void nmgr_broadcast_time_sync(uint32_t now) {
    hg_ts_t t = { 0 };
    t.utc          = (uint32_t)time(NULL);
    t.utc_offset_s = 0;
    t.flags        = node_mgr_time_valid() ? 0x01 : 0x00;
    t.ring_size    = nmgr_ring_size();
    nmgr_lock();
    t.online_mask  = ring_online_mask(nmgr_table(), HG_MAX_ZONES, now);
    nmgr_unlock();
    t.inhibit_mask = 0;   /* SP5 */
    uint8_t payload[HG_TS_LEN];
    if (hg_ts_pack(&t, payload) < 0) return;    /* fixed-size codec: 0 = OK, see send_assign */
    nmgr_send_raw(RING_ID_BCAST, RING_T_TIME_SYNC, payload, (uint8_t)sizeof payload);
}

/* ring_health_eval's callback text is already "NODE %u %s" / "RING OPEN %s"
 * / "RING CLOSED" (spec §2.7) -- strip the leading type word it already
 * carries so notify_emit's own type-name prefix isn't duplicated. See the
 * design note in node_mgr.c's NOTIFY passthrough for why the master's own
 * id (0) still shows up ahead of the real zone id in the final line. Called
 * from node_mgr.c's task loop while nmgr_lock() is already held (it's
 * ring_health_eval's own callback, invoked synchronously from inside that
 * locked call). */
void nmgr_health_cb(void *ctx, const char *line) {
    (void)ctx;
    if (strncmp(line, "NODE ", 5) == 0) {
        long id = strtol(line + 5, NULL, 10);
        notify_emit(NTF_NODE, (uint8_t)id, "%s", line + 5);
    } else if (strncmp(line, "RING ", 5) == 0) {
        notify_emit(NTF_RING, 0, "%s", line + 5);
    }
}
