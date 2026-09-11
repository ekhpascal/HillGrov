#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The master web UI's route table -- pure (no IDF headers), so the matcher and
 * the auth bits are host-tested. http_srv.c registers one httpd_uri_t per row
 * with .user_ctx = &HTTP_ROUTES[i] and one shared entry handler, which checks
 * the row's auth bit before dispatching on .id: adding a route means adding a
 * row here, and a row that forgets its auth bit fails test_http_routes. */

typedef enum { RT_LOGIN, RT_LOGOUT, RT_PASSWORD, RT_CMD, RT_HELP, RT_STATE, RT_SCHEMA, RT_CONFIG_GET, RT_CONFIG_PUT,
               RT_ALARMS, RT_FW_MASTER, RT_FW_ZONE, RT_FLEET_POST, RT_FLEET_DELETE, RT_WIFI_SCAN, RT_WIFI_SET,
               RT_INDEX, RT_APP_JS, RT_APP_CSS, RT_FLEET_BIN, RT_COUNT } route_id_t;

typedef struct { const char *method; const char *path; uint8_t auth; route_id_t id; } http_route_t;

extern const http_route_t HTTP_ROUTES[];   /* exactly RT_COUNT rows, in route_id_t order */
extern const int          HTTP_ROUTES_N;

/* Route id (>= 0) or -1 when nothing matches. The path is compared EXACTLY
 * (no prefix match, no trailing-slash tolerance) against everything before
 * the first '?', and the method must match exactly too -- so GET /api/login
 * is a 404, not the login handler with an empty body. *auth_out (optional) is
 * written only on a match; NULL method/uri return -1 rather than crash the
 * httpd worker. */
int http_route_find(const char *method, const char *uri, int *auth_out);

/* HTTP status for a CLI reply line, as answered by /api/cmd: 200 for "OK...",
 * 503 for the dispatcher's own "ERR BUSY" (its slot pool is full -- a capacity
 * answer, not a bad request), 500 for "ERR INTERNAL" (a dispatch that never
 * completed), 422 for every other ERR, i.e. everything the operator asked for
 * wrongly. Matches whole tokens, so "ERR BUSYNESS" is a 422. NULL -> 422. */
int http_reply_status(const char *reply);

#ifdef __cplusplus
}
#endif
