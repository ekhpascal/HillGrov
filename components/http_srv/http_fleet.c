#include <string.h>
#include "cJSON.h"
#include "node_mgr.h"     /* HG_MAX_ZONES */
#include "psvc_fleet.h"
#include "http_srv_internal.h"

/* The fleet update button: POST /api/fleet starts SP3's fleet OTA sequencer
 * over one zone ({"zone":2}) or every assigned zone ({"all":true}), DELETE
 * cancels a running one. Neither touches flash -- the sequencer pulls the
 * zone_fw image http_upload_zone.c stored, over GET /fw/zone.bin, from each
 * zone in turn after rebooting it into rescue. The bodies are tiny, so this
 * uses http_srv_body() rather than the upload path's streaming reader.
 *
 * These two live next to the uploads rather than in http_api.c because they
 * are the other half of the same operator story (upload an image, then push
 * it), and because the upload handlers refuse to run while a sequence started
 * here is active. */

esp_err_t h_fleet_post(httpd_req_t *req) {
    char body[64];
    int n = http_srv_body(req, body, sizeof body);
    if (n == HTTP_BODY_TOO_LONG) { http_srv_error(req, 413, "TOO_LONG", NULL);    return http_srv_done(req, 0); }
    if (n == HTTP_BODY_CHUNKED)  { http_srv_error(req, 400, "CHUNKED", NULL);     return http_srv_done(req, 0); }
    if (n < 0)                   { http_srv_error(req, 400, "BAD_REQUEST", NULL); return http_srv_done(req, 0); }

    cJSON *root = cJSON_ParseWithLength(body, (size_t)n);
    if (!root) { http_srv_error(req, 400, "BAD_JSON", NULL); return http_srv_done(req, 1); }
    const cJSON *all  = cJSON_GetObjectItemCaseSensitive(root, "all");
    const cJSON *zone = cJSON_GetObjectItemCaseSensitive(root, "zone");

    /* cJSON stores every number as a double AND a truncated int, so
     * {"zone":2.7} would otherwise start a sequence on zone 2 (fix round 1):
     * require the two to agree, i.e. an integral value. */
    int zone_ok = cJSON_IsNumber(zone) && zone->valuedouble == (double)zone->valueint;

    psvc_rc_t rc;
    if (cJSON_IsTrue(all)) {
        rc = psvc_fleet_all();
    } else if (zone_ok && zone->valueint >= 1 && zone->valueint <= HG_MAX_ZONES) {
        rc = psvc_fleet_zone((uint8_t)zone->valueint);
    } else {
        cJSON_Delete(root);
        http_srv_error(req, 400, "INVALID", NULL);
        return http_srv_done(req, 1);
    }
    cJSON_Delete(root);

    if (rc != PSVC_OK) {
        /* psvc_rc_from_fleet: -2 = a sequence is already running (or an upload holds the gate) -> FLEET_BUSY,
         * any other refusal -> FLEET_REJECTED -- unchanged tokens, now shared with the panel. */
        http_srv_error(req, 409, psvc_rc_token(rc), NULL);
        return http_srv_done(req, 1);
    }
    http_srv_json(req, 202, "{\"queued\":true}");
    return http_srv_done(req, 1);
}

esp_err_t h_fleet_delete(httpd_req_t *req) {
    psvc_rc_t rc = psvc_fleet_abort();
    if (rc != PSVC_OK) {
        http_srv_error(req, 409, psvc_rc_token(rc), NULL);   /* NOT_ACTIVE */
        return http_srv_done(req, 0);
    }
    http_srv_json(req, 200, "{\"ok\":true}");
    return http_srv_done(req, 0);
}
