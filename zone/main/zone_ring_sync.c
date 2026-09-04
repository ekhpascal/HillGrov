#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "app_if_common.h"
#include "ring_link.h"
#include "hg_store.h"
#include "notify.h"
#include "zone_ring_internal.h"

/* TIME_SYNC / ASSIGN_ID handling, zone link-state (W_LINK_LOST), and the SP2
 * inhibit-mask primitive -- split out of zone_ring.c per Task 12's line cap
 * (the natural TIME_SYNC/ASSIGN_ID/link-state/inhibit group). */

#define ZRING_LINK_MS 6000u   /* spec 2.7: zone link state LOST after 6000 ms without a frame */

static const char *TAG = "zsync";

static cmd_core_t *s_core;

static uint32_t s_last_rx_ms;                  /* any frame: proves the upstream leg is alive */
static uint32_t s_last_master_ms;               /* master-sourced frame only */
static uint8_t  s_link_lost;                    /* W_LINK_LOST edge latch, re-armed on recovery */

static uint8_t  s_ring_size;                    /* last TIME_SYNC ring_size (stored; not yet reported anywhere) */
static uint16_t s_online_mask;                  /* last TIME_SYNC (SP3: stores + reports in HB only) */
static uint8_t  s_inhibit_mask;
static uint32_t s_inhibit_ms;                   /* age-out clock for s_inhibit_mask, spec 2.7 (600 s) */
static uint8_t  s_time_synced;                  /* sticky: a valid ring TIME_SYNC has been received */
static uint32_t s_time_synced_at;               /* UPTIME seconds at that moment (never wall clock -- see
                                                   zsync_time_sync), for GET TIME's source token */

static uint32_t now_ms(void) { return s_core->now_ms(); }

void zone_ring_sync_init(cmd_core_t *core) { s_core = core; }

void zsync_note_rx(uint32_t now)     { s_last_rx_ms = now; }
void zsync_note_master(uint32_t now) { s_last_master_ms = now; }

void zsync_time_sync(const ring_frame_t *f, uint32_t now) {
    hg_ts_t t;
    if (hg_ts_parse(f->payload, f->hdr.len, &t) != 0) return;
    if (t.flags & 0x01) {                                   /* b0 time_valid */
        s_time_synced = 1;   /* latch on receipt, regardless of |delta| -- the master has
                               * already vouched for this time; the step below is only
                               * about whether OUR clock needs correcting to match it */
        int64_t delta = (int64_t)t.utc - (int64_t)time(NULL);
        if (delta < 0) delta = -delta;
        if (delta > 2) {
            struct timeval tv = { .tv_sec = (time_t)t.utc, .tv_usec = 0 };
            settimeofday(&tv, NULL);
        }
        s_time_synced_at = hg_app_uptime_s();   /* uptime, not wall clock: GET TIME compares this
                                                   against the local SET TIME stamp to pick the
                                                   more recent source, and a step must not make
                                                   the stepping source look older */
    }
    s_ring_size     = t.ring_size;
    s_online_mask   = t.online_mask;
    s_inhibit_mask  = t.inhibit_mask;
    s_inhibit_ms    = now;
}

void zsync_assign_id(const ring_frame_t *f) {
    hg_assign_t a;
    if (hg_assign_parse(f->payload, f->hdr.len, &a) != 0) return;
    /* Defence in depth: ring_route already routes 0xFE-addressed ASSIGN_IDs by
     * MAC, so a frame naming someone else's MAC reaching this handler means the
     * routing layer is broken -- say so loudly rather than adopting the id. */
    uint8_t mac[6];
    hg_app_get_mac(mac);
    if (memcmp(a.mac, mac, 6) != 0) {
        ESP_LOGW(TAG, "ASSIGN_ID for %02x:%02x:%02x:%02x:%02x:%02x ignored (not ours)",
                 a.mac[0], a.mac[1], a.mac[2], a.mac[3], a.mac[4], a.mac[5]);
        return;
    }
    /* hg_store_set_zid() takes any byte: 0x00 (the master's own id) or 0xFF
     * (broadcast) would poison routing for good, since the id is persisted
     * and reloaded on every boot. Only 1..HG_MAX_ZONES and the "no id yet"
     * sentinel are legal on the wire (spec 2.8). */
    if (!((a.zone_id >= 1 && a.zone_id <= HG_MAX_ZONES) || a.zone_id == RING_ID_UNASSIGNED)) {
        ESP_LOGW(TAG, "ASSIGN_ID with illegal zone_id %u ignored", a.zone_id);
        return;
    }
    /* Re-assert of the id we already hold: a genuine no-op. Persisting it
     * again and asking for an immediate heartbeat is what let a master that
     * re-asserts per heartbeat run the pair at wire speed -- the zone must
     * not answer an assignment that changes nothing. */
    if (a.zone_id == hg_store_zid()) return;

    hg_store_set_zid(a.zone_id);
    if (s_core) s_core->zone_id = a.zone_id;   /* live: cmd_dispatch shares this exact struct with cmd_task */
    notify_set_node_id(a.zone_id);
    zring_request_immediate_hb();               /* confirmed by the next heartbeat */
}

void zsync_link_tick(uint32_t now) {
    uint8_t lost = s_last_master_ms != 0 && (now - s_last_master_ms) >= ZRING_LINK_MS;
    if (lost && !s_link_lost) {
        notify_emit(NTF_RING, 0, "W_LINK_LOST master silent %lu s", (unsigned long)((now - s_last_master_ms) / 1000));
        s_link_lost = 1;
    } else if (!lost && s_link_lost) {
        s_link_lost = 0;                                    /* re-armed: next 6000 ms loss fires again */
    }
}

/* SYNCED once any valid (time_valid) ring TIME_SYNC has been received this
 * boot (sticky, latched on receipt regardless of whether a step was needed
 * -- see zsync_time_sync); else COARSE if the wall clock looks plausibly
 * set by some other local means (console SET TIME -- app_if_common tracks
 * no exposed flag for this, so a post-2020 clock value is the signal); else
 * NONE. */
uint8_t zsync_time_quality(void) {
    if (s_time_synced) return 2;
    return time(NULL) > 1577836800 ? 1 : 0;
}

/* see zone_ring.h */
uint32_t zone_ring_time_synced_at(void) { return s_time_synced_at; }

uint8_t zsync_link_flags(uint32_t now, uint8_t zid) {
    uint8_t link = 0;
    if (s_last_rx_ms     != 0 && (now - s_last_rx_ms)     < ZRING_LINK_MS) link |= 0x01;  /* upstream_alive */
    if (s_last_master_ms != 0 && (now - s_last_master_ms) < ZRING_LINK_MS) link |= 0x02;  /* master_alive */
    if (zid >= 1 && zid <= HG_MAX_ZONES && (s_online_mask & (1u << zid))) link |= 0x04;   /* heard_by_master */
    return link;
}

/* SP2 pump-gate primitive -- see zone_ring.h. Guarded against a call before
 * zone_ring_start() has run (s_core still NULL, e.g. an SP2 task racing
 * boot order): returns 0 rather than dereferencing a NULL core, which is
 * also the correct answer since nothing has been received yet either way. */
uint8_t zone_ring_inhibit_mask(void) {
    if (!s_core) return 0;
    uint32_t now = now_ms();
    if (s_inhibit_ms == 0 || (now - s_inhibit_ms) > 600000u) return 0;   /* never set, or stale */
    return s_inhibit_mask;
}
