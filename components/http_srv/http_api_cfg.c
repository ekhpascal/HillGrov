#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "hg_json.h"
#include "node_mgr.h"
#include "mcfg_store.h"
#include "wifi_mgr.h"
#include "time_svc.h"
#include "time_core.h"
#include "http_srv_internal.h"

static const char *TAG = "http_api_cfg";

/* net_ops_master.{h,c} live in master/main -- an app, not a component -- so
 * this component cannot include that header (same reason http_api.c and
 * http_login.c extern-declare rather than include). cfg_put_zone0 below
 * takes this same lock the CLI's SET WIFI/TZ rows take internally
 * (net_ops_master.c's net_set_sta/net_set_ap/net_set_tz), so a concurrent
 * console command and a zone-0 PUT can never interleave their own
 * snapshot -> modify -> commit -> apply sequences. */
extern int  master_net_ops_try_lock(uint32_t ms);
extern void master_net_ops_unlock(void);

/* zone=N&secrets=0|1, both optional. Review fix round 1 (CRITICAL #3): a
 * present-but-unparsable value must NOT silently fall back to the default --
 * ?zone=abc silently becoming zone 0 would let a UI bug (or a fat-fingered
 * curl) target the master's own mcfg instead of failing loudly. Tri-state:
 * 0 = key absent, caller uses its own default; 1 = parsed, *out set; -1 =
 * present but not a plain integer (trailing garbage, empty value, no digits
 * at all) -- caller must answer 400 BAD_QUERY, not proceed. */
static int query_int(httpd_req_t *req, const char *key, long *out) {
    char q[64], val[8];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return 0;
    if (httpd_query_key_value(q, key, val, sizeof val) != ESP_OK) return 0;
    if (val[0] == '\0') return -1;
    char *end = NULL;
    long v = strtol(val, &end, 10);
    if (!end || *end != '\0') return -1;
    *out = v;
    return 1;
}

/* ---- GET /api/config?zone=N[&secrets=0] ---- */

esp_err_t h_config_get(httpd_req_t *req) {
    long zone = 0, secrets = 1;
    if (query_int(req, "zone", &zone) < 0 || query_int(req, "secrets", &secrets) < 0) {
        http_srv_error(req, 400, "BAD_QUERY", NULL);
        return http_srv_done(req, 0);
    }

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

    /* Review fix round 1 (CRITICAL #2): warn is comma-joined path text built
     * from hg_json_merge_cfg's own field/group names, but its unknown-key
     * branch (hgj_warn/hgj_path in hg_json_internal.h) embeds the JSON key
     * verbatim as typed by the client -- e.g. a body key of
     * `a":1,"pwned":true,"x":"` was going straight into a hand-built
     * "{\"queued\":true,\"warnings\":\"%s\"}" format string with no
     * escaping, breaking out of the JSON string. cJSON_Print does the
     * escaping (quotes, backslashes, control chars) properly, so the
     * response is built through it instead of snprintf. */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "queued", 1);
    cJSON_AddStringToObject(resp, "warnings", warn);
    char *body_out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!body_out) {
        http_srv_error(req, 500, "INTERNAL", NULL);
        return http_srv_done(req, 1);
    }
    http_srv_json(req, 202, body_out);
    cJSON_free(body_out);
    return http_srv_done(req, 1);
}

/* zone 0: the master's own mcfg. mcfg_commit() has no err_path out for its
 * own -1 verdict, so the copy is validated here first, with the same checker
 * time_svc_start() now wires into both mcfg_store and hg_json's own merge-
 * time validate -- once that wiring is in place this call is defensive
 * (the merge below already ran an identical check), but it is what lets a
 * bad TIME.TZ (or any other mcfg field mcfg_commit's stricter check might
 * still catch) report a path instead of a bare 400.
 *
 * Review fix round 1 (CRITICAL #1): the whole snapshot -> merge -> validate
 * -> commit -> wifi_mgr_apply -> time_svc_apply_mcfg sequence now runs under
 * master_net_ops_try_lock() -- the exact mutex net_ops_master.c's own
 * net_set_sta/net_set_ap/net_set_tz already hold across their own identical
 * sequence, named in that file's own comment as being for this handler.
 * Without it, a concurrent console `SET WIFI STA`/`SET TZ` and this PUT both
 * snapshot mcfg_get() before either commits, and the second commit silently
 * overwrites the first's field with its own stale copy of everything else --
 * exactly the race net_ops_master.c's mutex exists to close, which this
 * handler was bypassing entirely by going straight to mcfg_commit(). 100 ms
 * try-lock, same budget h_wifi_scan uses; 409 BUSY on failure to acquire. */
static esp_err_t cfg_put_zone0(httpd_req_t *req, const char *body) {
    if (master_net_ops_try_lock(100) != 0) {
        http_srv_error(req, 409, "BUSY", NULL);
        return http_srv_done(req, 1);
    }

    hg_mcfg_t scratch = *mcfg_get();

    char err[64] = "";
    int rc = hg_json_merge_mcfg(&scratch, body, err, sizeof err);
    if (rc == -1) {
        master_net_ops_unlock();
        http_srv_error(req, 400, "BAD_JSON", NULL);
        return http_srv_done(req, 1);
    }
    if (rc == -2) {
        master_net_ops_unlock();
        http_srv_error(req, 400, "INVALID_FIELD", err);
        return http_srv_done(req, 1);
    }

    char verr[64] = "";
    if (hg_mcfg_validate(&scratch, tz_check, verr, sizeof verr) != 0) {
        master_net_ops_unlock();
        http_srv_error(req, 400, "VALIDATION", verr);
        return http_srv_done(req, 1);
    }

    rc = mcfg_commit(&scratch);
    if (rc == -1) {   /* defensive: see above -- the pre-check should have already caught this */
        master_net_ops_unlock();
        http_srv_error(req, 400, "VALIDATION", NULL);
        return http_srv_done(req, 1);
    }
    if (rc == -2) {
        master_net_ops_unlock();
        http_srv_error(req, 503, "STORAGE", NULL);
        return http_srv_done(req, 1);
    }

    wifi_mgr_apply();
    time_svc_apply_mcfg();
    master_net_ops_unlock();
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

    long zone = 0;
    if (query_int(req, "zone", &zone) < 0) {
        http_srv_error(req, 400, "BAD_QUERY", NULL);
        return http_srv_done(req, 1);   /* body already read above */
    }
    if (zone == 0) return cfg_put_zone0(req, body);
    if (zone < 1 || zone > HG_MAX_ZONES) {
        http_srv_error(req, 404, "ZONE_UNKNOWN", NULL);
        return http_srv_done(req, 1);
    }
    return cfg_put_zone(req, (uint8_t)zone, body);
}
