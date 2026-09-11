#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "hg_blob.h"      /* hg_crc32 */
#include "web_assets.h"
#include "http_srv_internal.h"

static const char *TAG = "http_static";

/* The three embedded gzip blobs, served straight out of flash: no copy, no
 * decompression, one send() of a pointer into rodata.
 *
 * Caching contract. The .gz bytes are byte-stable for a given build
 * (web_pack.py gzips with mtime=0), so their CRC-32 is a perfectly good
 * strong validator: a reflash with changed assets changes the ETag, a reflash
 * with identical assets does not, and an operator on a slow AP link
 * revalidates three files with three 304s instead of re-downloading 64 KB.
 * max-age=86400 keeps a browser from even asking for a day.
 *
 * The CRC is computed on first use and cached -- ~64 KB of CRC per asset,
 * once per boot, on the httpd task (the only task that reaches this file). */

typedef struct { const char *name; char etag[12]; } asset_cache_t;   /* "xxxxxxxx" + quotes + NUL */

static asset_cache_t s_cache[] = {
    { "index.html", "" },
    { "app.js",     "" },
    { "app.css",    "" },
};

static esp_err_t send_asset(httpd_req_t *req, int idx) {
    size_t      len   = 0;
    const char *ctype = NULL;
    const uint8_t *blob = web_asset(s_cache[idx].name, &len, &ctype);
    if (!blob || len == 0) {
        /* Only reachable if the embed step silently produced nothing, which
         * is exactly the failure WHOLE_ARCHIVE exists to prevent. */
        ESP_LOGE(TAG, "%s missing from the image", s_cache[idx].name);
        http_srv_error(req, 500, "NO_ASSET", s_cache[idx].name);
        return ESP_OK;
    }

    if (s_cache[idx].etag[0] == '\0')
        snprintf(s_cache[idx].etag, sizeof s_cache[idx].etag, "\"%08x\"",
                 (unsigned)hg_crc32(0, blob, len));

    /* The header may carry a list of validators, or a W/ prefix; the ETag is
     * quoted, so a substring test cannot match anything else. */
    char inm[96];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof inm) == ESP_OK &&
        strstr(inm, s_cache[idx].etag) != NULL) {
        httpd_resp_set_status(req, "304 Not Modified");
        httpd_resp_set_hdr(req, "ETag", s_cache[idx].etag);
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    }

    httpd_resp_set_type(req, ctype);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    httpd_resp_set_hdr(req, "ETag", s_cache[idx].etag);
    httpd_resp_send(req, (const char *)blob, len);
    return ESP_OK;
}

esp_err_t h_index(httpd_req_t *req)   { return send_asset(req, 0); }
esp_err_t h_app_js(httpd_req_t *req)  { return send_asset(req, 1); }
esp_err_t h_app_css(httpd_req_t *req) { return send_asset(req, 2); }
