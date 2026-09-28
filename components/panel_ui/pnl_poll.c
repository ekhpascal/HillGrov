#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "notify.h"
#include "alarm_mgr.h"
#include "wifi_mgr.h"
#include "psvc_state.h"
#include "psvc_zcfg.h"
#include "pnl_poll.h"

static const char *TAG = "pnl_poll";

#define POLL_PERIOD_MS   1000u
#define WIFI_PERIOD_MS   5000u
#define SCHED_PERIOD_MS 10000u
#define FROZEN_MS       10000u

static pnl_snap_t       *s_stage;   /* the poller's private buffer: filled with no lock held */
static pnl_snap_t       *s_pub;     /* the published copy: memcpy in and out under s_mux only */
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_seq;
static TaskHandle_t      s_task;
static TaskHandle_t      s_wifi_task;

static wifi_status_t     s_wifi;
static portMUX_TYPE      s_wifi_mux = portMUX_INITIALIZER_UNLOCKED;

static volatile uint32_t s_beat_ms;   /* 0 = the LVGL heartbeat timer never ran (panel dark) */
static uint8_t           s_frozen;

static uint8_t           s_sched_valid;
static uint32_t          s_sched_ms;
static uint8_t           s_sched_used[HG_MAX_ZONES];
static uint32_t          s_sched_gen[HG_MAX_ZONES];
static hg_zone_cfg_t     s_cfg;       /* scratch for the schedule rebuild, off the 6 KB stack */
static hg_zone_hw_t      s_hw;

static am_snapshot_t    *s_am_stage, *s_am_pub;   /* PSRAM, same stage + published pattern as the state */
static portMUX_TYPE      s_am_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_am_seq;
static int64_t           s_am_total_seen = -1;

static void alarms_poll(void) {
    if (!s_am_stage || !s_am_pub) return;
    int total = alarm_mgr_total();
    if ((int64_t)total == s_am_total_seen) return;
    alarm_mgr_copy(s_am_stage);                    /* alarm_mgr's own lock, microseconds */
    s_am_total_seen = (int64_t)s_am_stage->total;
    portENTER_CRITICAL(&s_am_mux);
    memcpy(s_am_pub, s_am_stage, sizeof *s_am_pub);
    s_am_seq++;
    portEXIT_CRITICAL(&s_am_mux);
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int sched_stale(const pnl_snap_t *sn, uint32_t now) {
    if (!s_sched_valid || now - s_sched_ms >= SCHED_PERIOD_MS) return 1;
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &sn->st.node[i];
        if (n->used != s_sched_used[i]) return 1;
        if (n->used && n->hb.cfg_gen != s_sched_gen[i]) return 1;
    }
    return 0;
}

static void sched_rebuild(pnl_snap_t *sn, uint32_t now) {
    pnl_sched_reset(&sn->sched);
    for (int i = 0; i < HG_MAX_ZONES; i++) {
        const hg_node_t *n = &sn->st.node[i];
        s_sched_used[i] = n->used;
        s_sched_gen[i] = n->hb.cfg_gen;
        if (!n->used) continue;
        int hp = 0;
        if (psvc_zone_cfg_get((uint8_t)(i + 1), &s_cfg, &s_hw, NULL, &hp) != PSVC_OK) continue;
        pnl_sched_add_zone(&sn->sched, &s_cfg, hp ? &s_hw : NULL);
    }
    s_sched_ms = now;
    s_sched_valid = 1;
}

static void heartbeat_check(uint32_t now) {
    uint32_t beat = s_beat_ms;
    if (!beat) return;
    /* The LVGL task beats on the other core: a beat stamped after `now` was read makes now - beat negative, and
     * an unsigned difference would wrap to ~49 days and raise a false alarm. Signed, clamped at 0. */
    int32_t sd = (int32_t)(now - beat);
    uint32_t age = sd > 0 ? (uint32_t)sd : 0u;
    if (!s_frozen && age > FROZEN_MS) {
        s_frozen = 1;
        ESP_LOGE(TAG, "LVGL task silent for %u ms -- the panel is frozen", (unsigned)age);
        /* alarm_mgr's active set keys on the first word: a W_ prefix activates "ALARM 0" (alarm_mgr.c:12-23), so the
         * band, the web banner and alarms_active all show it -- "PANEL FROZEN" would only reach the history ring. */
        notify_emit(NTF_ALARM, 0, "W_PANEL_FROZEN LVGL task silent %us", (unsigned)(age / 1000u));
    } else if (s_frozen && age <= FROZEN_MS) {
        s_frozen = 0;
        ESP_LOGW(TAG, "LVGL task beating again");
        /* NTF_ALARM is rate-limited to 1 s per idx (notify.c:21-23) and a pnl_poll_kick() can bring the next poll
         * sooner: reset the latch so the clearing line is never dropped (it would leave the alarm up for good). */
        notify_reset(NTF_ALARM, 0);
        notify_emit(NTF_ALARM, 0, "CLEARED PANEL_FROZEN");   /* CLEARED is a clearing word: "ALARM 0" leaves the set */
    }
}

static void poll_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t t0 = now_ms();
        psvc_state_fill(&s_stage->st, PSVC_FILL_SKIP_WIFI);
        portENTER_CRITICAL(&s_wifi_mux);
        s_stage->st.wifi = s_wifi;
        portEXIT_CRITICAL(&s_wifi_mux);
        for (int i = 0; i < HG_MAX_ZONES; i++)
            s_stage->cfg_busy[i] = s_stage->st.node[i].used
                                   ? (uint8_t)(psvc_zone_cfg_busy((uint8_t)(i + 1)) ? 1 : 0) : 0;
        if (sched_stale(s_stage, t0)) sched_rebuild(s_stage, t0);
        heartbeat_check(now_ms());   /* not t0: the fill above can take a while */
        alarms_poll();               /* after the heartbeat's NOTIFY, before the state publish (see pnl_poll_alarms) */
        if (s_am_total_seen >= 0) {  /* the band's count from the SAME copy the Alarms view shows: psvc_state_fill
                                      * reads the two counts under separate lock holds, and earlier than this */
            s_stage->st.alarms_active = s_am_stage->n_active;
            s_stage->st.alarms_total  = (int)s_am_stage->total;
        }
        s_stage->started = 1;
        s_stage->seq = s_seq + 1;

        portENTER_CRITICAL(&s_mux);
        memcpy(s_pub, s_stage, sizeof *s_pub);
        s_seq = s_stage->seq;
        portEXIT_CRITICAL(&s_mux);

        uint32_t spent = now_ms() - t0;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(spent >= POLL_PERIOD_MS ? 1u : POLL_PERIOD_MS - spent));
    }
}

static void wifi_task(void *arg) {
    (void)arg;
    wifi_status_t w;
    for (;;) {
        memset(&w, 0, sizeof w);
        wifi_mgr_status(&w);   /* P4: two esp_hosted RPCs, up to 5 s each -- here and only here */
        portENTER_CRITICAL(&s_wifi_mux);
        s_wifi = w;
        portEXIT_CRITICAL(&s_wifi_mux);
        vTaskDelay(pdMS_TO_TICKS(WIFI_PERIOD_MS));
    }
}

void pnl_poll_start(void) {
    if (s_task) return;
    s_stage = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    s_pub   = heap_caps_calloc(1, sizeof(pnl_snap_t), MALLOC_CAP_SPIRAM);
    if (!s_stage || !s_pub) {
        ESP_LOGE(TAG, "no PSRAM for the snapshot buffers -- the panel stays on \"starting\"");
        return;
    }
    s_am_stage = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    s_am_pub   = heap_caps_calloc(1, sizeof(am_snapshot_t), MALLOC_CAP_SPIRAM);
    if (!s_am_stage || !s_am_pub) {   /* both or neither: alarms_poll and pnl_poll_alarms test them as a pair */
        heap_caps_free(s_am_stage);
        heap_caps_free(s_am_pub);
        s_am_stage = s_am_pub = NULL;
        ESP_LOGE(TAG, "no PSRAM for the alarm snapshot -- the Alarms screen stays on \"Loading...\"");
    }
    if (xTaskCreatePinnedToCore(wifi_task, "pnl_wifi", 4096, NULL, 1, &s_wifi_task, 0) != pdPASS) {
        s_wifi_task = NULL;
        ESP_LOGE(TAG, "pnl_wifi task not created -- Wi-Fi lines stay blank");
    }
    if (xTaskCreatePinnedToCore(poll_task, "pnl_poll", 6144, NULL, 2, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "pnl_poll task not created -- the panel stays on \"starting\"");
        s_task = NULL;
        return;
    }
    ESP_LOGI(TAG, "poller up (1 Hz state, %u s Wi-Fi, not TWDT-subscribed)", (unsigned)(WIFI_PERIOD_MS / 1000));
}

void pnl_poll_latest(pnl_snap_t *out) {
    if (!s_pub) { memset(out, 0, sizeof *out); return; }
    portENTER_CRITICAL(&s_mux);
    memcpy(out, s_pub, sizeof *out);
    portEXIT_CRITICAL(&s_mux);
}

uint32_t pnl_poll_seq(void) { return s_seq; }

void pnl_poll_stack_free(uint32_t *poll_b, uint32_t *wifi_b) {
    if (poll_b) *poll_b = s_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_task) : 0;
    if (wifi_b) *wifi_b = s_wifi_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_wifi_task) : 0;
}

void pnl_poll_kick(void) {
    TaskHandle_t t = s_task;
    if (t) xTaskNotifyGive(t);
}

void pnl_lvgl_heartbeat(void) {
    uint32_t t = now_ms();
    s_beat_ms = t ? t : 1u;
}

void pnl_poll_alarms(am_snapshot_t *out) {
    if (!s_am_pub) { memset(out, 0, sizeof *out); return; }
    portENTER_CRITICAL(&s_am_mux);
    memcpy(out, s_am_pub, sizeof *out);
    portEXIT_CRITICAL(&s_am_mux);
}

uint32_t pnl_poll_alarms_seq(void) { return s_am_seq; }
