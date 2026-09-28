#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "panel_hw.h"
#include "panel_lock.h"
#include "pnl_zero.h"
#include "pnl_worker.h"

static const char *TAG = "pnl_work";

static pnl_job_t         s_pool[PNL_JOB_POOL];   /* internal RAM: jobs carry secrets and run() stacks point into them */
static uint8_t           s_used[PNL_JOB_POOL];
static QueueHandle_t     s_req, s_done;
static portMUX_TYPE      s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_gen;
static volatile int      s_pending;
static lv_timer_t       *s_drain;
static TaskHandle_t      s_work_task;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* pnl_work: one job at a time, in submit order. Never takes the display lock
 * and never calls lv_*: the result goes back through s_done. Not TWDT-
 * subscribed -- jobs make esp_hosted RPCs that can outlast the 8 s TWDT
 * (recovery design 6.1). */
static void worker_task(void *arg) {
    (void)arg;
    for (;;) {
        pnl_job_t *j = NULL;
        if (xQueueReceive(s_req, &j, portMAX_DELAY) != pdTRUE || !j) continue;
        if (j->run) j->run(j);
        j->t_done_ms = now_ms();
        (void)xQueueSend(s_done, &j, portMAX_DELAY);   /* depth == pool size: never actually waits */
    }
}

static void job_release(pnl_job_t *j) {
    pnl_zero(j->arg, sizeof j->arg);   /* secrets never linger past their job */
    pnl_zero(j->out, sizeof j->out);
    pnl_zero(j->err, sizeof j->err);
    int i = (int)(j - s_pool);
    portENTER_CRITICAL(&s_mux);
    s_used[i] = 0;
    s_pending--;
    portEXIT_CRITICAL(&s_mux);
}

/* 20 ms, on the LVGL task: the mailbox. */
static void drain_cb(lv_timer_t *t) {
    (void)t;
    pnl_job_t *j = NULL;
    while (xQueueReceive(s_done, &j, 0) == pdTRUE) {
        if (!j) continue;
        if (j->done) {
            int64_t t0 = esp_timer_get_time();   /* Global Constraints: a screen callback finishes in <= 200 ms */
            j->done(j);
            int64_t us = esp_timer_get_time() - t0;
            if (us > 200000) ESP_LOGW(TAG, "job done callback %p took %u ms (limit 200)", (void *)j->done, (unsigned)(us / 1000));
        }
        job_release(j);
    }
}

void pnl_worker_start(void) {
    if (s_req) return;
    s_req  = xQueueCreate(PNL_JOB_POOL, sizeof(pnl_job_t *));
    s_done = xQueueCreate(PNL_JOB_POOL, sizeof(pnl_job_t *));
    if (!s_req || !s_done) { ESP_LOGE(TAG, "job queues unavailable -- the panel cannot write or run commands"); return; }
    if (xTaskCreatePinnedToCore(worker_task, "pnl_work", 8192, NULL, 2, &s_work_task, 0) != pdPASS) {
        s_work_task = NULL;
        ESP_LOGE(TAG, "pnl_work task not created -- the panel cannot write or run commands");
        return;
    }
    if (!panel_lock(2000)) {
        ESP_LOGE(TAG, "display lock not taken -- job results will not be delivered");
        return;
    }
    s_drain = lv_timer_create(drain_cb, 20, NULL);
    panel_unlock();
    ESP_LOGI(TAG, "worker up (c0/p2, %d-job pool, not TWDT-subscribed)", PNL_JOB_POOL);
}

int pnl_worker_submit(pnl_job_run_fn run, pnl_job_done_fn done, const void *arg, size_t arg_len) {
    if (!s_req || !s_drain || !run || arg_len > PNL_JOB_ARG_MAX || (arg_len && !arg)) return -1;
    int slot = -1;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < PNL_JOB_POOL; i++) {
        if (!s_used[i]) { s_used[i] = 1; slot = i; s_pending++; break; }
    }
    portEXIT_CRITICAL(&s_mux);
    if (slot < 0) return -1;

    pnl_job_t *j = &s_pool[slot];
    memset(j, 0, sizeof *j);   /* a free slot is already wiped (job_release); this resets the header fields */
    j->run = run;
    j->done = done;
    j->screen_gen = s_gen;
    j->t_submit_ms = now_ms();
    j->rc = PSVC_OK;
    if (arg_len) memcpy(j->arg, arg, arg_len);
    if (xQueueSend(s_req, &j, 0) != pdTRUE) { job_release(j); return -1; }
    return 0;
}

int      pnl_worker_pending(void)  { return s_pending; }
uint32_t pnl_worker_stack_free(void) { return s_work_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_work_task) : 0; }
uint32_t pnl_screen_gen(void)      { return s_gen; }
void     pnl_screen_gen_bump(void) { s_gen++; }

int pnl_on_lvgl_task(void) {
    const char *n = pcTaskGetName(NULL);
    return n && strcmp(n, PANEL_LVGL_TASK_NAME) == 0;
}
