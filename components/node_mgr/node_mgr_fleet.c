#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ring_proto.h"
#include "notify.h"
#include "fw_srv.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"

/* Fleet OTA sequencer glue (Task 15): drives fleet_seq.c's pure state
 * machine from node_mgr.c's task loop (1 Hz tick + tracker events, the
 * same wiring nmgr_cfg_tick_1s/nmgr_cfg_on_ev use) and performs its
 * actions -- mark_updating, the tracked FW_UPDATE submit, and the three
 * NOTIFY FW lines the brief pins verbatim: "ZONE <z> UPDATING",
 * "ZONE <z> DONE <ver>", "ZONE <z> UPDATE_FAILED".
 *
 * s_fleet is touched from two tasks -- node_mgr (tick/ACK routing below)
 * and cmd_task (node_mgr_fw_zone/all/abort/status, reached via
 * master_cmds.c) -- so every access goes through s_fleet_mux. Held only
 * around the pure fleet_seq.c calls themselves, never across notify_emit()/
 * nmgr_submit()/nmgr_lock() (node_mgr.h's own table lock), so it never
 * nests with those. */

#define FLEET_HOLD_MS   180000u
#define FLEET_REBOOT_MS 500u

static fleet_t           s_fleet;
static SemaphoreHandle_t s_fleet_mux;

static void flock(void)   { if (s_fleet_mux) xSemaphoreTake(s_fleet_mux, portMAX_DELAY); }
static void funlock(void) { if (s_fleet_mux) xSemaphoreGive(s_fleet_mux); }

void nmgr_fleet_init(void) { s_fleet_mux = xSemaphoreCreateMutex(); }

/* nmgr_lock()'d snapshot of one zone for the pure core's PRECHECK/WAIT_HB
 * judging (same tolerance node_mgr_get() uses: a torn multi-field read is
 * never observed, a one-tick-stale value is the accepted cost). used=0
 * (unassigned/cleared) reads back online=0, hb_valid=0 -- a zone cleared
 * mid-sequence can never look like a false success. */
static void snap_zone(uint8_t zone, int *used, int *online, uint8_t fw[3], uint32_t *uptime, uint32_t *cfg_gen) {
    nmgr_lock();
    hg_node_t *nd = nmgr_node_by_id(zone);
    *used = nd && nd->used;
    *online = *used && nd->health == NODE_H_ONLINE;
    if (*used) {
        fw[0] = nd->hb.fw_maj; fw[1] = nd->hb.fw_min; fw[2] = nd->hb.fw_patch;
        *uptime = nd->hb.uptime_s;
        *cfg_gen = nd->hb.cfg_gen;
    } else {
        fw[0] = fw[1] = fw[2] = 0; *uptime = 0; *cfg_gen = 0;
    }
    nmgr_unlock();
}

static void notify_failed(uint8_t zone) { notify_emit(NTF_FW, zone, "ZONE %u UPDATE_FAILED", (unsigned)zone); }

/* Executes an FA_START action: mark_updating + NOTIFY UPDATING, pack+submit
   the tracked FW_UPDATE, then report the outcome back into the pure core.
   Runs on the node_mgr task, triggered from nmgr_fleet_tick_1s below. */
static void do_start(uint8_t zone) {
    int used, online; uint8_t fw[3]; uint32_t uptime, cfg_gen;
    snap_zone(zone, &used, &online, fw, &uptime, &cfg_gen);

    node_mgr_mark_updating(zone, FLEET_HOLD_MS);
    notify_emit(NTF_FW, zone, "ZONE %u UPDATING", (unsigned)zone);

    hg_fwu_t req;
    memset(&req, 0, sizeof req);
    req.reboot_delay_ms = FLEET_REBOOT_MS;
    snprintf(req.ssid, sizeof req.ssid, "HillGrow");
    snprintf(req.pass, sizeof req.pass, "hillgrow1");
    uint8_t payload[99];
    int n = hg_fwu_pack(&req, payload, sizeof payload);
    uint16_t seq = 0;
    int ok = n >= 0 && nmgr_submit(zone, RING_T_FW_UPDATE, payload, (uint8_t)n, &seq) == 0;

    fleet_act_t act;
    flock();
    int have = fleet_note_submitted(&s_fleet, ok, seq, nmgr_now_ms(), fw, uptime, cfg_gen, &act);
    funlock();
    if (have && act.kind == FA_FAILED) notify_failed(act.zone);
}

void nmgr_fleet_tick_1s(uint32_t now) {
    flock();
    uint8_t active = s_fleet.active;
    uint8_t zone = active ? s_fleet.zones[s_fleet.idx] : 0;
    funlock();
    if (!active) return;

    int image_ok = fw_srv_image_ok();
    int used, online; uint8_t fw[3]; uint32_t uptime, cfg_gen;
    snap_zone(zone, &used, &online, fw, &uptime, &cfg_gen);

    fleet_act_t act;
    flock();
    int have = fleet_tick(&s_fleet, now, image_ok, online, used, fw, uptime, cfg_gen, &act);
    funlock();
    if (!have) return;

    switch (act.kind) {
    case FA_START:  do_start(act.zone); break;
    case FA_DONE:   notify_emit(NTF_FW, act.zone, "ZONE %u DONE %u.%u.%u", (unsigned)act.zone,
                                 (unsigned)act.fw[0], (unsigned)act.fw[1], (unsigned)act.fw[2]); break;
    case FA_FAILED: notify_failed(act.zone); break;
    default: break;
    }
}

void nmgr_fleet_on_ev(const ring_trk_ev_t *ev) {
    int ok = ev->kind == RING_TRK_EV_DONE && ev->status == 0;
    fleet_act_t act;
    flock();
    int have = fleet_on_ack(&s_fleet, ev->seq, ok, &act);
    funlock();
    if (have && act.kind == FA_FAILED) notify_failed(act.zone);
}

int node_mgr_fw_zone(uint8_t zone) {
    if (zone < 1 || zone > HG_MAX_ZONES) return -1;
    uint8_t zones[1] = { zone };
    flock();
    int rc = fleet_start(&s_fleet, zones, 1);
    funlock();
    return rc;
}

int node_mgr_fw_all(void) {
    uint8_t zones[HG_MAX_ZONES];
    uint8_t n = 0;
    nmgr_lock();
    for (uint8_t id = 1; id <= HG_MAX_ZONES; id++) {
        hg_node_t *nd = nmgr_node_by_id(id);
        if (nd && nd->used) zones[n++] = id;
    }
    nmgr_unlock();
    if (n == 0) return -1;
    flock();
    int rc = fleet_start(&s_fleet, zones, n);
    funlock();
    return rc;
}

int node_mgr_fw_abort(void) {
    flock();
    int active = s_fleet.active;
    if (active) fleet_cancel(&s_fleet);
    funlock();
    return active ? 0 : -1;
}

int node_mgr_fw_status(char *buf, size_t n) {
    flock();
    fleet_status(&s_fleet, buf, n);
    funlock();
    return 0;
}
