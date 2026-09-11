#include <string.h>
#include "http_routes.h"

/* One row per route_id_t, in id order (test_http_routes pins both). auth = 1
 * means route_entry() calls http_srv_auth_ok() -- i.e. a valid hg_sess cookie
 * -- before the handler runs.
 *
 * The three auth = 0 rows are deliberate:
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

    /* Everything up to the first '?' is the path; the query string belongs to
     * the handler, not the match (GET /api/config?zone=2 is the same route as
     * GET /api/config). Length-compare rather than copy: the URI arrives in
     * httpd's own buffer and nothing here needs a mutable copy. */
    const char *q = strchr(uri, '?');
    size_t plen = q ? (size_t)(q - uri) : strlen(uri);

    for (int i = 0; i < HTTP_ROUTES_N; i++) {
        const http_route_t *r = &HTTP_ROUTES[i];
        if (strcmp(method, r->method) != 0) continue;
        if (strlen(r->path) != plen || strncmp(uri, r->path, plen) != 0) continue;
        if (auth_out) *auth_out = (int)r->auth;
        return (int)r->id;
    }
    return -1;
}
