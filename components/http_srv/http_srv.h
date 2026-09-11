#pragma once
#include <stdint.h>
#include "esp_http_server.h"
#include "cmd_core.h"
#include "hg_mcfg.h"

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
 * mutex (http_auth's wa_state_t, net_ops_master's mcfg read-modify-write) or
 * handed over through cmd_task's queue. */

/* Starts the instance and registers every row of HTTP_ROUTES plus, via
 * fw_srv_register(), GET /fw/zone.bin. core is the command table the
 * /api/cmd and /api/help endpoints run against; it must outlive the server
 * (app_main's static cmd_core_t). 0 ok, -1 on any failure (all logged) --
 * boot continues either way, but ota_trial_drivers_ok() must not be called
 * after a failure (spec 3.10: the drivers criterion is AP netif + httpd). */
int  http_srv_start(const cmd_core_t *core);

/* One-time init of the shared web-auth state: PSA crypto + web_auth_init()
 * over the session array, then the persisted sessions from NVS ("hg"/"sess").
 * Idempotent. Call it at boot once nvs_flash_init() has run -- app_main does,
 * and so does http_srv_start(), because a boot whose radio never came up
 * still has to be able to hash a console SET WEB PASSWORD. Until it has
 * succeeded every entry point answers as if the board had no crypto (a login
 * fails, a password change reports ERR INTERNAL) rather than touching
 * uninitialised state. 0 ok, -1 if crypto or the mutex is unavailable. */
int  http_auth_init(void);

/* Cookie gate: 0 = not authenticated, and a 401 {"error":"UNAUTHORIZED"} has
 * already been sent (the handler must just return ESP_OK); 1 = go ahead.
 * route_entry calls this for every auth=1 row, so a handler only needs it
 * when it is reached some other way. */
int  http_srv_auth_ok(httpd_req_t *req);

/* application/json + status line + send. json may be NULL/"" for 204. */
void http_srv_json(httpd_req_t *req, int status, const char *json);

/* text/plain + status line + send, for the CLI-shaped endpoints whose body is
 * the command reply verbatim rather than JSON. */
void http_srv_text(httpd_req_t *req, int status, const char *text);

/* {"error":"<code>","path":"<path>"} -- path is sanitised (it is
 * attacker-controlled) and may be NULL to omit the member. */
void http_srv_error(httpd_req_t *req, int status, const char *code, const char *path);

/* ---- shared web-password state (master/main/net_ops_master.c) ----
 * http_srv owns the ONE wa_state_t: it holds the live login sessions AND the
 * sha/rand hooks web_auth needs, so a password change made from the CLI and
 * one made over HTTP go through the same state and both drop the sessions. */

/* Puts a fresh salt + sha256(salt||pw) into *m and clears MCFG_F_WEB_DEFAULT,
 * without touching NVS -- the caller still owns the mcfg_commit(). Does NOT
 * drop sessions (see http_auth_sessions_drop, to be called only after the
 * commit succeeded). 0 ok, -1 pw outside 8..63, -3 SHA-256 unavailable (the
 * board's crypto is broken -- committing the all-zero digest would lock the
 * web UI out for good, so *m is left untouched). */
int  http_auth_hash_password(hg_mcfg_t *m, const char *pw);

/* Invalidates every live web session and rewrites NVS. Call right after a
 * successful password commit: the old cookies must not outlive the password
 * they were issued against. */
void http_auth_sessions_drop(void);

#ifdef __cplusplus
}
#endif
