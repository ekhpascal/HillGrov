#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"

/* ZONE-prefix CLI forward path (design doc "Master CLI forwarding path"):
 * ONE in-flight forward at a time -- a static slot guarded by a busy mutex
 * (ruling #10). A second concurrent caller never blocks on the ring at all;
 * it fails fast with BUSY the instant it can't claim the mutex. The claiming
 * caller holds the mutex for the whole round trip (submit -> wait -> done),
 * which is also what makes "one seq at a time" safe to match against.
 *
 * The wait/complete handoff between this (a foreign task -- cmd_task) and
 * nmgr_fwd_on_ev() (the node_mgr task) mirrors cmd_task.c's own
 * waiter/done/critical-section pattern exactly, including its timeout race
 * fix: a completion landing in the same instant as our wait times out is
 * still picked up correctly instead of being reported as a false timeout. */

#define FWD_TIMEOUT_MAX_MS 5000u   /* minor #8: bounds the caller's worst-case block; see node_mgr.h */

static SemaphoreHandle_t s_busy;         /* claimed for the whole call; try-take = BUSY */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t  s_active;       /* a seq is outstanding, waiting for nmgr_fwd_on_ev */
static volatile uint8_t  s_done;         /* nmgr_fwd_on_ev has written the result */
static TaskHandle_t      s_waiter;       /* NULL once the caller has given up (post-timeout) */
static uint16_t          s_seq;
static uint8_t           s_status;
static char              s_detail[126];
static const char       *s_fail_token;   /* NULL = EV_DONE, else EV_FAIL's token */
static uint8_t           s_zone;         /* the outstanding forward's target, for the §4.4 hook below */
static uint8_t           s_verb_set;     /* its line's first token was SET */

/* §4.4 bench ruling: an edit issued THROUGH the master is authoritative.
 * The zone applies a forwarded SET as its own LOCAL edit and bumps its gen,
 * so the reconciler would see "zone gen higher than my cache" and push the
 * master's now-stale copy straight back over it -- the operator's own command
 * answering OK and then undoing itself (bench: NOTIFY NODE 0 2 CFG_REVERTED).
 * Dropping the cache on the OK ACK turns that next decision into the adopt
 * branch instead, which pulls the value the operator just set. Only SET
 * qualifies: GET/DEBUG/CLEAR change no config, and a non-OK reply means the
 * zone rejected the edit and its config is unchanged. Cost when wrong is one
 * extra CFG_GET round trip. */
static int verb_is_set(const char *line) {
    while (*line == ' ') line++;
    return (line[0] == 'S' || line[0] == 's') && (line[1] == 'E' || line[1] == 'e') &&
           (line[2] == 'T' || line[2] == 't') && (line[3] == ' ' || line[3] == '\0');
}

/* the zone's reply line, verbatim from the ACK detail: "OK" or "OK <rest>" */
static int reply_is_ok(const char *detail) {
    return detail[0] == 'O' && detail[1] == 'K' &&
           (detail[2] == '\0' || detail[2] == ' ' || detail[2] == '\n');
}

void nmgr_fwd_init(void) { s_busy = xSemaphoreCreateMutex(); }

void nmgr_fwd_on_ev(const ring_trk_ev_t *ev) {
    uint8_t adopt_zone = 0;
    taskENTER_CRITICAL(&s_mux);
    if (!s_active || ev->seq != s_seq) { taskEXIT_CRITICAL(&s_mux); return; }   /* stale/foreign seq */
    s_active = 0;
    s_done = 1;
    if (ev->kind == RING_TRK_EV_DONE) {
        s_status = ev->status;
        snprintf(s_detail, sizeof s_detail, "%s", ev->detail);
        s_fail_token = NULL;
        if (s_verb_set && ev->status == 0 && reply_is_ok(ev->detail)) adopt_zone = s_zone;
    } else {
        s_fail_token = ev->fail_token;   /* "ZONE_TIMEOUT" | "ZONE_UNKNOWN" */
    }
    TaskHandle_t w = s_waiter;
    taskEXIT_CRITICAL(&s_mux);
    if (w) xTaskNotifyGive(w);
    /* This hook runs on the node_mgr task (node_mgr.c's handle_ack ->
     * route_trk_ev), which is node_mgr_cfg's single writer, so the internal
     * is called directly rather than queued through a request flag. It also
     * makes the ordering exact: the ACK is on the wire ahead of the zone's
     * gen-bumped heartbeat and both are consumed by this one task, so the
     * cache is already gone before nmgr_cfg_tick_1s takes its next decision. */
    if (adopt_zone) nmgr_cfg_invalidate(adopt_zone);
}

static int errline(char *resp, int len, const char *token) {
    snprintf(resp, (size_t)len, "ERR %s\n", token);
    return -1;
}

/* runs with s_busy already held */
static int do_forward(uint8_t zone, const char *line, char *resp, int resp_len, uint32_t timeout_ms) {
    if (zone < 1 || zone > HG_MAX_ZONES) return errline(resp, resp_len, "ZONE_UNKNOWN");
    hg_node_t *nd = nmgr_node_by_id(zone);
    /* .used / .health: unlocked single-word reads, same tolerance as
     * node_mgr_push_cfg's cache-valid check -- a stale read here just means
     * an occasional wrong-but-safe verdict (ZONE_UNKNOWN/ZONE_OFFLINE vs. a
     * real attempt); it is never a torn multi-field read. */
    if (!nd || !nd->used) return errline(resp, resp_len, "ZONE_UNKNOWN");
    if (nd->health == NODE_H_OFFLINE) return errline(resp, resp_len, "ZONE_OFFLINE");
    ring_status_t rs;
    node_mgr_ring_status(&rs);
    if (rs.state == RING_ST_OPEN) return errline(resp, resp_len, "RING_DOWN");

    size_t n = strlen(line);
    if (n > RING_MAX_PAYLOAD) n = RING_MAX_PAYLOAD;
    uint16_t seq;
    if (nmgr_submit(zone, RING_T_CMD, (const uint8_t *)line, (uint8_t)n, &seq) != 0)
        return errline(resp, resp_len, "BUSY");

    taskENTER_CRITICAL(&s_mux);
    s_seq = seq; s_active = 1; s_done = 0; s_fail_token = NULL;
    s_zone = zone; s_verb_set = (uint8_t)verb_is_set(line);
    s_waiter = xTaskGetCurrentTaskHandle();
    taskEXIT_CRITICAL(&s_mux);

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        taskENTER_CRITICAL(&s_mux);
        if (!s_done) {
            s_waiter = NULL;             /* orphan: nmgr_fwd_on_ev sees s_active clear and drops it */
            s_active = 0;
            taskEXIT_CRITICAL(&s_mux);
            nmgr_lock(); nd->cmd_timeouts++; nmgr_unlock();   /* DEGRADED via cmd_timeouts fed by forward failures */
            return errline(resp, resp_len, "ZONE_TIMEOUT");
        }
        taskEXIT_CRITICAL(&s_mux);
        /* done was already set: nmgr_fwd_on_ev read a non-NULL waiter under
         * the same critical section and has committed to (or already did)
         * give our notification -- it is guaranteed imminent. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }

    if (s_fail_token) {
        nmgr_lock(); nd->cmd_timeouts++; nmgr_unlock();
        return errline(resp, resp_len, s_fail_token);
    }
    snprintf(resp, resp_len, "%s", s_detail);
    return s_status == 0 ? 0 : -1;
}

int node_mgr_forward(uint8_t zone, const char *line, char *resp, int resp_len, uint32_t timeout_ms) {
    if (!s_busy) return errline(resp, resp_len, "RING_DOWN");   /* minor #8: node_mgr_start() hasn't run yet */
    if (timeout_ms > FWD_TIMEOUT_MAX_MS) timeout_ms = FWD_TIMEOUT_MAX_MS;
    if (xSemaphoreTake(s_busy, 0) != pdTRUE) return errline(resp, resp_len, "BUSY");
    int rc = do_forward(zone, line, resp, resp_len, timeout_ms);
    xSemaphoreGive(s_busy);
    return rc;
}
