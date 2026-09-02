#include <stdio.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "rescue.h"

static const char *TAG = "rescue_http";

extern const uint8_t upload_html_start[] asm("_binary_upload_html_start");
/* upload_html_end unused: EMBED_TXTFILES NUL-terminates the blob, so the
 * start pointer alone is a valid C string. */

static esp_timer_handle_t s_reboot_timer;

static void reboot_cb(void *arg) {
    (void)arg;
    esp_restart();
}

/* Delays the reboot by 1 s so the httpd response for this request has time
 * to reach the client before the socket (and the AP) disappears. */
static void schedule_reboot(void) {
    if (!s_reboot_timer) {
        const esp_timer_create_args_t args = { .callback = reboot_cb, .name = "reboot" };
        esp_timer_create(&args, &s_reboot_timer);
    }
    esp_timer_start_once(s_reboot_timer, 1000000);
}

/* Copies src into dst (cap bytes incl. NUL), replacing each "%RUNNING%" with
 * running and each "%SLOTS%" with slots. Returns the written length, or -1
 * if the result wouldn't fit in dst. */
static int render_page(const char *src, char *dst, size_t cap, const char *running, const char *slots) {
    size_t o = 0;
    for (const char *p = src; *p; ) {
        const char *rep = NULL;
        size_t tag_len = 0;
        if (!strncmp(p, "%RUNNING%", 9)) { rep = running; tag_len = 9; }
        else if (!strncmp(p, "%SLOTS%", 7)) { rep = slots; tag_len = 7; }
        if (rep) {
            size_t rl = strlen(rep);
            if (o + rl >= cap) return -1;
            memcpy(dst + o, rep, rl);
            o += rl;
            p += tag_len;
        } else {
            if (o + 1 >= cap) return -1;
            dst[o++] = *p++;
        }
    }
    dst[o] = '\0';
    return (int)o;
}

/* esp_partition_t.label is char[17] (<=16 chars), esp_app_desc_t.version is
 * char[32] (<=31 chars); size generously so gcc's -Wformat-truncation can
 * prove "label: version" always fits. */
#define SLOT_LINE_MAX 56

static void slot_describe(char *out, size_t cap, const esp_partition_t *p) {
    /* Zero-initialized so a failed/skipped read leaves desc.version as an
     * empty string rather than uninitialized stack bytes; %.31s bounds the
     * read to the field's declared size regardless, since char[32] read
     * back from flash is not guaranteed to contain a NUL. */
    esp_app_desc_t desc = { 0 };
    if (p && esp_ota_get_partition_description(p, &desc) == ESP_OK) {
        snprintf(out, cap, "%s: %.31s", p->label, desc.version);
    } else {
        snprintf(out, cap, "%s: empty", p ? p->label : "?");
    }
}

static esp_err_t root_get(httpd_req_t *req) {
    const esp_partition_t *ota0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    const esp_partition_t *ota1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    char s0[SLOT_LINE_MAX], s1[SLOT_LINE_MAX], slots[2 * SLOT_LINE_MAX + 8];
    slot_describe(s0, sizeof s0, ota0);
    slot_describe(s1, sizeof s1, ota1);
    snprintf(slots, sizeof slots, "%s, %s", s0, s1);

    static char page[4096];
    int n = render_page((const char *)upload_html_start, page, sizeof page,
                         esp_app_get_description()->version, slots);
    if (n < 0) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, "page render overflow", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, page, n);
    return ESP_OK;
}

/* httpd's default recv_wait_timeout is 5 s (HTTPD_DEFAULT_CONFIG); this many
 * *consecutive* timeouts (~60 s of no data at all) gives up rather than
 * retrying forever. httpd is single-threaded: a client that vanishes
 * without a FIN (e.g. a phone walking out of AP range mid-upload) would
 * otherwise wedge the entire recovery server -- including /reboot -- with
 * the OTA handle open on a half-written slot, recoverable only by a power
 * cycle. (review-round fix, IMPORTANT 2) */
#define UPLOAD_MAX_CONSECUTIVE_TIMEOUTS 12

static esp_err_t upload_post(httpd_req_t *req) {
    const esp_partition_t *dst = rescue_target_slot();
    if (!dst || req->content_len == 0 || req->content_len > dst->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad content-length or no OTA slot free");
        return ESP_OK;
    }

    /* The httpd worker task isn't TWDT-subscribed by default, so without
     * this the esp_task_wdt_reset() below is a silent no-op (review-round
     * fix, minor 4). Deleted again below on every path out of this
     * function past this point. */
    esp_task_wdt_add(NULL);

    esp_ota_handle_t ota = 0;
    int rc = -1;
    if (esp_ota_begin(dst, req->content_len, &ota) == ESP_OK) {
        static char buf[4096];
        int remaining = (int)req->content_len;
        int timeouts = 0;
        rc = 0;
        while (remaining > 0) {
            esp_task_wdt_reset();
            int chunk = remaining < (int)sizeof buf ? remaining : (int)sizeof buf;
            int n = httpd_req_recv(req, buf, chunk);
            if (n == HTTPD_SOCK_ERR_TIMEOUT) {
                if (++timeouts > UPLOAD_MAX_CONSECUTIVE_TIMEOUTS) {
                    ESP_LOGW(TAG, "upload stalled (no data for ~%d s), aborting",
                             UPLOAD_MAX_CONSECUTIVE_TIMEOUTS * 5);
                    rc = -1;
                    break;
                }
                continue;
            }
            timeouts = 0;
            if (n <= 0 || esp_ota_write(ota, buf, (size_t)n) != ESP_OK) { rc = -1; break; }
            remaining -= n;
        }
        if (rc == 0 && esp_ota_end(ota) != ESP_OK) rc = -1;
        else if (rc != 0) esp_ota_abort(ota);
        if (rc == 0 && esp_ota_set_boot_partition(dst) != ESP_OK) rc = -1;
    }

    esp_task_wdt_delete(NULL);

    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "flash write failed");
        return ESP_OK;
    }

    /* Zero-initialized + %.31s bounded: see slot_describe's comment above --
     * a failed read (checked here, unlike before) leaves desc.version as an
     * empty string instead of stack garbage or an unbounded read. */
    esp_app_desc_t desc = { 0 };
    if (esp_ota_get_partition_description(dst, &desc) != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_get_partition_description failed after a successful write to %s", dst->label);
    }
    char resp[64];
    int rl = snprintf(resp, sizeof resp, "OK %.31s \xe2\x80\x94 rebooting\n", desc.version);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, resp, rl);
    ESP_LOGW(TAG, "upload -> %s (%.31s), rebooting", dst->label, desc.version);
    schedule_reboot();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK \xe2\x80\x94 rebooting\n", HTTPD_RESP_USE_STRLEN);
    schedule_reboot();
    return ESP_OK;
}

void rescue_http_start(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    static const httpd_uri_t upload = { .uri = "/upload", .method = HTTP_POST, .handler = upload_post };
    static const httpd_uri_t reboot = { .uri = "/reboot", .method = HTTP_POST, .handler = reboot_post };
    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &upload);
    httpd_register_uri_handler(server, &reboot);
}
