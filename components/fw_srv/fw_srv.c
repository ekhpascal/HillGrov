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

/* Explicit little-endian byte-offset reads (C code rule: wire/persisted
 * data is packed by explicit offset, never via struct-cast) -- mirrors
 * hg_blob.c's rd32(). */
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One-time validation, streamed through a stack buffer (the image can be
 * up to ~1.5 MB -- far too big to read in one shot). hg_crc32 chains across
 * chunks by feeding the previous result back in as the next seed (hg_blob.h:
 * "seed 0 for one-shot; feed previous result to continue"). */
static int validate_image(const esp_partition_t *part, uint32_t *len_out) {
    uint8_t hdr[FW_HDR_LEN];
    if (esp_partition_read(part, 0, hdr, FW_HDR_LEN) != ESP_OK) return -1;
    if (rd32(hdr) != FW_HDR_MAGIC) return -1;
    uint32_t len = rd32(hdr + 4);
    uint32_t want_crc = rd32(hdr + 8);
    if (len == 0 || (uint64_t)FW_HDR_LEN + len > part->size) return -1;

    static uint8_t buf[FW_CHUNK];
    uint32_t rem = len, off = FW_HDR_LEN, crc = 0;
    while (rem) {
        uint32_t n = rem < FW_CHUNK ? rem : FW_CHUNK;
        if (esp_partition_read(part, off, buf, n) != ESP_OK) return -1;
        crc = hg_crc32(crc, buf, n);
        off += n;
        rem -= n;
    }
    if (crc != want_crc) return -1;

    *len_out = len;
    return 0;
}

static esp_err_t zone_bin_get(httpd_req_t *req) {
    if (!s_img_ok) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_send(req, "FW_NO_IMAGE", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    char clen[16];
    snprintf(clen, sizeof clen, "%lu", (unsigned long)s_img_len);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Length", clen);   /* set -> IDF httpd sends identity (not chunked) framing */

    /* The httpd worker task isn't TWDT-subscribed by default -- add/delete
     * around the loop, reset every chunk (SP1 rescue-upload pattern,
     * rescue_http.c's upload_post). */
    esp_task_wdt_add(NULL);
    static uint8_t buf[FW_CHUNK];
    uint32_t rem = s_img_len, off = FW_HDR_LEN;
    esp_err_t rc = ESP_OK;
    while (rem) {
        esp_task_wdt_reset();
        uint32_t n = rem < FW_CHUNK ? rem : FW_CHUNK;
        if (esp_partition_read(s_part, off, buf, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) {
            rc = ESP_FAIL;
            break;
        }
        off += n;
        rem -= n;
    }
    if (rc == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);   /* terminate the response */
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
