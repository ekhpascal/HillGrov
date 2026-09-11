#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "cmd_task.h"
#include "http_srv_internal.h"

static const char *TAG = "http_cmd";

/* POST /api/cmd is the CLI line over HTTP: the body is one command, the
 * response is the CLI's own reply text, verbatim. Everything about what a
 * command may do stays in cmd_dispatch -- an HTTP session is a cmd_session_t
 * with source CMD_SRC_HTTP, which is what makes the dispatcher answer
 * NOT_LOCAL for session-scoped rows (DEBUG ENABLE) and refuse the ones
 * gated behind a console unlock. This file only moves bytes.
 *
 * Two sessions, each with its own response buffer, claimed under a mutex.
 * esp_http_server serves every socket from ONE task today, so the second slot
 * (and the 503) cannot actually be reached; the claim is what keeps that an
 * implementation detail rather than an assumption baked into the endpoint. */

#define HTTP_CMD_SESSIONS 2

typedef struct {
    cmd_session_t ses;
    char          resp[CMD_RESP_MAX];
    uint8_t       busy;
} cmd_slot_t;

static cmd_slot_t       s_slot[HTTP_CMD_SESSIONS];
static SemaphoreHandle_t s_lock;

int http_cmd_init(void) {
    for (int i = 0; i < HTTP_CMD_SESSIONS; i++) {
        s_slot[i].ses.source          = CMD_SRC_HTTP;
        s_slot[i].ses.echo            = 0;
        s_slot[i].ses.notify_mask     = 0;   /* NOTIFY lines have no stream to go to here */
        s_slot[i].ses.unlock_until_ms = 0;   /* a web session can never hold the debug unlock */
        s_slot[i].busy                = 0;
    }
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "session mutex unavailable");
        return -1;
    }
    return 0;
}

static cmd_slot_t *claim(void) {
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return NULL;
    cmd_slot_t *got = NULL;
    for (int i = 0; i < HTTP_CMD_SESSIONS && !got; i++)
        if (!s_slot[i].busy) { s_slot[i].busy = 1; got = &s_slot[i]; }
    xSemaphoreGive(s_lock);
    return got;
}

static void release(cmd_slot_t *s) {
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s->busy = 0;
        xSemaphoreGive(s_lock);
        return;
    }
    s->busy = 0;   /* a uint8_t store either way; losing the slot would be worse */
}

esp_err_t h_cmd(httpd_req_t *req) {
    /* cap - 1 == CMD_LINE_MAX - 1, the dispatcher's own line limit */
    char line[CMD_LINE_MAX];
    int n = http_srv_body(req, line, sizeof line);
    if (n == HTTP_BODY_TOO_LONG) { http_srv_text(req, 413, "ERR TOO_LONG\n");   return ESP_OK; }
    if (n == HTTP_BODY_CHUNKED)  { http_srv_text(req, 400, "ERR CHUNKED\n");    return ESP_OK; }
    if (n < 0)                   { http_srv_text(req, 400, "ERR BAD_REQUEST\n"); return ESP_OK; }

    /* A form post or a text editor may add a trailing newline; the CLI takes
     * the line without one. */
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';

    cmd_slot_t *slot = claim();
    if (!slot) {
        http_srv_text(req, 503, "ERR BUSY\n");
        return ESP_OK;
    }

    /* 3500 ms: a forwarded ZONE command needs up to three ring ACK attempts
     * (SP3's forward budget), and cmd_dispatch is what enforces it -- this
     * only has to not give up first. */
    cmd_task_execute(&slot->ses, line, slot->resp, CMD_RESP_MAX, 3500);
    int status = (strncmp(slot->resp, "OK", 2) == 0) ? 200 : 422;
    http_srv_text(req, status, slot->resp);
    release(slot);
    return ESP_OK;
}

esp_err_t h_help(httpd_req_t *req) {
    const cmd_core_t *core = http_srv_core();
    if (!core) {
        http_srv_text(req, 500, "ERR INTERNAL\n");
        return ESP_OK;
    }

    /* cmd_help runs entirely on the table -- no dispatch, no forwarding -- so
     * it is safe to call straight from the httpd task. It still wants a
     * session for the role/unlock filtering, which is exactly why an HTTP
     * session is used here: the HELP text an operator sees over the web must
     * match what the web can actually run. */
    cmd_slot_t *slot = claim();
    if (!slot) {
        http_srv_text(req, 503, "ERR BUSY\n");
        return ESP_OK;
    }
    cmd_help(core, &slot->ses, NULL, 0, slot->resp, CMD_RESP_MAX);
    http_srv_text(req, 200, slot->resp);
    release(slot);
    return ESP_OK;
}
