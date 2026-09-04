#include <string.h>
#include <stdio.h>
#include "ring_proto.h"       /* HG_MAX_ZONES only -- no IDF headers (see node_mgr_internal.h) */
#include "node_mgr_internal.h"

/* Pure fleet OTA sequencer core (Task 15). See node_mgr_internal.h for the
 * full contract; node_mgr_fleet.c is the impure glue that drives this from
 * node_mgr.c's task loop and performs the actions this returns. */

#define FLEET_HOLD_MS 180000u

void fleet_init(fleet_t *s) { memset(s, 0, sizeof *s); }

int fleet_start(fleet_t *s, const uint8_t *zones, uint8_t n) {
    if (s->active || n == 0 || n > HG_MAX_ZONES) return -1;
    memset(s, 0, sizeof *s);
    memcpy(s->zones, zones, n);
    s->n = n;
    s->active = 1;
    s->st = FZS_PRECHECK;
    return 0;
}

void fleet_cancel(fleet_t *s) {
    if (!s->active) return;
    if (s->st == FZS_PRECHECK) s->active = 0;   /* nothing sent yet: stop right away */
    else s->cancel_req = 1;                      /* in-flight zone completes on its own */
}

static uint8_t cur_zone(const fleet_t *s) { return s->zones[s->idx]; }

/* Called after a zone resolves (FA_DONE/FA_FAILED): failure or a latched
   cancel always stops the run; otherwise advance to the next zone. */
static void advance(fleet_t *s, int failed) {
    if (failed || s->cancel_req || (uint8_t)(s->idx + 1) >= s->n) { s->active = 0; return; }
    s->idx++;
    s->st = FZS_PRECHECK;
}

int fleet_tick(fleet_t *s, uint32_t now_ms, int image_ok, int online,
               int hb_valid, const uint8_t hb_fw[3], uint32_t hb_uptime, uint32_t hb_cfg_gen,
               fleet_act_t *out) {
    if (!s->active) return 0;

    if (s->st == FZS_PRECHECK) {
        uint8_t z = cur_zone(s);
        if (!image_ok || !online) {
            out->kind = FA_FAILED; out->zone = z;
            advance(s, 1);
            return 1;
        }
        s->st = FZS_WAIT_ACK;
        out->kind = FA_START; out->zone = z;
        return 1;
    }

    if (s->st == FZS_WAIT_HB) {
        uint8_t z = cur_zone(s);
        int fw_changed = hb_valid && (hb_fw[0] != s->pre_fw[0] || hb_fw[1] != s->pre_fw[1] || hb_fw[2] != s->pre_fw[2]);
        int uptime_reset_intact = hb_valid && hb_uptime < s->pre_uptime && hb_cfg_gen == s->pre_cfg_gen;
        if (fw_changed || uptime_reset_intact) {
            out->kind = FA_DONE; out->zone = z;
            out->fw[0] = hb_fw[0]; out->fw[1] = hb_fw[1]; out->fw[2] = hb_fw[2];
            advance(s, 0);
            return 1;
        }
        if ((int32_t)(now_ms - s->deadline_ms) >= 0) {
            out->kind = FA_FAILED; out->zone = z;
            advance(s, 1);
            return 1;
        }
    }

    return 0;   /* FZS_WAIT_ACK: nothing to report here, see fleet_on_ack */
}

int fleet_on_ack(fleet_t *s, uint16_t seq, int ok, fleet_act_t *out) {
    if (!s->active || s->st != FZS_WAIT_ACK || seq != s->seq) return 0;
    if (!ok) {
        out->kind = FA_FAILED; out->zone = cur_zone(s);
        advance(s, 1);
        return 1;
    }
    s->st = FZS_WAIT_HB;
    return 0;   /* ack alone isn't a reportable action -- WAIT_HB starts silently */
}

int fleet_note_submitted(fleet_t *s, int submitted, uint16_t seq, uint32_t now_ms,
                          const uint8_t pre_fw[3], uint32_t pre_uptime, uint32_t pre_cfg_gen,
                          fleet_act_t *out) {
    if (!s->active || s->st != FZS_WAIT_ACK) return 0;
    if (!submitted) {
        out->kind = FA_FAILED; out->zone = cur_zone(s);
        advance(s, 1);
        return 1;
    }
    s->seq = seq;
    s->deadline_ms = now_ms + FLEET_HOLD_MS;
    memcpy(s->pre_fw, pre_fw, 3);
    s->pre_uptime = pre_uptime;
    s->pre_cfg_gen = pre_cfg_gen;
    return 0;
}

void fleet_status(const fleet_t *s, char *buf, size_t cap) {
    if (!s->active) { snprintf(buf, cap, "IDLE"); return; }
    const char *phase = s->st == FZS_PRECHECK ? "PRECHECK" : s->st == FZS_WAIT_ACK ? "UPDATING" : "WAIT_HB";
    snprintf(buf, cap, "ZONE %u %s", (unsigned)cur_zone(s), phase);
}
