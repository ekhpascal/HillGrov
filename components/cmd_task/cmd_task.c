#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "cmd_task.h"

static const char *TAG = "cmd_task";
#define POOL_N 4

typedef struct {
    volatile int  used;
    volatile int  done;        /* worker has finished dispatch and decided this slot's fate */
    cmd_session_t *ses;
    char           line[CMD_LINE_MAX];
    char          *resp;
    int            resp_len, rc;
    TaskHandle_t   waiter;
} slot_t;

static slot_t s_pool[POOL_N];
static QueueHandle_t s_q;                  /* items: slot_t* */
static const cmd_core_t *s_core;
/* Guards slot claim/done/waiter/used bookkeeping so exactly one side (caller
 * on timeout, or the worker on completion) ever frees a slot, and the worker
 * only ever notifies a task that has not already given up on it. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static void cmd_task_main(void *arg) {
    (void)arg;
    esp_task_wdt_add(NULL);
    slot_t *s;
    for (;;) {
        esp_task_wdt_reset();
        if (xQueueReceive(s_q, &s, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        s->rc = cmd_dispatch(s_core, s->ses, s->line, s->resp, s->resp_len);
        taskENTER_CRITICAL(&s_mux);
        s->done = 1;
        TaskHandle_t w = s->waiter;
        if (!w) s->used = 0;                /* caller already gave up: free here */
        taskEXIT_CRITICAL(&s_mux);
        if (w) xTaskNotifyGive(w);
    }
}

void cmd_task_start(const cmd_core_t *core) {
    s_core = core;
    s_q = xQueueCreate(8, sizeof(slot_t *));
    int rc = cmd_table_check(core->table, core->table_len);
    if (rc >= 0) ESP_LOGE(TAG, "command table row %d invalid", rc);
    xTaskCreatePinnedToCore(cmd_task_main, "cmd_task", 6144, NULL, 5, NULL, 0);
}

int cmd_task_execute(cmd_session_t *ses, const char *line, char *resp, int resp_len, uint32_t timeout_ms) {
    slot_t *s = NULL;
    for (int tries = 0; tries < 10 && !s; tries++) {          /* ~100 ms pool wait */
        taskENTER_CRITICAL(&s_mux);
        for (int i = 0; i < POOL_N; i++)
            if (!s_pool[i].used) { s_pool[i].used = 1; s_pool[i].done = 0; s = &s_pool[i]; break; }
        taskEXIT_CRITICAL(&s_mux);
        if (!s) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!s) return cmd_err(resp, resp_len, "BUSY");
    s->ses = ses;
    snprintf(s->line, sizeof s->line, "%s", line);
    s->resp = resp; s->resp_len = resp_len;
    s->waiter = xTaskGetCurrentTaskHandle();
    if (xQueueSend(s_q, &s, pdMS_TO_TICKS(50)) != pdTRUE) {
        s->used = 0;
        return cmd_err(resp, resp_len, "BUSY");
    }
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms)) == 0) {
        taskENTER_CRITICAL(&s_mux);
        if (!s->done) {
            s->waiter = NULL;                /* orphan; cmd_task frees the slot on completion */
            taskEXIT_CRITICAL(&s_mux);
            return cmd_err(resp, resp_len, "INTERNAL");
        }
        taskEXIT_CRITICAL(&s_mux);
        /* done was already set: the worker read a non-NULL waiter under the
         * same critical section and has committed to (or already did) give
         * our notification -- it is guaranteed imminent. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        int rc = s->rc;
        s->used = 0;
        return rc;
    }
    int rc = s->rc;
    s->used = 0;
    return rc;
}
