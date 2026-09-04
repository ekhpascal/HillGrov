#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "driver/uart.h"
#include "ring_link.h"
#include "ota_trial.h"
#include "cmd_task.h"
#include "hg_model.h"
#include "hg_store.h"
#include "notify.h"
#include "rescue_handover.h"
#include "app_if_common.h"
#include "app_version.h"
#include "zone_ring.h"
#include "zone_ring_internal.h"

/* Task 15's bootloader-side handshake owner; forward-declared here the same
 * way app_if_zone.c does until a shared header exists (SP1 placeholder,
 * carried forward per Task 12's brief). */
void hg_reboot_to_rescue(void);

/* spec 5.5 edge default: what travels from this zone to the ring/master. */
#define ZRING_NOTIFY_MASK (uint16_t)(NTF_MASK(NTF_BOOT) | NTF_MASK(NTF_ALARM) | NTF_MASK(NTF_SAFE) | \
                                      NTF_MASK(NTF_FW)   | NTF_MASK(NTF_NODE)  | NTF_MASK(NTF_RING))

static cmd_core_t     *s_core;
static cmd_session_t s_ring_ses = { .source = CMD_SRC_RING, .echo = 0, .notify_mask = ZRING_NOTIFY_MASK };
static char           s_resp[CMD_RESP_MAX];
static ring_dup_t     s_dup;
static uint16_t       s_tx_seq;
static portMUX_TYPE   s_seq_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t        s_mac[6];
static uint8_t        s_reset_reason;
static int            s_notify_sink = -1;
static uint16_t       s_synced_notify_mask = ZRING_NOTIFY_MASK;

static uint32_t     s_last_hb_ms;
static volatile int  s_hb_now = 1;             /* immediate-send flag: link start / ASSIGN_ID */

#define ZRING_HB_PERIOD_MS  2000u   /* spec 2.7: HEARTBEAT every 2000 ms ... */
#define ZRING_HB_FLOOR_MS    500u   /* ... immediate triggers "rate-limited 1/500 ms" */

/* Two headers, two names for "no id yet": hg_cfg_types.h's HG_NODE_UNASSIGNED
 * (what hg_store persists and cmd_core carries) and ring_proto.h's
 * RING_ID_UNASSIGNED (what goes on the wire and what ring_route matches). They
 * are compared against each other all over the zone; this TU is the one that
 * sees both, so pin them here (review minor #3). */
_Static_assert(HG_NODE_UNASSIGNED == RING_ID_UNASSIGNED,
               "unassigned-node id must be the same value on the wire and in the store");

static uint32_t now_ms(void) { return s_core->now_ms(); }

/* TIME_SYNC/ASSIGN_ID, link-state (W_LINK_LOST) and the inhibit-mask
 * primitive live in zone_ring_sync.c (Task 12 line-cap split); ASSIGN_ID
 * requests an immediate HB through this, since s_hb_now stays here. */
void zring_request_immediate_hb(void) { s_hb_now = 1; }

/* zring_next_seq is called from whatever task last called notify_emit() (the
 * ring NOTIFY sink runs inline on the emitting task, not on "zring") as well
 * as from zone_ring_task itself -- a plain ++ would race across tasks. */
uint16_t zring_next_seq(void) {
    taskENTER_CRITICAL(&s_seq_mux);
    uint16_t v = ++s_tx_seq;
    taskEXIT_CRITICAL(&s_seq_mux);
    return v;
}

static void zring_send(uint8_t dst, uint8_t type, const uint8_t *payload, uint8_t len) {
    ring_hdr_t h = { .src = hg_store_zid(), .dst = dst, .type = type, .flags = 0,
                      .ttl = RING_TTL_INIT, .len = len, .seq = zring_next_seq() };
    ring_link_send(&h, payload);
}

void zring_send_ack(uint8_t dst, uint16_t acked_seq, uint8_t status, const char *detail, uint8_t dlen) {
    hg_ack_t a = { .acked_seq = acked_seq, .status = status, .detail_len = dlen };
    if (dlen) memcpy(a.detail, detail, dlen);
    uint8_t payload[3 + 125];
    int n = hg_ack_pack(&a, payload, sizeof payload);
    if (n < 0) return;                          /* every caller already clamps dlen <= 125 */
    zring_send(dst, RING_T_ACK, payload, (uint8_t)n);
}

/* First reply line + as many whole "  "-continuation lines as fit 125 B
 * (spec 2.4); truncates at a line boundary, never mid-line. */
static uint8_t build_ack_detail(const char *resp, char out[126]) {
    size_t total = 0;
    const char *p = resp;
    int first = 1;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (!first && !(linelen >= 3 && p[0] == ' ' && p[1] == ' ')) break;
        if (first) { if (linelen > 125) linelen = 125; }
        else if (total + linelen > 125) break;
        memcpy(out + total, p, linelen);
        total += linelen;
        first = 0;
        if (!nl) break;
        p = nl + 1;
    }
    return (uint8_t)total;
}

/* At-most-once cache (spec 2.6), shared by the two tracked, side-effecting
 * frame types the master sends: CMD and CFG_COMMIT. One slot is enough because
 * the master keeps exactly ONE tracked frame outstanding ring-wide (ring_trk is
 * stop-and-wait), so two of them are never mid-execution here at the same time.
 * 1 = this frame was a retransmit and is fully handled (absorbed while the
 * original is still running, or the cached reply re-sent); 0 = the caller must
 * execute it and then report the outcome through zring_dup_finish(). */
int zring_dup_begin(const ring_frame_t *f, uint32_t now) {
    int st = ring_dup_check(&s_dup, f->hdr.seq, now);
    if (st == RING_DUP_ABSORB) return 1;                   /* in-progress dup: nothing */
    if (st == RING_DUP_REPLAY) {
        uint8_t dlen = (uint8_t)strnlen(s_dup.detail, sizeof s_dup.detail - 1);
        zring_send_ack(f->hdr.src, f->hdr.seq, s_dup.status, s_dup.detail, dlen);
        return 1;
    }
    ring_dup_start(&s_dup, f->hdr.seq, now);
    return 0;
}

void zring_dup_finish(uint16_t seq, uint8_t status, const char *detail) {
    ring_dup_done(&s_dup, seq, status, detail);
}

static void handle_cmd(const ring_frame_t *f, uint32_t now) {
    if (zring_dup_begin(f, now)) return;
    char line[RING_MAX_PAYLOAD + 1];
    size_t n = f->hdr.len > RING_MAX_PAYLOAD ? RING_MAX_PAYLOAD : f->hdr.len;
    memcpy(line, f->payload, n);
    line[n] = '\0';
    int rc = cmd_task_execute(&s_ring_ses, line, s_resp, sizeof s_resp, 3500);
    /* mirrors cli.c's own live sync: a ring-sourced "SET NOTIFY ..." mutates
     * s_ring_ses.notify_mask (the calling session) via cmd_dispatch, but the
     * ring NOTIFY sink's filter (registered once at start) only changes when
     * told to -- push it here, right after the command that could have
     * changed it. */
    if (s_notify_sink >= 0 && s_ring_ses.notify_mask != s_synced_notify_mask) {
        notify_set_sink_mask(s_notify_sink, s_ring_ses.notify_mask);
        s_synced_notify_mask = s_ring_ses.notify_mask;
    }
    uint8_t status = rc == 0 ? 0 : 1;
    char detail[126];
    /* s_resp is static, not stack (cmd_task's abandoned-slot rule, spec/SP1):
     * on a 3500 ms internal timeout, cmd_task_execute itself already wrote a
     * synchronous "ERR INTERNAL" into s_resp before returning here, but the
     * orphaned worker task can still be mid-cmd_dispatch and later overwrite
     * s_resp with the *original* command's real output -- after this ACK has
     * already gone out. That's a stale/racy content read on an already-rare
     * timeout path, never a dangling pointer (the buffer is never freed) --
     * an accepted SP1 tradeoff, not something this read needs to guard. */
    uint8_t dlen = build_ack_detail(s_resp, detail);
    detail[dlen] = '\0';
    ring_dup_done(&s_dup, f->hdr.seq, status, detail);
    zring_send_ack(f->hdr.src, f->hdr.seq, status, detail, dlen);
}

static void handle_fw_update(const ring_frame_t *f) {
    hg_fwu_t fw;
    memset(&fw, 0, sizeof fw);
    if (hg_fwu_parse(f->payload, f->hdr.len, &fw) != 0) return;
    hg_store_flush(2000);
    hg_handover_t h;
    memset(&h, 0, sizeof h);
    h.expect_link = 1;
    memcpy(h.ssid, fw.ssid, sizeof h.ssid);
    memcpy(h.pass, fw.pass, sizeof h.pass);
    snprintf(h.url, sizeof h.url, "http://192.168.7.7/fw/zone.bin");
    if (hg_handover_write(&h) != 0) {
        zring_send_ack(f->hdr.src, f->hdr.seq, 1, "ERR NVS_WRITE", (uint8_t)(sizeof("ERR NVS_WRITE") - 1));
        return;
    }
    zring_send_ack(f->hdr.src, f->hdr.seq, 0, "OK", 2);
    uart_wait_tx_done(UART_NUM_2, pdMS_TO_TICKS(500));
    /* TWDT budget: flush 2000 + drain 500 + this delay must stay well under
     * the watchdog's window -- clamp so a bogus/oversized wire value from a
     * corrupted or malicious frame can't stall the reboot indefinitely. */
    uint16_t delay = fw.reboot_delay_ms;
    if (delay > 3000) delay = 3000;
    vTaskDelay(pdMS_TO_TICKS(delay));
    hg_reboot_to_rescue();
}

static void build_hb(hg_hb_t *h, uint32_t now) {
    memset(h, 0, sizeof *h);                                /* SP2 fields (mode/faults/shelf/...) stay 0 */
    memcpy(h->mac, s_mac, 6);

    app_version_t v;
    if (app_version_parse(esp_app_get_description()->version, &v) == 0) {
        h->fw_maj = v.major; h->fw_min = v.minor; h->fw_patch = v.patch;
    }

    hg_zone_cfg_t cfg;
    hg_model_snapshot_cfg(&cfg, NULL);
    for (int i = 0; i < HG_MAX_SHELVES; i++) if (cfg.shelf[i].enabled) h->n_shelves++;

    h->uptime_s  = hg_app_uptime_s();
    h->unix_time = (uint32_t)time(NULL);
    hg_model_cfg_info(&h->cfg_gen, &h->cfg_crc);
    hg_model_hw_crc(&h->hw_crc);
    h->cfg_src       = hg_model_cfg_src();
    h->reset_reason  = s_reset_reason;
    h->time_quality  = zsync_time_quality();
    h->link_flags    = zsync_link_flags(now, hg_store_zid());

    ring_counters_t cnt;
    ring_link_counters(&cnt);
    h->rx_crc_err        = (uint16_t)cnt.rx_crc_err;
    h->rx_uart_err       = (uint16_t)cnt.rx_uart_err;
    h->rx_drop           = (uint16_t)cnt.rx_drop;
    h->fwd_count         = (uint16_t)cnt.fwd_count;
    h->min_free_heap_kb  = (uint16_t)(esp_get_minimum_free_heap_size() / 1024);
}

static void send_hb(uint32_t now) {
    hg_hb_t h;
    build_hb(&h, now);
    uint8_t payload[62 + 14 * HG_MAX_SHELVES];
    int n = hg_hb_pack(&h, payload, sizeof payload);
    if (n >= 0) zring_send(RING_ID_MASTER, RING_T_HEARTBEAT, payload, (uint8_t)n);
    s_last_hb_ms = now;
    s_hb_now = 0;
}

static void ring_notify_sink(void *ctx, const char *line) {
    (void)ctx;
    size_t n = strlen(line);
    if (n > RING_MAX_PAYLOAD) n = RING_MAX_PAYLOAD;
    zring_send(RING_ID_MASTER, RING_T_NOTIFY, (const uint8_t *)line, (uint8_t)n);
}

static void dispatch_frame(const ring_frame_t *f, uint32_t now) {
    switch (f->hdr.type) {
        case RING_T_CMD:        handle_cmd(f, now); break;
        case RING_T_TIME_SYNC:  zsync_time_sync(f, now); break;
        case RING_T_ASSIGN_ID:  zsync_assign_id(f); break;
        case RING_T_CFG_CHUNK:  zone_ring_cfg_chunk(f, now); break;
        case RING_T_CFG_COMMIT: zone_ring_cfg_commit(f, now); break;
        case RING_T_CFG_GET:    zone_ring_cfg_get(f, now); break;
        case RING_T_FW_UPDATE:  handle_fw_update(f); break;
        default: break;   /* HEARTBEAT/ACK/NOTIFY/FAULT_EVENT never route to a zone (ring_route) */
    }
}

static void zone_ring_task(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    s_reset_reason = (uint8_t)esp_reset_reason();
    hg_app_get_mac(s_mac);
    ring_dup_init(&s_dup);
    zone_ring_cfg_init();

    for (;;) {
        esp_task_wdt_reset();
        ring_frame_t f;
        if (ring_link_recv(&f, 100) == 0) {
            uint32_t now = now_ms();
            zsync_note_rx(now);
            if (f.hdr.src == RING_ID_MASTER) { ota_trial_master_frame(); zsync_note_master(now); }
            dispatch_frame(&f, now);
        }
        ota_trial_tick();
        uint32_t now = now_ms();
        zsync_link_tick(now);
        /* spec 2.7's floor: an immediate trigger (link start, ASSIGN_ID, fault
         * edge) may bring the next heartbeat forward, but never closer than
         * 500 ms to the last one. s_hb_now stays latched until it actually
         * fires (send_hb clears it), so a trigger inside the floor window is
         * delayed, not lost. Without this any per-heartbeat stimulus from the
         * master turns the pair into a wire-speed loop. */
        uint32_t since_hb = now - s_last_hb_ms;
        if ((s_hb_now && since_hb >= ZRING_HB_FLOOR_MS) || since_hb >= ZRING_HB_PERIOD_MS) send_hb(now);
    }
}

void zone_ring_start(cmd_core_t *core) {
    s_core = core;
    zone_ring_sync_init(core);
    ota_trial_drivers_ok();     /* ring_link_start() already returned by the time app_main calls us */
    /* Registered here (synchronously), not inside zone_ring_task: app_main's
     * once-per-boot NTF_BOOT emit follows this call immediately and must not
     * race the new task's startup, or the ring never sees the POWERON line. */
    s_notify_sink = notify_add_sink(ring_notify_sink, NULL, ZRING_NOTIFY_MASK);
    xTaskCreatePinnedToCore(zone_ring_task, "zring", 4096, NULL, 5, NULL, 0);
}
