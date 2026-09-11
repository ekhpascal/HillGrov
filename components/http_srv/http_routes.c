#include <string.h>
#include "http_routes.h"

/* The pure half of http_srv: the route table with its auth bits, the matcher,
 * and the CLI-reply -> HTTP-status mapping. No IDF headers, so all three are
 * host-tested (tests/host/test_http_routes.c).
 *
 * One row per route_id_t, in id order (the tests pin both). auth = 1 means
 * route_entry() calls http_srv_auth_ok() -- i.e. a valid hg_sess cookie --
 * before the handler runs.
 *
 * The three auth = 0 groups are deliberate:
 *   POST /api/login   the gate itself
 *   GET / /app.js /app.css   the login page has to load before there is a
 *                            cookie; the assets carry no greenhouse data
 *   GET /fw/zone.bin  SP3's rescue pull. A zone rebooted into rescue has no
 *                     cookie and no way to get one; the AP's WPA2 password is
 *                     that path's gate, exactly as in SP3. */
const http_route_t HTTP_ROUTES[] = {
    { "POST",   "/api/login",     0, RT_LOGIN        },
    { "POST",   "/api/logout",    1, RT_LOGOUT       },
    { "POST",   "/api/password",  1, RT_PASSWORD     },
    { "POST",   "/api/cmd",       1, RT_CMD          },
    { "GET",    "/api/help",      1, RT_HELP         },
    { "GET",    "/api/state",     1, RT_STATE        },
    { "GET",    "/api/schema",    1, RT_SCHEMA       },
    { "GET",    "/api/config",    1, RT_CONFIG_GET   },
    { "PUT",    "/api/config",    1, RT_CONFIG_PUT   },
    { "GET",    "/api/alarms",    1, RT_ALARMS       },
    { "POST",   "/api/fw/master", 1, RT_FW_MASTER    },
    { "POST",   "/api/fw/zone",   1, RT_FW_ZONE      },
    { "POST",   "/api/fleet",     1, RT_FLEET_POST   },
    { "DELETE", "/api/fleet",     1, RT_FLEET_DELETE },
    { "GET",    "/api/wifi/scan", 1, RT_WIFI_SCAN    },
    { "POST",   "/api/wifi",      1, RT_WIFI_SET     },
    { "GET",    "/",              0, RT_INDEX        },
    { "GET",    "/app.js",        0, RT_APP_JS       },
    { "GET",    "/app.css",       0, RT_APP_CSS      },
    { "GET",    "/fw/zone.bin",   0, RT_FLEET_BIN    },
};

const int HTTP_ROUTES_N = (int)(sizeof HTTP_ROUTES / sizeof HTTP_ROUTES[0]);

int http_route_find(const char *method, const char *uri, int *auth_out) {
    if (!method || !uri) return -1;

    /* Everything up to the first '?' or '#' is the path; the query string
     * belongs to the handler, not to the match (GET /api/config?zone=2 is the
     * same route as GET /api/config), and a fragment belongs to nobody -- a
     * conforming client never puts one on the wire, so stripping it is
     * defence in depth against a client that does. Length-compare rather than
     * copy: the URI arrives in httpd's own buffer and nothing here needs a
     * mutable copy. */
    size_t plen = strcspn(uri, "?#");

    for (int i = 0; i < HTTP_ROUTES_N; i++) {
        const http_route_t *r = &HTTP_ROUTES[i];
        if (strcmp(method, r->method) != 0) continue;
        if (strlen(r->path) != plen || strncmp(uri, r->path, plen) != 0) continue;
        if (auth_out) *auth_out = (int)r->auth;
        return (int)r->id;
    }
    return -1;
}

/* 1 when reply opens with the whole token lit -- lit followed by the end of
 * the line or a space, not by more token characters -- so that a future
 * "ERR BUSYNESS" cannot be read as "ERR BUSY". */
static int token_is(const char *reply, const char *lit) {
    size_t n = strlen(lit);
    if (strncmp(reply, lit, n) != 0) return 0;
    char c = reply[n];
    return c == '\0' || c == '\n' || c == '\r' || c == ' ';
}

int http_reply_status(const char *reply) {
    if (!reply) return 422;
    if (strncmp(reply, "OK", 2) == 0) return 200;
    /* Two of the dispatcher's ERR tokens are about the server rather than the
     * request, and a web UI has to be able to tell them apart: BUSY means
     * cmd_task's slot pool was full (worth retrying), INTERNAL means the
     * dispatch never came back. Everything else is something the operator
     * asked for wrongly, which is what 422 says. */
    if (token_is(reply, "ERR BUSY"))     return 503;
    if (token_is(reply, "ERR INTERNAL")) return 500;
    return 422;
}
