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
 * grouping, mirroring zone_ring_sync.c's split in Task 12). */

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
}

int nmgr_ztab_set_name(uint8_t zone, const char *name) {
    if (ztab_set_name(&s_ztab, zone, name) != 0) return -1;
    return node_store_save(&s_ztab);
}

int nmgr_ztab_clear(uint8_t zone) {
    if (ztab_clear(&s_ztab, zone) != 0) return -1;
    return node_store_save(&s_ztab);
}

int nmgr_unassigned_copy(uint8_t macs[][6], int cap) {
    int n = s_unassigned_n < cap ? s_unassigned_n : cap;
    for (int i = 0; i < n; i++) memcpy(macs[i], s_unassigned_mac[i], 6);
    return n;
}

static void send_assign(const uint8_t mac[6], uint8_t zone_id) {
    hg_assign_t a; memcpy(a.mac, mac, 6); a.zone_id = zone_id;
    uint8_t payload[7];
    int n = hg_assign_pack(&a, payload);
    if (n >= 0) nmgr_send_raw(RING_ID_UNASSIGNED, RING_T_ASSIGN_ID, payload, (uint8_t)n);
}

static void mac_str(const uint8_t mac[6], char out[18]) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void track_unassigned(const uint8_t mac[6]) {
    for (int i = 0; i < s_unassigned_n; i++)
        if (memcmp(s_unassigned_mac[i], mac, 6) == 0) return;
    if (s_unassigned_n < HG_MAX_ZONES) memcpy(s_unassigned_mac[s_unassigned_n++], mac, 6);
    if (!s_unassigned_notified) { s_unassigned_notified = 1; notify_emit(NTF_NODE, 0, "TABLE_FULL"); }
}

static void update_telemetry(hg_node_t *nd, uint8_t zone_id, const ring_hdr_t *hdr, const hg_hb_t *hb) {
    nd->hops         = (uint8_t)(RING_TTL_INIT - hdr->ttl);
    nd->link_flags   = hb->link_flags;
    nd->fault_flag   = hb->active_faults != 0;
    nd->cmd_timeouts = 0;                          /* any HB clears forward-failure accounting */
    nd->last_hb_ms   = nmgr_now_ms();
    nd->hb           = *hb;

    /* a forward jump of hdr.seq beyond +1 folds the gap into this HB's own
     * rx_drop tally (ruling #6); a backward/huge jump (zone reboot resetting
     * its tx_seq) is not a "gap" over the ring -- just resync quietly. */
    uint8_t idx = (uint8_t)(zone_id - 1);
    if (s_seq_known[idx]) {
        uint16_t delta = (uint16_t)(hdr->seq - s_last_seq[idx]);
        if (delta > 1 && delta < 32768u) nd->hb.rx_drop = (uint16_t)(nd->hb.rx_drop + (delta - 1));
    }
    s_last_seq[idx] = hdr->seq;
    s_seq_known[idx] = 1;
}

void nmgr_enrol_handle_hb(const ring_frame_t *f) {
    hg_hb_t hb;
    if (hg_hb_parse(f->payload, f->hdr.len, &hb) != 0) return;

    uint8_t out_id = 0;
    ztab_en_t v = ztab_enrol(&s_ztab, hb.mac, f->hdr.src, &out_id);
    if (v == ZTAB_EN_FULL) { track_unassigned(hb.mac); return; }
    if (v == ZTAB_EN_CONFLICT) {
        send_assign(hb.mac, RING_ID_UNASSIGNED);       /* intruder reset to 0xFE (spec §2.8) */
        notify_emit(NTF_NODE, out_id, "%u ID_CONFLICT", out_id);
        return;                                        /* the real owner's row is untouched */
    }

    hg_node_t *nd = nmgr_node_by_id(out_id);
    if (!nd) return;                                    /* defensive: out_id always 1..8 above */

    if (v == ZTAB_EN_ASSIGNED) {
        node_store_save(&s_ztab);
        memset(nd, 0, sizeof *nd);
        nd->used = 1; nd->id = out_id; nd->unconfigured = 1;
        char ms[18]; mac_str(hb.mac, ms);
        notify_emit(NTF_NODE, out_id, "%u NEW %s", out_id, ms);
    }
    send_assign(hb.mac, out_id);   /* KNOWN/STALE/ASSIGNED all (re)assert the same id, spec §2.8 */
    update_telemetry(nd, out_id, &f->hdr, &hb);
}

void nmgr_broadcast_time_sync(uint32_t now) {
    hg_ts_t t = { 0 };
    t.utc          = (uint32_t)time(NULL);
    t.utc_offset_s = 0;
    t.flags        = node_mgr_time_valid() ? 0x01 : 0x00;
    t.ring_size    = nmgr_ring_size();
    t.online_mask  = ring_online_mask(nmgr_table(), HG_MAX_ZONES, now);
    t.inhibit_mask = 0;   /* SP5 */
    uint8_t payload[13];
    int n = hg_ts_pack(&t, payload);
    if (n >= 0) nmgr_send_raw(RING_ID_BCAST, RING_T_TIME_SYNC, payload, (uint8_t)n);
}

/* ring_health_eval's callback text is already "NODE %u %s" / "RING OPEN %s"
 * / "RING CLOSED" (spec §2.7) -- strip the leading type word it already
 * carries so notify_emit's own type-name prefix isn't duplicated. See the
 * design note in node_mgr.c's NOTIFY passthrough for why the master's own
 * id (0) still shows up ahead of the real zone id in the final line. */
void nmgr_health_cb(void *ctx, const char *line) {
    (void)ctx;
    if (strncmp(line, "NODE ", 5) == 0) {
        long id = strtol(line + 5, NULL, 10);
        notify_emit(NTF_NODE, (uint8_t)id, "%s", line + 5);
    } else if (strncmp(line, "RING ", 5) == 0) {
        notify_emit(NTF_RING, 0, "%s", line + 5);
    }
}
