#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"      /* CONFIG_IDF_TARGET_ESP32P4 (h_state) */
#include "esp_log.h"
#include "cJSON.h"
#include "state_snap.h"
#include "hg_json.h"
#include "alarm_mgr.h"
#include "wifi_mgr.h"
#include "master_cmds.h"   /* net_ops_t -- a component header, safe to include */
#include "psvc_net.h"   /* master_net_ops(), psvc_wifi_scan() -- components/panel_svc */
#include "psvc_state.h"
#include "http_srv_internal.h"

static const char *TAG = "http_api";

/* ---- GET /api/schema: built once, served forever ---- */

#define SCHEMA_CAP 6144
static char s_schema_buf[SCHEMA_CAP];
static int  s_schema_len = -1;

int http_api_init(void) {
    s_schema_len = hg_json_schema(s_schema_buf, sizeof s_schema_buf);
    if (s_schema_len < 0) ESP_LOGE(TAG, "hg_json_schema overflowed its %d B cache", SCHEMA_CAP);
    return s_schema_len < 0 ? -1 : 0;
}

esp_err_t h_schema(httpd_req_t *req) {
    if (s_schema_len < 0) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, s_schema_buf, s_schema_len);
    return http_srv_done(req, 0);
}

/* ---- GET /api/state ---- */

/* Wraps httpd_resp_send_chunk as a snap_write_fn: 0 ok / -1 the send failed
 * (state_snap_write stops immediately on a nonzero return, per its own
 * contract, and never calls this again for that response). */
static int chunk_writer(void *ctx, const char *buf, size_t n) {
    return httpd_resp_send_chunk((httpd_req_t *)ctx, buf, n) == ESP_OK ? 0 : -1;
}

esp_err_t h_state(httpd_req_t *req) {
    /* The gather is panel_svc's (panel plan Task 9): the panel's poller runs
     * the same psvc_state_fill(). ~1.9 KB. On the ESP32 master it lives on
     * the httpd task's 8 KB stack (http_srv.c cfg.stack_size), as h_state's
     * own gather did before Task 9: internal RAM there has no room for it as
     * permanent .bss (the 64 KB heap-min bar). On the P4 it stays static --
     * httpd serves every socket from ONE task, so h_state never runs twice
     * at once. */
#if CONFIG_IDF_TARGET_ESP32P4
    static psvc_state_t s;
#else
    psvc_state_t s;
#endif
    psvc_state_fill(&s, PSVC_FILL_ALL);
    snap_master_t m;
    psvc_state_to_snap(&s, &m);   /* m points into s, which outlives the write below */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    int rc = state_snap_write(&m, s.node, HG_MAX_ZONES, &s.ring, s.cfg_sync_failed, s.now_ms, chunk_writer, req);
    if (rc != 0) return ESP_FAIL;   /* a chunk send already failed; nothing more to send */

    httpd_resp_send_chunk(req, NULL, 0);
    return http_srv_done(req, 0);   /* GET: no request body to drain */
}

/* ---- GET /api/alarms ---- */

esp_err_t h_alarms(httpd_req_t *req) {
    static char buf[6144];
    int n = alarm_mgr_json(buf, sizeof buf);
    if (n < 0) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, buf, n);
    return http_srv_done(req, 0);
}

/* ---- GET /api/wifi/scan ---- */

esp_err_t h_wifi_scan(httpd_req_t *req) {
    /* wifi_mgr_scan() parks the single radio for the whole scan, so it must not
     * run concurrently with a master-config apply (SET WIFI STA/AP/TZ, SET WEB
     * PASSWORD, a zone-0 PUT or a panel save): psvc_wifi_scan() holds the
     * components/mcfg_ops lock across the scan, which every one of those takes
     * across its own commit AND apply. 100 ms try, exactly as before. */
    wifi_scan_t out[20];
    int n = 0;
    psvc_rc_t src = psvc_wifi_scan(out, 20, &n, PSVC_LOCK_WEB_MS);
    if (src == PSVC_E_BUSY) {
        http_srv_error(req, 409, "BUSY", NULL);
        return http_srv_done(req, 0);
    }
    if (src != PSVC_OK) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", out[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", out[i].rssi);
        cJSON_AddNumberToObject(o, "auth", out[i].auth);
        cJSON_AddItemToArray(arr, o);
    }
    char *body = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!body) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return http_srv_done(req, 0);
}

/* ---- POST /api/wifi ---- */

esp_err_t h_wifi_set(httpd_req_t *req) {
    /* Review fix round 1 (IMPORTANT #4): a legal 63-char password (or a
     * 32-char ssid) built entirely of '"'/'\\' doubles under JSON-string
     * escaping on the wire, so 192 B was not actually enough headroom for a
     * legal worst-case body -- sized for 2x(32+63) plus punctuation/keys. */
    char body[384];
    int n = http_srv_body(req, body, sizeof body);
    if (n == HTTP_BODY_TOO_LONG) { http_srv_error(req, 413, "TOO_LONG", NULL);    return http_srv_done(req, 0); }
    if (n == HTTP_BODY_CHUNKED)  { http_srv_error(req, 400, "CHUNKED", NULL);     return http_srv_done(req, 0); }
    if (n < 0)                   { http_srv_error(req, 400, "BAD_REQUEST", NULL); return http_srv_done(req, 0); }

    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    if (!root) { http_srv_error(req, 400, "BAD_JSON", NULL); return http_srv_done(req, 1); }

    cJSON *sta = cJSON_GetObjectItemCaseSensitive(root, "sta");
    cJSON *ap  = cJSON_GetObjectItemCaseSensitive(root, "ap");
    cJSON *target = (sta && !ap) ? sta : (ap && !sta) ? ap : NULL;   /* exactly one of the two */

    const cJSON *ssid_j = target ? cJSON_GetObjectItemCaseSensitive(target, "ssid") : NULL;
    const cJSON *pass_j = target ? cJSON_GetObjectItemCaseSensitive(target, "pass") : NULL;
    const char *ssid = (cJSON_IsString(ssid_j) && ssid_j->valuestring) ? ssid_j->valuestring : NULL;
    const char *pass = (cJSON_IsString(pass_j) && pass_j->valuestring) ? pass_j->valuestring : NULL;

    if (!target || !ssid || !pass) {
        cJSON_Delete(root);
        http_srv_error(req, 400, "INVALID", NULL);
        return http_srv_done(req, 1);
    }

    /* net_ops_t's set_sta/set_ap already serialize themselves against every
     * other net_ops write (via components/mcfg_ops's mcfg_ops_edit()) -- this
     * handler must NOT also hold mcfg_ops_lock() around the call, that mutex
     * is not recursive. */
    const net_ops_t *net = master_net_ops();
    int rc = sta ? net->set_sta(ssid, pass) : net->set_ap(ssid, pass);
    cJSON_Delete(root);

    if (rc != 0) {
        int status = rc == -2 ? 503 : rc == -3 ? 500 : 400;
        const char *code = rc == -2 ? "STORAGE" : rc == -3 ? "INTERNAL" : "INVALID";
        http_srv_error(req, status, code, NULL);
        return http_srv_done(req, 1);
    }
    http_srv_json(req, 200, "{\"ok\":true}");
    return http_srv_done(req, 1);
}
