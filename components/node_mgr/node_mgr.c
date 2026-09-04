#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "ring_link.h"
#include "notify.h"
#include "node_mgr.h"
#include "node_mgr_internal.h"

/* Master's ring orchestration task (spec §6.1). Split per Task 13's brief:
 * this file owns the node table, the tracker and the 50 ms/1 s tick loop
 * (frame dispatch, ACK/tracker-event routing, NOTIFY/FAULT_EVENT
 * passthrough); node_mgr_enrol.c is the ztab + HB enrolment + TIME_SYNC
 * half; node_mgr_fwd.c is the CLI forward path; node_mgr_cfg.c is the §4.4
 * config reconciliation + push/pull machinery. */

static hg_node_t     s_tab[HG_MAX_ZONES];          /* slot = id-1 */
static ring_trk_t    s_trk;
static SemaphoreHandle_t s_trk_mux;                /* guards s_trk: node_mgr_forward submits cross-task */
static ring_status_t s_ring_status;
static uint8_t       s_time_valid;

static uint16_t      s_seq;
static portMUX_TYPE  s_seq_mux = portMUX_INITIALIZER_UNLOCKED;

static SemaphoreHandle_t s_state_mux;              /* guards s_tab/s_ring_status/ztab + cfg's request flags */

/* ---- shared plumbing (node_mgr_internal.h) ---- */

void nmgr_lock(void)   { xSemaphoreTake(s_state_mux, portMAX_DELAY); }
void nmgr_unlock(void) { xSemaphoreGive(s_state_mux); }

uint32_t nmgr_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

uint16_t nmgr_next_seq(void) {
    taskENTER_CRITICAL(&s_seq_mux);
    uint16_t v = ++s_seq;
    taskEXIT_CRITICAL(&s_seq_mux);
    return v;
}

hg_node_t *nmgr_node_by_id(uint8_t id) {
    if (id < 1 || id > HG_MAX_ZONES) return NULL;
    return &s_tab[id - 1];
}

hg_node_t *nmgr_table(void) { return s_tab; }

uint8_t nmgr_ring_size(void) { return (uint8_t)node_mgr_node_count(); }

int nmgr_submit(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len, uint16_t *seq_out) {
    uint16_t seq = nmgr_next_seq();
    ring_hdr_t h = { .src = RING_ID_MASTER, .dst = dst, .type = type, .flags = RING_F_ACK_REQ,
                      .ttl = RING_TTL_INIT, .len = len, .seq = seq };
    xSemaphoreTake(s_trk_mux, portMAX_DELAY);
    int rc = ring_trk_submit(&s_trk, &h, payload, nmgr_now_ms());
    xSemaphoreGive(s_trk_mux);
    if (rc != 0) return -1;
    if (seq_out) *seq_out = seq;
    return 0;
}

void nmgr_send_raw(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len) {
    ring_hdr_t h = { .src = RING_ID_MASTER, .dst = dst, .type = type, .flags = 0,
                      .ttl = RING_TTL_INIT, .len = len, .seq = nmgr_next_seq() };
    ring_link_send(&h, payload);
}

/* ---- ACK / tracker event routing ---- */

static void route_trk_ev(const ring_trk_ev_t *ev) {
    switch (ev->type) {
    case RING_T_CMD:        nmgr_fwd_on_ev(ev); break;
    case RING_T_CFG_GET:
    case RING_T_CFG_COMMIT: nmgr_cfg_on_ev(ev); break;
    default: break;   /* FW_UPDATE: Task 15's fleet sequencer, not wired here */
    }
}

static void handle_ack(const ring_frame_t *f) {
    hg_ack_t a;
    if (hg_ack_parse(f->payload, f->hdr.len, &a) != 0) return;
    ring_trk_ev_t ev;
    xSemaphoreTake(s_trk_mux, portMAX_DELAY);
    int have = ring_trk_ack(&s_trk, a.acked_seq, a.status, a.detail, a.detail_len, &ev);
    xSemaphoreGive(s_trk_mux);
    if (have) route_trk_ev(&ev);
}

/* ---- NOTIFY / FAULT_EVENT passthrough ----
 * A zone's NOTIFY payload is already a complete "NOTIFY <TYPE> <zoneid> ..."
 * line (its own notify_emit, relayed verbatim by zone_ring's sink). notify.c
 * has no raw-broadcast API, so re-injecting it means re-running it through
 * the master's own notify_emit -- which always prints ITS OWN node id (0,
 * fixed at notify_init) right after the type. To avoid losing which zone
 * this is about, the parsed zone id is kept as the first content word, so a
 * master console line reads "NOTIFY <TYPE> 0 <zoneid> <rest>" -- the extra
 * leading 0 is an accepted artifact of notify.c's single-id design running
 * on a device that reports on OTHER entities; ring_health's own lines (see
 * node_mgr_enrol.c's nmgr_health_cb) get the same treatment for the same
 * reason. */

static void handle_notify_frame(const ring_frame_t *f) {
    char buf[RING_MAX_PAYLOAD + 1];
    size_t n = f->hdr.len > RING_MAX_PAYLOAD ? RING_MAX_PAYLOAD : f->hdr.len;
    memcpy(buf, f->payload, n); buf[n] = '\0';
    size_t len = strlen(buf);
    if (len && buf[len - 1] == '\n') buf[len - 1] = '\0';

    char *p = buf;
    if (strncmp(p, "NOTIFY ", 7) == 0) p += 7;
    char *sp1 = strchr(p, ' ');
    if (!sp1) return;
    *sp1 = '\0';
    char *id_str = sp1 + 1;
    char *sp2 = strchr(id_str, ' ');
    const char *rest = "";
    if (sp2) { *sp2 = '\0'; rest = sp2 + 1; }

    int type = notify_parse(p);
    if (type < 0 || type >= NTF_COUNT) return;
    long id = strtol(id_str, NULL, 10);
    notify_emit((ntf_type_t)type, (uint8_t)id, "%ld %s", id, rest);
}

/* FAULT_EVENT has no payload codec yet (SP2's fault store hasn't landed) --
 * best-effort per the brief: relay the raw bytes as the ALARM's content. */
static void handle_fault_event(const ring_frame_t *f) {
    char buf[RING_MAX_PAYLOAD + 1];
    size_t n = f->hdr.len > RING_MAX_PAYLOAD ? RING_MAX_PAYLOAD : f->hdr.len;
    memcpy(buf, f->payload, n); buf[n] = '\0';
    notify_emit(NTF_ALARM, f->hdr.src, "%u %s", (unsigned)f->hdr.src, buf);
}

static void dispatch_frame(const ring_frame_t *f, uint32_t now) {
    if (f->hdr.src == RING_ID_MASTER) {
        /* ring_link's authorized DROP_SELF extension (ruling #9): our own
         * tracked unicast (CMD/FW_UPDATE/CFG_COMMIT/CFG_GET) came back
         * unconsumed -- the addressed id doesn't exist on the ring. */
        ring_trk_ev_t ev;
        xSemaphoreTake(s_trk_mux, portMAX_DELAY);
        int have = ring_trk_unclaimed(&s_trk, &f->hdr, &ev);
        xSemaphoreGive(s_trk_mux);
        if (have) route_trk_ev(&ev);
        return;
    }
    switch (f->hdr.type) {
    case RING_T_HEARTBEAT:   nmgr_enrol_handle_hb(f); break;
    case RING_T_ACK:         handle_ack(f); break;
    case RING_T_NOTIFY:      handle_notify_frame(f); break;
    case RING_T_FAULT_EVENT: handle_fault_event(f); break;
    case RING_T_CFG_CHUNK:   nmgr_cfg_on_chunk(f, now); break;
    default: break;   /* CMD/TIME_SYNC/ASSIGN_ID/CFG_COMMIT/CFG_GET/FW_UPDATE never route to the master */
    }
}

/* ---- task ---- */

static void node_mgr_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    ring_trk_init(&s_trk);
    nmgr_cfg_init();
    nmgr_enrol_boot_init();   /* ruling #2: no events at load -- table starts NODE_H_EMPTY */

    uint32_t last_1s = nmgr_now_ms();
    uint32_t last_2s = last_1s;
    for (;;) {
        esp_task_wdt_reset();
        uint32_t now = nmgr_now_ms();

        ring_trk_ev_t ev;
        xSemaphoreTake(s_trk_mux, portMAX_DELAY);
        int have = ring_trk_tick(&s_trk, now, nmgr_ring_size(), &ev);
        xSemaphoreGive(s_trk_mux);
        if (have) {
            if (ev.kind == RING_TRK_EV_SEND) ring_link_send_raw(ev.wire, ev.wire_len);
            else route_trk_ev(&ev);
        }

        ring_frame_t f;
        while (ring_link_recv(&f, 0) == 0) dispatch_frame(&f, now);

        if (now - last_1s >= 1000) {
            last_1s = now;
            nmgr_lock();
            ring_health_eval(s_tab, HG_MAX_ZONES, now, ring_link_ts_returned_ms(), &s_ring_status,
                              nmgr_health_cb, NULL);
            nmgr_unlock();
            nmgr_cfg_tick_1s(now);
        }
        if (now - last_2s >= 2000) { last_2s = now; nmgr_broadcast_time_sync(now); }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void node_mgr_start(void) {
    s_trk_mux = xSemaphoreCreateMutex();
    s_state_mux = xSemaphoreCreateMutex();
    nmgr_fwd_init();
    xTaskCreatePinnedToCore(node_mgr_task, "node_mgr", 6144, NULL, 4, NULL, 1);
}

/* ---- public accessors ----
 * node_mgr_get/ring_status/node_count copy under nmgr_lock() -- the same
 * mutex node_mgr_cfg.c's pending-request flags use -- so a caller never
 * observes a torn snapshot (fix round minor #7); the values themselves can
 * still be one tick stale, which is exactly what "snapshot" already means. */

int node_mgr_node_count(void) {
    nmgr_lock();
    int n = 0;
    for (int i = 0; i < HG_MAX_ZONES; i++) if (s_tab[i].used) n++;
    nmgr_unlock();
    return n;
}

int node_mgr_get(int slot, hg_node_t *out) {
    if (slot < 0 || slot >= HG_MAX_ZONES) return -1;
    nmgr_lock();
    int used = s_tab[slot].used;
    if (used) *out = s_tab[slot];
    nmgr_unlock();
    return used ? 0 : -1;
}

void node_mgr_ring_status(ring_status_t *out) {
    nmgr_lock();
    *out = s_ring_status;
    nmgr_unlock();
}

int node_mgr_set_name(uint8_t zone, const char *name) {
    nmgr_lock();
    int rc = nmgr_ztab_set_name(zone, name);
    if (rc == 0) {
        hg_node_t *nd = nmgr_node_by_id(zone);
        if (nd && nd->used) snprintf(nd->name, sizeof nd->name, "%s", name);
    }
    nmgr_unlock();
    return rc;
}

int node_mgr_clear(uint8_t zone) {
    nmgr_lock();
    int rc = nmgr_ztab_clear(zone);
    if (rc == 0) {
        hg_node_t *nd = nmgr_node_by_id(zone);
        if (nd) memset(nd, 0, sizeof *nd);
    }
    nmgr_unlock();
    /* the RAM cfg cache (s_cx/s_cfg/s_hw/s_casm) belongs to the node_mgr
     * task alone (important #3) -- queue it instead of touching it here. */
    if (rc == 0) nmgr_cfg_request_clear(zone);
    return rc;
}

int node_mgr_unassigned(uint8_t macs[][6], int cap) { return nmgr_unassigned_copy(macs, cap); }

int node_mgr_time_valid(void) { return s_time_valid; }
void node_mgr_time_was_set(void) { s_time_valid = 1; }

void node_mgr_mark_updating(uint8_t zone, uint32_t hold_ms) {
    nmgr_lock();
    hg_node_t *nd = nmgr_node_by_id(zone);
    if (nd) nd->updating_until_ms = nmgr_now_ms() + hold_ms;
    nmgr_unlock();
}
