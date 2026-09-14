#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "hg_json.h"
#include "node_mgr.h"
#include "mcfg_store.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "time_core.h"
#include "http_srv_internal.h"

static const char *TAG = "http_api_cfg";

/* zone=N&secrets=0|1 -- both optional, def on either a missing query string
 * or a missing/unparsable key. */
static long query_int(httpd_req_t *req, const char *key, long def) {
    char q[64], val[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return def;
    if (httpd_query_key_value(q, key, val, sizeof val) != ESP_OK) return def;
    return strtol(val, NULL, 10);
}

/* ---- GET /api/config?zone=N[&secrets=0] ---- */

esp_err_t h_config_get(httpd_req_t *req) {
    long zone    = query_int(req, "zone", 0);
    long secrets = query_int(req, "secrets", 1);

    static char doc[4096];
    int n;

    if (zone == 0) {
        n = hg_json_export_mcfg(mcfg_get(), secrets != 0, doc, sizeof doc);
    } else if (zone >= 1 && zone <= HG_MAX_ZONES) {
        hg_zone_cfg_t cfg;
        hg_zone_hw_t  hw;
        uint32_t      cfg_gen = 0;
        if (node_mgr_cfg_get((uint8_t)zone, &cfg, &hw, &cfg_gen, NULL) != 0) {
            http_srv_error(req, 404, "NO_CACHE", NULL);
            return http_srv_done(req, 0);
        }
        n = hg_json_export_cfg(&hw, &cfg, cfg_gen, doc, sizeof doc);
    } else {
        http_srv_error(req, 404, "ZONE_UNKNOWN", NULL);
        return http_srv_done(req, 0);
    }

    if (n < 0) {
        ESP_LOGE(TAG, "config export overflowed its 4 KB buffer (zone %ld)", zone);
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 0);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, doc, n);
    return http_srv_done(req, 0);
}

/* ---- PUT /api/config?zone=N ---- */

/* zone 1..8: merge applies the "cfg" plane only -- "hw" is read-only from the
 * web (system spec §4.4) and comes back as warnings, never merged (see
 * hg_json_merge_cfg's own doc comment). Status mapping is the controller
 * ruling verbatim, cross-checked against node_mgr_cfg_api.c's own comments. */
static esp_err_t cfg_put_zone(httpd_req_t *req, uint8_t zone, const char *body) {
    if (node_mgr_cfg_busy(zone)) {
        http_srv_error(req, 409, "BUSY", NULL);
        return http_srv_done(req, 1);
    }

    hg_zone_cfg_t cfg;
    hg_zone_hw_t  hw;
    if (node_mgr_cfg_get(zone, &cfg, &hw, NULL, NULL) != 0) {
        http_srv_error(req, 404, "NO_CACHE", NULL);
        return http_srv_done(req, 1);
    }

    char err[96]  = "";
    char warn[256] = "";
    int rc = hg_json_merge_cfg(&hw, &cfg, body, err, sizeof err, warn, sizeof warn);
    if (rc == -1) { http_srv_error(req, 400, "BAD_JSON", NULL);     return http_srv_done(req, 1); }
    if (rc == -2) { http_srv_error(req, 400, "INVALID_FIELD", err); return http_srv_done(req, 1); }
    if (rc == -3) { http_srv_error(req, 400, "VALIDATION", err);    return http_srv_done(req, 1); }

    rc = node_mgr_cfg_set(zone, &cfg);
    if (rc == -1) { http_srv_error(req, 404, "ZONE_UNKNOWN", NULL);    return http_srv_done(req, 1); }
    if (rc == -2) { http_srv_error(req, 409, "BUSY", NULL);            return http_srv_done(req, 1); }
    if (rc == -3) { http_srv_error(req, 409, "ZONE_NOT_ONLINE", NULL); return http_srv_done(req, 1); }

    char resp[320];
    snprintf(resp, sizeof resp, "{\"queued\":true,\"warnings\":\"%s\"}", warn);
    http_srv_json(req, 202, resp);
    return http_srv_done(req, 1);
}

/* zone 0: the master's own mcfg. mcfg_commit() has no err_path out for its
 * own -1 verdict, so the copy is validated here first, with the same checker
 * time_svc_start() now wires into both mcfg_store and hg_json's own merge-
 * time validate -- once that wiring is in place this call is defensive
 * (the merge below already ran an identical check), but it is what lets a
 * bad TIME.TZ (or any other mcfg field mcfg_commit's stricter check might
 * still catch) report a path instead of a bare 400. */
static esp_err_t cfg_put_zone0(httpd_req_t *req, const char *body) {
    hg_mcfg_t scratch = *mcfg_get();

    char err[64] = "";
    int rc = hg_json_merge_mcfg(&scratch, body, err, sizeof err);
    if (rc == -1) { http_srv_error(req, 400, "BAD_JSON", NULL);     return http_srv_done(req, 1); }
    if (rc == -2) { http_srv_error(req, 400, "INVALID_FIELD", err); return http_srv_done(req, 1); }

    char verr[64] = "";
    if (hg_mcfg_validate(&scratch, tz_check, verr, sizeof verr) != 0) {
        http_srv_error(req, 400, "VALIDATION", verr);
        return http_srv_done(req, 1);
    }

    rc = mcfg_commit(&scratch);
    if (rc == -1) { http_srv_error(req, 400, "VALIDATION", NULL); return http_srv_done(req, 1); }   /* defensive: see above */
    if (rc == -2) { http_srv_error(req, 503, "STORAGE", NULL);    return http_srv_done(req, 1); }

    wifi_mgr_apply();
    time_svc_apply_mcfg();
    http_srv_json(req, 200, "{\"ok\":true}");
    return http_srv_done(req, 1);
}

esp_err_t h_config_put(httpd_req_t *req) {
    if (esp_get_free_heap_size() < 40 * 1024) {
        http_srv_error(req, 503, "LOW_HEAP", NULL);
        return http_srv_done(req, 0);
    }

    static char body[4097];   /* cap-1 == 4096: "body > 4096 -> 413" exactly */
    int n = http_srv_body(req, body, sizeof body);
    if (n == HTTP_BODY_TOO_LONG) { http_srv_error(req, 413, "TOO_LONG", NULL);    return http_srv_done(req, 0); }
    if (n == HTTP_BODY_CHUNKED)  { http_srv_error(req, 400, "CHUNKED", NULL);     return http_srv_done(req, 0); }
    if (n < 0)                   { http_srv_error(req, 400, "BAD_REQUEST", NULL); return http_srv_done(req, 0); }

    long zone = query_int(req, "zone", 0);
    if (zone == 0) return cfg_put_zone0(req, body);
    if (zone < 1 || zone > HG_MAX_ZONES) {
        http_srv_error(req, 404, "ZONE_UNKNOWN", NULL);
        return http_srv_done(req, 1);
    }
    return cfg_put_zone(req, (uint8_t)zone, body);
}
