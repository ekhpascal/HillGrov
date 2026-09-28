#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "cmd_core.h"
#include "cmd_task.h"
#include "pnl_zero.h"
#include "pnl_worker.h"
#include "pnl_cmd.h"

static const char *TAG = "pnl_cmd";

typedef struct {
    cmd_session_t ses;
    char          resp[CMD_RESP_MAX];   /* what cmd_task writes; never handed to a caller */
    uint8_t       busy;
    uint8_t       dead;                 /* quarantined: an orphaned cmd_task worker still owns resp */
} pnl_cmd_slot_t;

static pnl_cmd_slot_t   *s_slot;        /* PNL_CMD_SLOTS in PSRAM, never freed */
static SemaphoreHandle_t s_lock;
static volatile uint8_t  s_quarantined; /* slots permanently withdrawn; only ever grows */

void pnl_cmd_init(void) {
    if (s_slot && s_lock) return;
    if (!s_slot) s_slot = heap_caps_calloc(PNL_CMD_SLOTS, sizeof *s_slot, MALLOC_CAP_SPIRAM);
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_slot || !s_lock) {
        ESP_LOGE(TAG, "no memory for the panel command sessions -- console, Replace board, Set clock and Reboot unavailable");
        return;
    }
    for (int i = 0; i < PNL_CMD_SLOTS; i++) {
        s_slot[i].ses.source          = CMD_SRC_HTTP;   /* D1: the web's semantics exactly */
        s_slot[i].ses.echo            = 0;
        s_slot[i].ses.notify_mask     = 0;              /* NOTIFY lines have no stream to go to here */
        s_slot[i].ses.unlock_until_ms = 0;              /* the glass can never hold the debug unlock */
    }
}

uint8_t pnl_cmd_quarantined(void) { return s_quarantined; }

static void put(char *reply, size_t cap, const char *s) {
    if (reply && cap) snprintf(reply, cap, "%s", s);
}

static void release(pnl_cmd_slot_t *slot) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        slot->busy = 0;
        xSemaphoreGive(s_lock);
        return;
    }
    slot->busy = 0;   /* a byte store either way; losing the slot would be worse (http_cmd.c's release) */
}

int pnl_cmd_run(const char *line, char *reply, size_t cap) {
    if (pnl_on_lvgl_task()) {
        ESP_LOGE(TAG, "pnl_cmd_run called on the LVGL task -- refused (the LVGL task must never block)");
        put(reply, cap, "ERR INTERNAL\n");
        return -1;
    }
    size_t n = line ? strlen(line) : 0;
    if (n == 0 || n > (size_t)(CMD_LINE_MAX - 1)) { put(reply, cap, "ERR TOO_LONG\n"); return -4; }
    if (!s_slot || !s_lock) { put(reply, cap, "ERR INTERNAL\n"); return -1; }

    pnl_cmd_slot_t *slot = NULL;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < PNL_CMD_SLOTS && !slot; i++)
            if (!s_slot[i].busy && !s_slot[i].dead) { s_slot[i].busy = 1; slot = &s_slot[i]; }
        xSemaphoreGive(s_lock);
    }
    if (!slot) { put(reply, cap, "ERR BUSY\n"); return -3; }

    int rc = cmd_task_execute(&slot->ses, line, slot->resp, CMD_RESP_MAX, PNL_CMD_TIMEOUT_MS);
    if (rc == -2) {
        /* cmd_task.h: the orphaned worker still owns slot->resp and will write into it later. Never read it, never
         * release the slot. The line is not logged: a console line can carry a credential. */
        slot->dead = 1;
        s_quarantined++;
        ESP_LOGE(TAG, "cmd dispatch did not return in %u ms -- panel session slot %u/%d withdrawn",
                 (unsigned)PNL_CMD_TIMEOUT_MS, (unsigned)s_quarantined, PNL_CMD_SLOTS);
        put(reply, cap, "ERR INTERNAL\n");
        return -2;
    }
    put(reply, cap, slot->resp);
    pnl_zero(slot->resp, CMD_RESP_MAX);   /* a reply can echo a credential; do not leave it in a pooled buffer (C3) */
    release(slot);
    return rc == 0 ? 0 : -1;
}
