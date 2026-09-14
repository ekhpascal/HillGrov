#pragma once
#include "esp_http_server.h"
#include "cmd_core.h"
#include "web_auth.h"
#include "http_routes.h"
#include "http_srv.h"

/* Cross-file wiring inside the http_srv component. Not installed for other
 * components: everything a caller outside http_srv needs is in http_srv.h. */

/* ---- http_srv.c ---- */

const cmd_core_t *http_srv_core(void);   /* the core passed to http_srv_start() */

/* How every handler must return (see the block comment in http_srv.c): ESP_OK
 * lets httpd keep the connection AND purge any unread request body, which a
 * trickling client with a huge Content-Length can stretch out indefinitely on
 * the one httpd task. Pass drained = 1 only when the whole body was read;
 * anything else closes the socket after the response is already sent. */
esp_err_t http_srv_done(httpd_req_t *req, int drained);

/* Reads the whole request body into buf and NUL-terminates it. Returns the
 * byte count (>= 0) or one of the negative codes below WITHOUT sending
 * anything -- the caller answers in its own content type, because /api/cmd
 * speaks text/plain while the JSON routes speak JSON. */
#define HTTP_BODY_CHUNKED  (-1)   /* Transfer-Encoding present -> 400 */
#define HTTP_BODY_TOO_LONG (-2)   /* content_len > cap-1        -> 413 */
#define HTTP_BODY_IO       (-3)   /* recv error/timeout/socket  -> 400 */
int http_srv_body(httpd_req_t *req, char *buf, size_t cap);

/* The 501 every route whose handler lands in a later task answers with, so
 * the route table and the auth gate are complete from day one. */
esp_err_t h_not_impl(httpd_req_t *req);

/* ---- http_auth.c: the locked facade over the shared wa_state_t ----
 * Every one of these takes http_auth's mutex internally; nothing outside
 * http_auth.c ever sees the wa_state_t itself. */

int  http_auth_check(const char *cookie_hdr);                         /* 0 valid / -1 not */
int  http_auth_login(const char *pw, char cookie_out[2 * WA_TOKEN_LEN + 1]);  /* 0 / -1 bad / -2 locked */
int  http_auth_verify_password(const char *pw);                       /* 0 correct / -1 wrong; no session, no fail count */
void http_auth_logout_cookie(const char *cookie_hdr);

/* ---- http_login.c: the HTTP surface over that facade ---- */
esp_err_t h_login(httpd_req_t *req);
esp_err_t h_logout(httpd_req_t *req);
esp_err_t h_password(httpd_req_t *req);

/* ---- http_cmd.c ---- */
int http_cmd_init(void);   /* the two HTTP cmd_session_t slots + their claim mutex */
esp_err_t h_cmd(httpd_req_t *req);
esp_err_t h_help(httpd_req_t *req);   /* its own cmd_session_t -- never claims a /api/cmd slot */
uint8_t http_cmd_quarantined(void);   /* count of /api/cmd slots permanently withdrawn (state_snap) */

/* ---- http_static.c ---- */
esp_err_t h_index(httpd_req_t *req);
esp_err_t h_app_js(httpd_req_t *req);
esp_err_t h_app_css(httpd_req_t *req);

/* ---- http_api.c / http_api_cfg.c ----
 * The JSON API: state, schema, config (zone >= 1 through node_mgr's §4.4
 * primitives, zone 0 = the master's own mcfg), alarms and Wi-Fi. Config GET
 * and PUT are split into http_api_cfg.c to keep both files under the
 * project's ~300-line guideline; everything else lives in http_api.c. */

/* Builds the /api/schema cache once (the schema is constant for the life of
 * the image). Call once from http_srv_start(), before the server can take a
 * request. 0 ok, -1 if the schema did not fit its cache buffer (logged;
 * h_schema then answers 500 rather than serve a truncated document). */
int http_api_init(void);

esp_err_t h_state(httpd_req_t *req);
esp_err_t h_schema(httpd_req_t *req);
esp_err_t h_config_get(httpd_req_t *req);
esp_err_t h_config_put(httpd_req_t *req);
esp_err_t h_alarms(httpd_req_t *req);
esp_err_t h_wifi_scan(httpd_req_t *req);
esp_err_t h_wifi_set(httpd_req_t *req);

/* ---- http_upload.c (+ _master.c / _zone.c) and http_fleet.c ----
 * The two firmware upload endpoints -- raw application/octet-stream bodies,
 * master -> the inactive OTA slot, zone -> the zone_fw partition behind the
 * HGFW header -- and the fleet update button. http_upload.c holds the guards
 * and the one streaming loop; the two sinks are a file each (they share
 * nothing but the upload_sink_t contract); http_fleet.c is the pair of
 * node_mgr calls behind the button. http_upload.h carries the guard list and
 * the upload-progress getter /api/state reads. */
#include "http_upload.h"

esp_err_t h_fw_master(httpd_req_t *req);
esp_err_t h_fw_zone(httpd_req_t *req);
esp_err_t h_fleet_post(httpd_req_t *req);
esp_err_t h_fleet_delete(httpd_req_t *req);
