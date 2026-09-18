#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "hg_blob.h"      /* hg_crc32 */
#include "web_assets.h"
#include "http_srv_internal.h"

static const char *TAG = "http_static";

/* Whole-transfer budget for one asset. The three gzipped blobs total ~29 KB
 * and a healthy pull over the master's AP is well under a second, so 30 s is
 * ~50x a good transfer -- generous for a phone at the edge of the AP, and
 * still a bound. It has to exist: these routes are auth=0 (GET /app.js needs
 * no login), and without a deadline a peer advertising a one-byte receive
 * window keeps every send positive and pins the single httpd task for hours.
 * fw_srv.c's 120 s image budget is the same idea at image scale. */
#define ASSET_BUDGET_US (30LL * 1000 * 1000)

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
        return http_srv_done(req, 0);
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
        return http_srv_done(req, 0);
    }

    /* Hand-framed, exactly as fw_srv.c composes its image response and for the
     * same reason: httpd_resp_send() hands the whole ~26 KB blob to IDF's
     * httpd_send_all(), which loops while bytes remain and only aborts on a
     * NEGATIVE return -- send_wait_timeout catches only a send that moves zero
     * bytes, so a peer with a one-byte receive window keeps every send positive
     * and the single httpd task is pinned for hours on an unauthenticated
     * route. Composing the headers here lets the body go out through
     * http_srv_send_all() under a whole-transfer deadline. The header set is
     * exactly what httpd_resp_send() would have emitted (identity framing with
     * Content-Length), so the wire format and the caching contract above are
     * unchanged. */
    int64_t deadline = esp_timer_get_time() + ASSET_BUDGET_US;
    char head[256];
    int hn = snprintf(head, sizeof head,
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Encoding: gzip\r\n"
                      "Cache-Control: max-age=86400\r\nETag: %s\r\nContent-Length: %u\r\n\r\n",
                      ctype, s_cache[idx].etag, (unsigned)len);
    if (hn < 0 || (size_t)hn >= sizeof head) {
        ESP_LOGE(TAG, "%s: response header did not fit %u B", s_cache[idx].name, (unsigned)sizeof head);
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
    if (http_srv_send_all(req, head, (size_t)hn, deadline) != 0 ||
        http_srv_send_all(req, (const char *)blob, len, deadline) != 0) {
        /* Part of a response the client was promised Content-Length bytes of is
         * already on the wire: close, the way fw_srv.c does on a short image. */
        ESP_LOGW(TAG, "%s: send abandoned", s_cache[idx].name);
        return ESP_FAIL;
    }
    /* These are GETs, so there is normally no body at all and this keeps the
     * connection; a GET that announces one gets the socket closed instead of
     * letting httpd purge it on the single httpd task. */
    return http_srv_done(req, 0);
}

esp_err_t h_index(httpd_req_t *req)   { return send_asset(req, 0); }
esp_err_t h_app_js(httpd_req_t *req)  { return send_asset(req, 1); }
esp_err_t h_app_css(httpd_req_t *req) { return send_asset(req, 2); }
