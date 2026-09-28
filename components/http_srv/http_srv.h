#pragma once
#include <stdint.h>
#include "esp_http_server.h"
#include "cmd_core.h"
#include "hg_mcfg.h"
#include "http_auth.h"   /* the web-session store (components/http_auth) -- every http_auth_* entry point */

#ifdef __cplusplus
extern "C" {
#endif

/* The master's ONE HTTP server: port 80, a single esp_http_server instance
 * shared by the web UI, the CLI-over-HTTP endpoint and SP3's fleet-firmware
 * pull. Every request enters through one handler (route_entry) which decides
 * authentication from the pure, host-tested table in http_routes.h before any
 * per-route code runs -- this is the product's security boundary, so there is
 * deliberately no second way in.
 *
 * Threading: esp_http_server serves every socket from a single task, so the
 * route handlers never run concurrently with each other. They DO run
 * concurrently with the CLI task, ring/node_mgr tasks and the fleet
 * sequencer, so all shared state reached from here is either owned by a
 * mutex (http_auth's wa_state_t, panel_svc's mcfg read-modify-write) or
 * handed over through cmd_task's queue. */

/* Starts the instance and registers every row of HTTP_ROUTES plus, via
 * fw_srv_register(), GET /fw/zone.bin. core is the command table the
 * /api/cmd and /api/help endpoints run against; it must outlive the server
 * (app_main's static cmd_core_t). 0 ok, -1 on any failure (all logged) --
 * boot continues either way, but ota_trial_drivers_ok() must not be called
 * after a failure (spec 3.10: the drivers criterion is AP netif + httpd). */
int  http_srv_start(const cmd_core_t *core);

/* Cookie gate: 0 = not authenticated, and a 401 {"error":"UNAUTHORIZED"} has
 * already been sent (the handler must still return through
 * http_srv_done(req, drained), using whatever drained value applies to its
 * own request -- never a bare ESP_OK, or an unread body on that request
 * would be left for httpd to purge, see http_srv_done's own comment); 1 = go
 * ahead. route_entry calls this for every auth=1 row, so a handler only
 * needs it when it is reached some other way. */
int  http_srv_auth_ok(httpd_req_t *req);

/* application/json + status line + send; json may be NULL for an empty body. */
void http_srv_json(httpd_req_t *req, int status, const char *json);

/* A bare 204: no body, and -- unlike anything httpd_resp_send can produce --
 * no Content-Length and no Content-Type either, because a 204 has no
 * representation to describe. cookie is an optional Set-Cookie value, copied
 * into the response here. Returns 0 once the response is on the wire, -1 on a
 * header-overflow (a 500 was sent instead) or a short/failed send -- either
 * way the caller must NOT return http_srv_done(req, drained) in that case:
 * the client cannot be trusted to have received a well-formed response, so
 * the caller returns ESP_FAIL outright and lets httpd close the socket. */
int http_srv_no_content(httpd_req_t *req, const char *cookie);

/* text/plain + status line + send, for the CLI-shaped endpoints whose body is
 * the command reply verbatim rather than JSON. */
void http_srv_text(httpd_req_t *req, int status, const char *text);

/* {"error":"<code>","path":"<path>"} -- path is sanitised (it is
 * attacker-controlled) and may be NULL to omit the member. */
void http_srv_error(httpd_req_t *req, int status, const char *code, const char *path);

#ifdef __cplusplus
}
#endif
