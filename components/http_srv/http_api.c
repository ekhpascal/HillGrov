#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "state_snap.h"
#include "hg_json.h"
#include "alarm_mgr.h"
#include "wifi_mgr.h"
#include "node_mgr.h"
#include "app_if_common.h"
#include "mcfg_store.h"
#include "mcfg_ops.h"
#include "master_cmds.h"   /* net_ops_t -- a component header, safe to include */
#include "http_srv_internal.h"

static const char *TAG = "http_api";

/* net_ops_master.{h,c} live in master/main -- an app, not a component -- so
 * this component cannot include that header (same reason http_login.c
 * extern-declares master_web_set_password rather than including it). This is
 * the whole contract this file needs from it; the lock itself now comes from
 * components/mcfg_ops (Task 5), which both this file and http_api_cfg.c take
 * the SAME instance of. */
extern const net_ops_t *master_net_ops(void);

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
    snap_master_t m;
    memset(&m, 0, sizeof m);

    m.version     = esp_app_get_description()->version;
    m.uptime_s    = hg_app_uptime_s();
    m.heap_min_kb = esp_get_minimum_free_heap_size() / 1024;

    /* hg_app_time_get_noted's fixed "YYYY-MM-DD HH:MM:SS <SRC> <age_s>" shape
     * (app_if_common.c): the first 19 chars go verbatim into m.time, the next
     * whitespace-delimited token is the source. m.time_src is a const char*
     * into this stack buffer, which lives for the rest of this call -- long
     * enough to be read once by state_snap_write() below. */
    char tbuf[48];
    char time_src_buf[8] = "";
    hg_app_time_get_noted(tbuf, sizeof tbuf);
    size_t tlen = strlen(tbuf);
    /* Bounded memcpy rather than snprintf("%s", ...): tbuf can (defensively)
     * hold more than m.time's 19+NUL capacity, and GCC's format-truncation
     * check cannot see that the normal-case branch below never does -- a
     * plain memcpy of a runtime-computed, always-in-range length sidesteps
     * that check outright instead of arguing with it. */
    size_t tcopy = tlen < 19 ? tlen : 19;
    memcpy(m.time, tbuf, tcopy);
    m.time[tcopy] = '\0';
    if (tlen >= 19) sscanf(tbuf + 19, " %7s", time_src_buf);
    m.time_src = time_src_buf;

    wifi_status_t w;
    memset(&w, 0, sizeof w);
    wifi_mgr_status(&w);
    m.sta.up = w.sta_up;
    snprintf(m.sta.ip, sizeof m.sta.ip, "%s", w.sta_ip);
    snprintf(m.sta.ssid, sizeof m.sta.ssid, "%s", w.sta_ssid);
    m.sta.rssi = w.rssi;
    snprintf(m.sta.reason, sizeof m.sta.reason, "%s", w.sta_reason);
    snprintf(m.ap.ssid, sizeof m.ap.ssid, "%s", w.ap_ssid);
    m.ap.clients = w.ap_clients;
    snprintf(m.ap.ip, sizeof m.ap.ip, "%s", w.ap_ip);

    /* hg_app_fw_info's fixed "<version> <slot> <state> <other>" shape
     * (app_if_common.c): version duplicates m.version above (skipped here),
     * "other" is always the literal "NONE" on a master (no second-slot
     * concept), kept as a distinct token anyway to match the shared format. */
    char fwbuf[96];
    char fw_slot[16] = "", fw_state[16] = "", fw_other[16] = "";
    hg_app_fw_info(fwbuf, sizeof fwbuf);
    sscanf(fwbuf, "%*s %15s %15s %15s", fw_slot, fw_state, fw_other);
    m.fw.slot  = fw_slot;
    m.fw.state = fw_state;
    m.fw.other = fw_other;

    const char *upload_kind = "";
    uint8_t     upload_pct  = 0;
    http_upload_progress(&upload_kind, &upload_pct);   /* Task 13 fills this in */
    m.fw.upload_kind = upload_kind;
    m.fw.upload_pct  = upload_pct;

    char fleet_buf[32];
    node_mgr_fw_status(fleet_buf, sizeof fleet_buf);
    m.fleet_line = fleet_buf;

    m.alarms_active = alarm_mgr_active_count();
    m.alarms_total  = alarm_mgr_total();

    uint8_t flags = mcfg_get()->flags;
    m.web_default = (flags & MCFG_F_WEB_DEFAULT) ? 1 : 0;
    m.ap_default  = (flags & MCFG_F_AP_DEFAULT)  ? 1 : 0;

    m.cmd_quarantined = http_cmd_quarantined();

    /* Node table copy under node_mgr's own lock (node_mgr_get), one slot at a
     * time -- node_mgr_get leaves *out untouched on an empty/unused slot, so
     * the memset above is load-bearing, not decorative. */
    hg_node_t tab[HG_MAX_ZONES];
    memset(tab, 0, sizeof tab);
    for (int i = 0; i < HG_MAX_ZONES; i++) (void)node_mgr_get(i, &tab[i]);

    uint8_t cfg_sync_failed[HG_MAX_ZONES];
    for (int i = 0; i < HG_MAX_ZONES; i++)
        cfg_sync_failed[i] = (uint8_t)node_mgr_cfg_sync_failed((uint8_t)(i + 1));

    ring_status_t rs;
    memset(&rs, 0, sizeof rs);
    node_mgr_ring_status(&rs);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);   /* same clock as node_mgr's last_hb_ms */
    int rc = state_snap_write(&m, tab, HG_MAX_ZONES, &rs, cfg_sync_failed, now_ms, chunk_writer, req);
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
    /* wifi_mgr_scan() blocks ~2 s and parks the single radio -- must not run
     * concurrently with a net_ops apply (SET WIFI STA/AP/TZ, SET WEB
     * PASSWORD, or this same handler's own PUT /api/config?zone=0 path),
     * which all take this same net_ops_master mutex internally. */
    if (mcfg_ops_lock(100) != 0) {
        http_srv_error(req, 409, "BUSY", NULL);
        return http_srv_done(req, 0);
    }
    wifi_scan_t out[20];
    int n = wifi_mgr_scan(out, 20);
    mcfg_ops_unlock();

    if (n < 0) {
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
