#include <stdio.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_http_server.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "hg_blob.h"   /* hg_crc32 */
#include "fw_srv.h"

static const char *TAG = "fw_srv";

#define FW_HDR_LEN   16u
#define FW_HDR_MAGIC 0x57464748u   /* 'HGFW' LE -- hg_cfg_types.h's HG_MAGIC_* convention */
#define FW_CHUNK     4096u

static const esp_partition_t *s_part;
static uint32_t                s_img_len;
static uint8_t                 s_img_ok;

/* Shared by validate_image() (fw_srv_start(), before httpd exists) and
 * zone_bin_get() (the httpd worker, after) -- never touched concurrently
 * (fix round minor #8: validation always finishes before httpd_start()
 * even runs, so one 4 KB buffer is enough for both). */
static uint8_t s_buf[FW_CHUNK];

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
static int send_all(httpd_req_t *r, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = httpd_send(r, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
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
static esp_err_t zone_bin_get(httpd_req_t *req) {
    if (!s_img_ok) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "FW_NO_IMAGE", HTTPD_RESP_USE_STRLEN);   /* identity by default: fine as-is */
        return ESP_OK;
    }

    char head[128];
    int hn = snprintf(head, sizeof head,
                       "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %lu\r\n\r\n",
                       (unsigned long)s_img_len);
    if (hn < 0 || (size_t)hn >= sizeof head || send_all(req, head, (size_t)hn) != 0) return ESP_FAIL;

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
            send_all(req, (const char *)s_buf, n) != 0) {
            rc = ESP_FAIL;   /* ret<=0 from send_all: httpd already closed the socket -- correct for a truncated response */
            break;
        }
        off += n;
        rem -= n;
    }
    esp_task_wdt_delete(NULL);
    return rc;
}

int fw_srv_start(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "zone_fw");
    if (!s_part) {
        ESP_LOGE(TAG, "zone_fw partition not found");
        return -1;
    }

    s_img_ok = (validate_image(s_part, &s_img_len) == 0) ? 1 : 0;
    if (!s_img_ok)
        ESP_LOGW(TAG, "zone_fw image missing/invalid (magic/len/crc) -- GET /fw/zone.bin will 404 FW_NO_IMAGE");

    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return -1;
    }

    static const httpd_uri_t get_zone_bin = { .uri = "/fw/zone.bin", .method = HTTP_GET, .handler = zone_bin_get };
    if (httpd_register_uri_handler(server, &get_zone_bin) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler failed");
        return -1;
    }

    return 0;
}

int fw_srv_image_ok(void) { return s_img_ok; }
