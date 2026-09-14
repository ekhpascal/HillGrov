#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "hg_blob.h"   /* hg_crc32 */
#include "fw_srv.h"

static const char *TAG = "fw_srv";

#define FW_HDR_LEN   16u
#define FW_HDR_MAGIC 0x57464748u   /* 'HGFW' LE -- hg_cfg_types.h's HG_MAGIC_* convention */
#define FW_CHUNK     4096u
/* 120 s: a healthy 1.5 MB pull over the master's AP takes ~12 s. */
#define XFER_BUDGET_US (120LL * 1000 * 1000)

static const esp_partition_t *s_part;
static uint32_t                s_img_len;
static uint8_t                 s_img_ok;

/* Shared by validate_image() (fw_srv_validate(), at boot) and zone_bin_get()
 * (the httpd task, later) -- never touched concurrently (fix round minor #8:
 * app_main calls fw_srv_validate() before http_srv_start(), so validation
 * always finishes before any request can arrive and one 4 KB buffer is enough
 * for both). */
static uint8_t s_buf[FW_CHUNK];

/* esp_task_wdt_reset() is NOT a quiet no-op for a task that is not
 * subscribed: it logs an ESP_LOGE("task not found") every time (bench: ~60
 * error lines at boot, one per 4 KB chunk, from fw_srv_validate() -- and 384
 * for a full-size image). Both loops below run in two different worlds --
 * fw_srv_validate() at boot on an unsubscribed task, fw_srv_revalidate() and
 * zone_bin_get() on the httpd task inside its own subscription -- so the
 * reset is gated on the subscription rather than called blind.
 * esp_task_wdt_status() is silent when the answer is "not subscribed". */
static void wdt_kick(void) {
    if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
}

/* Explicit little-endian byte-offset reads (C code rule: wire/persisted
 * data is packed by explicit offset, never via struct-cast) -- mirrors
 * hg_blob.c's rd32(). */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One-time validation, streamed through s_buf (the image can be up to
 * ~1.5 MB -- far too big to read in one shot). hg_crc32 chains across
 * chunks by feeding the previous result back in as the next seed (hg_blob.h:
 * "seed 0 for one-shot; feed previous result to continue"). */
static int validate_image(const esp_partition_t *part, uint32_t *len_out) {
    uint8_t hdr[FW_HDR_LEN];
    if (esp_partition_read(part, 0, hdr, FW_HDR_LEN) != ESP_OK) return -1;
    if (rd32(hdr) != FW_HDR_MAGIC) return -1;
    uint32_t len = rd32(hdr + 4);
    uint32_t want_crc = rd32(hdr + 8);
    if (len == 0 || (uint64_t)FW_HDR_LEN + len > part->size) return -1;

    uint32_t rem = len, off = FW_HDR_LEN, crc = 0;
    while (rem) {
        /* Silent at boot (that caller is not subscribed); load-bearing for
         * fw_srv_revalidate(), which runs on the TWDT-subscribed httpd task
         * right after a browser upload and re-reads the whole image. */
        wdt_kick();
        uint32_t n = rem < FW_CHUNK ? rem : FW_CHUNK;
        if (esp_partition_read(part, off, s_buf, n) != ESP_OK) return -1;
        crc = hg_crc32(crc, s_buf, n);
        off += n;
        rem -= n;
    }
    if (crc != want_crc) return -1;

    *len_out = len;
    return 0;
}

/* fix round 2: httpd_send() returns the byte count of ONE send() call,
 * which can be short under the socket's 5 s SO_SNDTIMEO on a marginal link
 * -- exactly the edge-of-AP fleet-OTA case this server exists for. IDF's
 * own higher-level resp API loops internally (the private httpd_send_all);
 * a hand-rolled send path has to do the same, or a short write silently
 * advances the cursor past bytes that were never actually sent, desyncing
 * the stream against the Content-Length already promised to the client.
 * Returns 0 once all len bytes are sent, -1 on any ret <= 0 (error or the
 * peer/httpd closing the socket). */
static int send_all(httpd_req_t *r, const char *buf, size_t len, int64_t deadline_us) {
    size_t sent = 0;
    while (sent < len) {
        /* Fix round 1: feeding the dog on progress (below) removed the only
         * bound on how long a slow-READING client can hold the one httpd task
         * -- and this route is the UNAUTHENTICATED one (a zone in rescue has
         * no cookie). The caller's whole-transfer deadline is that bound:
         * ~10x a healthy 1.5 MB pull, after which the response is abandoned,
         * the socket closed by the ESP_FAIL path, and rescue retries. */
        if (esp_timer_get_time() > deadline_us) {
            ESP_LOGW(TAG, "zone.bin transfer budget spent with %u B to go -- closing",
                     (unsigned)(len - sent));
            return -1;
        }
        int n = httpd_send(r, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
        /* SP4 Task 13 bench fix: each httpd_send() can block for the socket's
         * full 5 s SO_SNDTIMEO and still come back with only a partial count
         * on a marginal AP link, so two slow partial sends inside ONE 4 KB
         * chunk already exceed the 8 s TWDT -- and the caller below only
         * resets between chunks. That is not a hypothetical: a fleet pull to
         * a zone in rescue tripped it on the bench and PANIC=y rebooted the
         * master mid-update (the zone's own retry then recovered, but the
         * master lost its sequencer state and never reported DONE). Feeding
         * the dog on PROGRESS keeps hang detection intact: a peer that has
         * genuinely stopped reading makes httpd_send return <= 0 after one
         * 5 s timeout and the loop above bails out to close the socket. */
        wdt_kick();
    }
    return 0;
}

/* SP4 Task 13: re-run the one-time validation after POST /api/fw/zone has
 * rewritten the partition (http_upload_zone.c). Callable ONLY from the httpd
 * task: it reuses the same s_buf that zone_bin_get() streams from, and both
 * only ever run on that one task (see s_buf's comment above). Unlike
 * fw_srv_validate() it also clears s_img_len on a bad verdict -- validate_image
 * leaves *len_out untouched when it fails, and a stale length behind
 * s_img_ok = 0 is a trap for anything that ever reads the two together.
 * 0 = the partition now holds a good image, -1 = it does not (which is the
 * expected answer when the caller has deliberately erased the header). */
int fw_srv_revalidate(void) {
    if (!s_part) return -1;
    s_img_ok = (validate_image(s_part, &s_img_len) == 0) ? 1 : 0;
    if (!s_img_ok) s_img_len = 0;
    ESP_LOGI(TAG, "zone_fw revalidated: %s (%lu B)", s_img_ok ? "ok" : "no image", (unsigned long)s_img_len);
    return s_img_ok ? 0 : -1;
}

/* fix round 1 CRITICAL fix: httpd_resp_send_chunk() UNCONDITIONALLY emits
 * "Transfer-Encoding: chunked" (IDF httpd_txrx.c), regardless of whether
 * the caller also set a Content-Length header -- the two together trip
 * esp_http_client's strict parser (HPE_UNEXPECTED_CONTENT_LENGTH), which
 * is exactly what the zone's rescue_pull() client uses. curl tolerates
 * the malformed dual-header response (which is why this only showed up
 * against a real rescue pull, not a bench curl smoke test). So this
 * handler composes the identity-framed response BY HAND -- status line +
 * headers + blank line via one send_all(), then the body via send_all()
 * chunks -- and never touches httpd_resp_send_chunk/httpd_resp_set_hdr at
 * all. */
/* How this handler returns (SP4 Task 11 record item, settled in Task 13) --
 * the same rule http_srv_done() states for every other handler on the shared
 * instance, applied by hand here because this file deliberately does not use
 * http_srv's helpers:
 *   404 FW_NO_IMAGE -> ESP_OK. This is a GET with no request body, so there
 *       is nothing for httpd to purge and the connection may be kept; the
 *       rescue client closes it itself after reading the 404.
 *   any failure after the headers are on the wire -> ESP_FAIL, so httpd
 *       CLOSES the socket. The client has been promised Content-Length bytes
 *       it is not going to get, and a truncated body on a kept-alive
 *       connection would be parsed as the next response. The close is the
 *       only honest framing left. */
static esp_err_t zone_bin_get(httpd_req_t *req) {
    if (!s_img_ok) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "FW_NO_IMAGE", HTTPD_RESP_USE_STRLEN);   /* identity by default: fine as-is */
        return ESP_OK;
    }

    /* Whole-transfer budget, shared by the header send and every body chunk:
     * ~10x what a healthy 1.5 MB pull over the AP takes (fix round 1). It is
     * the one bound on a slow-reading client of this unauthenticated route
     * now that the watchdog is fed on progress. */
    int64_t deadline = esp_timer_get_time() + XFER_BUDGET_US;

    char head[128];
    int hn = snprintf(head, sizeof head,
                       "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\n\r\n",
                       (unsigned long)s_img_len);
    if (hn < 0 || (size_t)hn >= sizeof head || send_all(req, head, (size_t)hn, deadline) != 0) return ESP_FAIL;

    /* The httpd worker task isn't TWDT-subscribed by default -- add/delete
     * around the loop, reset every chunk (SP1 rescue-upload pattern,
     * rescue_http.c's upload_post). */
    esp_task_wdt_add(NULL);
    uint32_t rem = s_img_len, off = FW_HDR_LEN;
    esp_err_t rc = ESP_OK;
    while (rem) {
        esp_task_wdt_reset();
        uint32_t n = rem < FW_CHUNK ? rem : FW_CHUNK;
        if (esp_partition_read(s_part, off, s_buf, n) != ESP_OK ||
            send_all(req, (const char *)s_buf, n, deadline) != 0) {
            rc = ESP_FAIL;   /* ret<=0 from send_all: httpd already closed the socket -- correct for a truncated response */
            break;
        }
        off += n;
        rem -= n;
    }
    esp_task_wdt_delete(NULL);
    return rc;
}

int fw_srv_validate(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zone_fw");
    if (!s_part) {
        ESP_LOGE(TAG, "zone_fw partition not found");
        return -1;
    }

    s_img_ok = (validate_image(s_part, &s_img_len) == 0) ? 1 : 0;
    if (!s_img_ok)
        ESP_LOGW(TAG, "zone_fw image missing/invalid (magic/len/crc) -- GET /fw/zone.bin will 404 FW_NO_IMAGE");

    return 0;
}

/* The master serves this path from the one shared httpd instance (Task 11);
 * the 4096-byte stack this server used to run on is gone with it -- the
 * shared instance runs at 8192, which the 4 KB static chunk buffer and the
 * hand-framed response are comfortably inside. */
int fw_srv_register(httpd_handle_t server) {
    static const httpd_uri_t get_zone_bin = { .uri = "/fw/zone.bin", .method = HTTP_GET, .handler = zone_bin_get };
    esp_err_t rc = httpd_register_uri_handler(server, &get_zone_bin);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "register GET /fw/zone.bin failed: %s", esp_err_to_name(rc));
        return -1;
    }
    return 0;
}

int fw_srv_image_ok(void) { return s_img_ok; }
