#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_log.h"
#include "node_mgr.h"
#include "http_upload.h"
#include "http_srv_internal.h"

static const char *TAG = "http_upload";

/* Sized like rescue_http.c's upload buffer; static, because the httpd task's
 * 8 KB stack is shared with /api/cmd's dispatch path and state_snap's writer.
 * One buffer for both uploads is safe: s_busy makes them mutually exclusive
 * (and esp_http_server runs every socket from one task anyway). */
#define UPLOAD_BUF   4096
/* rescue's rule verbatim: this many CONSECUTIVE recv timeouts (~60 s of no
 * data at all at httpd's 5 s recv_wait_timeout) gives up rather than letting
 * a client that walked out of AP range mid-upload pin the single httpd task
 * with a half-written slot open. */
#define MAX_TIMEOUTS 12
/* KraftWerk lesson: yield after this many flash writes so the Wi-Fi and IDLE
 * tasks still run during a ~1 MB flash burst. */
#define YIELD_BLOCKS 8
/* esp_image_header_t (24) + esp_image_segment_header_t (8) + the offset of
 * project_name inside esp_app_desc_t (48) + its length (32). The first recv
 * is NOT guaranteed to return this much, so the check waits for the bytes to
 * accumulate across recv boundaries. */
#define ID_MIN_BYTES 112
#define ID_NAME_OFF  (32 + 48)
#define ID_NAME_LEN  32

#define LOW_HEAP_B   (40 * 1024)   /* same guard as PUT /api/config */

/* ---- exclusivity ----
 * One upload at a time, claimed test-and-set under a critical section. The
 * fleet sequencer is excluded separately (node_mgr_fw_status): it pulls the
 * zone_fw partition a zone upload erases, and it runs on node_mgr's task, so
 * "the httpd task is busy" is not enough to keep the two apart. */
static portMUX_TYPE s_busy_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_busy;

static int busy_claim(void) {
    int got = 0;
    portENTER_CRITICAL(&s_busy_mux);
    if (!s_busy) { s_busy = 1; got = 1; }
    portEXIT_CRITICAL(&s_busy_mux);
    return got;
}

static void busy_release(void) {
    portENTER_CRITICAL(&s_busy_mux);
    s_busy = 0;
    portEXIT_CRITICAL(&s_busy_mux);
}

/* ---- progress (see http_upload.h for why this is lock-free) ---- */
static const char *volatile s_kind = "";
static volatile uint32_t    s_pct;

static void progress(const char *kind, uint32_t pct) {
    s_pct  = pct;
    s_kind = kind;   /* published last: a reader never sees a kind without a pct */
}

int http_upload_progress(const char **kind, uint8_t *pct) {
    const char *k = s_kind;
    uint32_t    p = s_pct;
    if (kind) *kind = k ? k : "";
    if (pct)  *pct  = (uint8_t)(p > 100 ? 100 : p);
    return (k && *k) ? 1 : 0;
}

/* ---- the shared upload path ---- */

static int identity_ok(const uint8_t *b, const char *want) {
    if (b[0] != 0xE9) return 0;   /* esp_image_header_t.magic */
    const char *name = (const char *)b + ID_NAME_OFF;
    size_t n = strnlen(name, ID_NAME_LEN);
    return n == strlen(want) && memcmp(name, want, n) == 0;
}

static int type_is_octet_stream(httpd_req_t *req) {
    char ct[48];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK) return 0;
    static const char want[] = "application/octet-stream";
    size_t n = sizeof want - 1;
    if (strncasecmp(ct, want, n) != 0) return 0;
    return ct[n] == '\0' || ct[n] == ';' || ct[n] == ' ';   /* parameters are allowed */
}

/* Reads away the rest of a body this handler has already decided to refuse.
 * 1 = the body is fully consumed (the connection may be kept), 0 = the peer
 * stalled or went away (the caller closes instead).
 *
 * Bench finding (Task 13): closing the socket on a client that is still
 * streaming -- http_srv_done's usual answer for an unread body -- makes the
 * peer's TCP stack discard whatever of our response it had already buffered
 * (the close RSTs the connection because unread data is pending). curl showed
 * "Recv failure: Connection was reset" and only the status line survived; a
 * browser would see the fetch reject outright. Since the operator's commonest
 * mistake -- the wrong image in the wrong endpoint -- is exactly the case that
 * has to explain itself, the body is drained first and the answer sent into a
 * quiet socket. That is affordable ONLY here: by this point the size guard has
 * already bounded content_len by the target partition, and the drain gives up
 * after 3 consecutive timeouts (~15 s) rather than following a trickle. */
#define DRAIN_MAX_TIMEOUTS 3

static int drain_body(httpd_req_t *req, uint8_t *buf, size_t cap, size_t got) {
    int timeouts = 0;
    while (got < req->content_len) {
        esp_task_wdt_reset();
        size_t want = req->content_len - got;
        if (want > cap) want = cap;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > DRAIN_MAX_TIMEOUTS) return 0;
            continue;
        }
        if (n <= 0) return 0;
        timeouts = 0;
        got += (size_t)n;
    }
    return 1;
}

/* Every exit from here answers exactly once and returns through
 * http_srv_done(): drained = 1 only when the whole body was read, so a
 * refusal with a body still on the wire closes the socket instead of letting
 * httpd purge megabytes on the single httpd task. */
static esp_err_t fw_upload(httpd_req_t *req, const char *kind, const char *want_name,
                           size_t max_len, const upload_sink_t *sink) {
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0) {
        http_srv_error(req, 400, "CHUNKED_UNSUPPORTED", NULL);
        return http_srv_done(req, 0);
    }
    if (!type_is_octet_stream(req)) {
        http_srv_error(req, 400, "BAD_TYPE", NULL);
        return http_srv_done(req, 0);
    }
    if (req->content_len == 0) {
        http_srv_error(req, 400, "EMPTY_BODY", NULL);
        return http_srv_done(req, 0);
    }

    char fleet[40] = "";
    node_mgr_fw_status(fleet, sizeof fleet);
    if (strcmp(fleet, "IDLE") != 0) {
        http_srv_error(req, 409, "FLEET_ACTIVE", NULL);
        return http_srv_done(req, 0);
    }
    if (!busy_claim()) {
        http_srv_error(req, 409, "UPLOAD_ACTIVE", NULL);
        return http_srv_done(req, 0);
    }
    /* from here on out every exit goes through busy_release() */

    const char *code = NULL;
    int status = 0;
    if (esp_get_free_heap_size() < LOW_HEAP_B) { status = 503; code = "LOW_HEAP"; }
    else if (max_len == 0)                     { status = 500; code = "INTERNAL"; }
    else if (req->content_len > max_len)       { status = 413; code = "TOO_LARGE"; }
    if (code) {
        ESP_LOGW(TAG, "%s upload refused: %s (%u B, max %u)", kind, code,
                 (unsigned)req->content_len, (unsigned)max_len);
        /* Refused before a single body byte was read, so the socket is closed
         * rather than drained (see drain_body): content_len is whatever the
         * client claimed -- TOO_LARGE means it is bigger than any partition
         * here by definition -- and reading megabytes away just to be polite
         * about the error body would hand any logged-in client the single
         * httpd task for as long as it liked. The status code still reaches
         * the client; only the JSON body can be lost to the reset. */
        http_srv_error(req, status, code, NULL);
        busy_release();
        return http_srv_done(req, 0);
    }

    ESP_LOGW(TAG, "%s upload starting: %u B", kind, (unsigned)req->content_len);
    progress(kind, 0);
    esp_task_wdt_add(NULL);   /* the httpd task is not TWDT-subscribed by default */

    static uint8_t buf[UPLOAD_BUF];
    size_t got = 0, fill = 0;
    int timeouts = 0, blocks = 0, started = 0, sock_ok = 1;

    while (got < req->content_len) {
        esp_task_wdt_reset();
        size_t want = req->content_len - got;
        if (want > UPLOAD_BUF - fill) want = UPLOAD_BUF - fill;

        int n = httpd_req_recv(req, (char *)buf + fill, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > MAX_TIMEOUTS) {
                ESP_LOGW(TAG, "%s upload stalled (~%d s of silence), aborting", kind, MAX_TIMEOUTS * 5);
                status = 400; code = "STALLED"; sock_ok = 0;
                break;
            }
            continue;
        }
        if (n <= 0) { status = 400; code = "RECV_FAILED"; sock_ok = 0; break; }
        timeouts = 0;
        fill += (size_t)n;
        got  += (size_t)n;
        progress(kind, (uint32_t)((uint64_t)got * 100 / req->content_len));

        /* Identify the image BEFORE the first write, so nothing is erased for
         * a file that was never going to be accepted: the brief's order
         * (erase, then check) would cost the stored zone image every time
         * someone picks the wrong file in the browser. */
        if (!started && (fill >= ID_MIN_BYTES || got == req->content_len)) {
            if (fill < ID_MIN_BYTES || !identity_ok(buf, want_name)) {
                ESP_LOGW(TAG, "%s upload is not a %s image", kind, want_name);
                status = 422; code = "IMAGE_MISMATCH";
                break;
            }
            if (sink->begin(req->content_len) != 0) { status = 422; code = "WRITE_FAILED"; break; }
            started = 1;
        }
        if (started && (fill == UPLOAD_BUF || got == req->content_len)) {
            if (sink->write(buf, fill) != 0) { status = 422; code = "WRITE_FAILED"; break; }
            fill = 0;
            if (++blocks % YIELD_BLOCKS == 0) vTaskDelay(1);
        }
    }

    char resp[128];
    int drained = (got == req->content_len);
    if (!code && sink->finish(resp, sizeof resp) != 0) { status = 422; code = "WRITE_FAILED"; }
    if (code) {
        if (started) sink->cancel();
        ESP_LOGE(TAG, "%s upload failed after %u/%u B: %s", kind, (unsigned)got,
                 (unsigned)req->content_len, code);
        /* Drain BEFORE answering, never after: curl stops sending as soon as
         * it sees an error status ("HTTP error before end of send"), so a
         * drain that ran afterwards would wait out its own timeout budget for
         * bytes that are never coming. */
        if (!drained && sock_ok) drained = drain_body(req, buf, sizeof buf, got);
    }

    /* Every flash write and every recv is done by here. The response send
     * below is bounded by the socket's own 5 s send timeout and can legitimately
     * take several seconds on a marginal AP link, so the TWDT subscription --
     * which exists to catch a stuck flash/recv loop, not a slow client -- is
     * dropped first. fw_srv.c's send_all() carries the same lesson the hard
     * way: a slow-but-healthy send panicked the board on the bench. */
    esp_task_wdt_delete(NULL);
    progress("", 0);
    busy_release();

    if (code) http_srv_error(req, status, code, NULL);
    else      http_srv_json(req, 200, resp);
    return http_srv_done(req, drained);
}

esp_err_t h_fw_master(httpd_req_t *req) {
    int rc = http_upload_master_ready();   /* -1 no slot, -2 the running image is still on trial */
    if (rc != 0) {
        http_srv_error(req, rc == -2 ? 409 : 500, rc == -2 ? "TRIAL_PENDING" : "NO_SLOT", NULL);
        return http_srv_done(req, 0);
    }
    return fw_upload(req, "master", "hillgrow_master", http_upload_master_max(), http_upload_master_sink());
}

esp_err_t h_fw_zone(httpd_req_t *req) {
    return fw_upload(req, "zone", "hillgrow_zone", http_upload_zone_max(), http_upload_zone_sink());
}
