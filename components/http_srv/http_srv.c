#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "fw_srv.h"
#include "http_srv_internal.h"

static const char *TAG = "http_srv";

static httpd_handle_t    s_server;
static const cmd_core_t *s_core;

const cmd_core_t *http_srv_core(void) { return s_core; }

/* ---- responses ----
 * httpd_resp_set_status()/set_hdr() store the POINTER they are given, so
 * every status line and header value handed to them must outlive the send:
 * string literals here, stack buffers in the handlers (alive across the send
 * call by construction). */

static const char *status_line(int status) {
    switch (status) {
    case 200: return "200 OK";
    case 204: return "204 No Content";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 413: return "413 Content Too Large";
    case 422: return "422 Unprocessable Content";
    case 429: return "429 Too Many Requests";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default:  return "500 Internal Server Error";
    }
}

void http_srv_json(httpd_req_t *req, int status, const char *json) {
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json ? json : "", json ? HTTPD_RESP_USE_STRLEN : 0);
}

/* httpd_send() returns the count of ONE send() call, which can be short under
 * the socket's 5 s SO_SNDTIMEO -- the same reason fw_srv.c loops. */
static int send_all(httpd_req_t *req, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = httpd_send(req, buf + sent, len - sent);
        if (n <= 0) return -1;
        sent += (size_t)n;
    }
    return 0;
}

/* A 204 must carry neither a body nor a Content-Length, and there is no
 * representation for a Content-Type to describe -- but httpd_resp_send()
 * always emits both (its own default type when the handler set none). The
 * three lines are therefore composed by hand, the way fw_srv.c composes its
 * identity-framed image response. Safe with keep-alive: IDF's httpd neither
 * emits nor honours Connection headers, and a 204 is self-delimiting, so the
 * client knows the response ended without needing a length.
 *
 * cookie is a Set-Cookie VALUE (or NULL for no cookie); it is copied here, so
 * unlike httpd_resp_set_hdr the caller's buffer need not outlive the call. */
void http_srv_no_content(httpd_req_t *req, const char *cookie) {
    char head[192];
    int n;
    if (cookie && *cookie)
        n = snprintf(head, sizeof head, "HTTP/1.1 204 No Content\r\nSet-Cookie: %s\r\n\r\n", cookie);
    else
        n = snprintf(head, sizeof head, "HTTP/1.1 204 No Content\r\n\r\n");
    if (n < 0 || (size_t)n >= sizeof head) {
        ESP_LOGE(TAG, "204 header overflow (%d B) -- answering 500", n);
        http_srv_error(req, 500, "INTERNAL", NULL);
        return;
    }
    if (send_all(req, head, (size_t)n) != 0) ESP_LOGW(TAG, "204 send failed");
}

void http_srv_text(httpd_req_t *req, int status, const char *text) {
    httpd_resp_set_status(req, status_line(status));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

/* The path is echoed back to the client, so it is attacker-controlled text
 * going into a JSON string. Rather than escape it, copy only the characters a
 * legitimate route can contain and fold everything else to '.', which cannot
 * break out of the string no matter what arrives. */
static void sanitise_path(const char *in, char *out, size_t cap) {
    size_t o = 0;
    for (; in && *in && o + 1 < cap; in++) {
        char c = *in;
        int keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '/' || c == '.' || c == '-' || c == '_' || c == '?' || c == '=' || c == '&';
        out[o++] = keep ? c : '.';
    }
    out[o] = '\0';
}

void http_srv_error(httpd_req_t *req, int status, const char *code, const char *path) {
    char body[160];
    if (path) {
        char safe[80];
        sanitise_path(path, safe, sizeof safe);
        snprintf(body, sizeof body, "{\"error\":\"%s\",\"path\":\"%s\"}", code, safe);
    } else {
        snprintf(body, sizeof body, "{\"error\":\"%s\"}", code);
    }
    http_srv_json(req, status, body);
}

esp_err_t h_not_impl(httpd_req_t *req) {
    /* Registered now so the route table and its auth bits are complete and
     * auditable from day one; Task 12/13 replace these entries. */
    http_srv_json(req, 501, "{\"error\":\"NOT_IMPLEMENTED\"}");
    return http_srv_done(req, 0);   /* body never read */
}

/* ---- how a handler ends ----
 * ESP_OK tells httpd to keep the connection, and httpd_req_delete() then
 * PURGES whatever is left of the request body before it will look at the next
 * request (httpd_parse.c: a `while (ra->remaining_len)` recv loop). So a
 * client that announces `Content-Length: 100000000` and then trickles one byte
 * at a time pins the one httpd task for as long as it likes -- an
 * unauthenticated denial of service against every route, including the ones
 * that rejected the request precisely because the body was too long.
 *
 * Every path that answers WITHOUT having consumed the body therefore returns
 * ESP_FAIL instead: httpd logs "uri handler execution failed", skips the purge
 * entirely (httpd_sess_process returns before httpd_req_delete) and closes the
 * socket. Our response bytes are already on the wire by then, so the client
 * still sees its 413/401/404/501 -- it just does not get to keep the
 * connection it was abusing.
 *
 * drained = 1 only when the whole body was read. A chunked request always
 * counts as not drained: remaining_len is 0 for it, so httpd would purge
 * nothing and then try to parse the chunk framing left in the socket as the
 * next request. */
esp_err_t http_srv_done(httpd_req_t *req, int drained) {
    if (drained) return ESP_OK;
    if (req->content_len == 0 && httpd_req_get_hdr_value_len(req, "Transfer-Encoding") == 0)
        return ESP_OK;   /* there was no body to leave behind */
    return ESP_FAIL;
}

/* ---- request body ---- */

#define BODY_MAX_TIMEOUTS 3   /* ~15 s at recv_wait_timeout 5 -- these bodies are <= 192 B */

int http_srv_body(httpd_req_t *req, char *buf, size_t cap) {
    if (!buf || cap == 0) return HTTP_BODY_IO;
    buf[0] = '\0';

    /* KraftWerk scar: esp_http_server does NOT de-chunk request bodies. For a
     * chunked request req->content_len is 0, so a handler reading
     * content_len bytes sees an empty body while the chunk framing still sits
     * in the socket -- it would then be parsed as the start of the next
     * request on a keep-alive connection. Reject the framing instead of
     * guessing: only "chunked" and the deprecated "identity" are legal
     * values, so the header being present at all is enough. */
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding") > 0) return HTTP_BODY_CHUNKED;

    size_t total = req->content_len;
    if (total > cap - 1) return HTTP_BODY_TOO_LONG;

    size_t got = 0;
    int timeouts = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /* A client that stalls mid-body must not pin the single httpd
             * task forever (the SP1 rescue-upload scar, same shape). */
            if (++timeouts > BODY_MAX_TIMEOUTS) return HTTP_BODY_IO;
            continue;
        }
        if (n <= 0) return HTTP_BODY_IO;
        timeouts = 0;
        got += (size_t)n;
    }
    buf[got] = '\0';
    return (int)got;
}

/* ---- the one entry handler ----
 * Registered for every row of HTTP_ROUTES. The authentication decision comes
 * from http_route_find() on the live method + URI -- the same pure function
 * the host tests exercise -- and auth defaults to 1 so any future gap in the
 * table fails closed. */

typedef esp_err_t (*route_fn)(httpd_req_t *);

static const route_fn HANDLERS[RT_COUNT] = {
    [RT_LOGIN]        = h_login,
    [RT_LOGOUT]       = h_logout,
    [RT_PASSWORD]     = h_password,
    [RT_CMD]          = h_cmd,
    [RT_HELP]         = h_help,
    [RT_STATE]        = h_not_impl,   /* Task 12 */
    [RT_SCHEMA]       = h_not_impl,   /* Task 12 */
    [RT_CONFIG_GET]   = h_not_impl,   /* Task 12 */
    [RT_CONFIG_PUT]   = h_not_impl,   /* Task 12 */
    [RT_ALARMS]       = h_not_impl,   /* Task 12 */
    [RT_FW_MASTER]    = h_not_impl,   /* Task 13 */
    [RT_FW_ZONE]      = h_not_impl,   /* Task 13 */
    [RT_FLEET_POST]   = h_not_impl,   /* Task 13 */
    [RT_FLEET_DELETE] = h_not_impl,   /* Task 13 */
    [RT_WIFI_SCAN]    = h_not_impl,   /* Task 12 */
    [RT_WIFI_SET]     = h_not_impl,   /* Task 12 */
    [RT_INDEX]        = h_index,
    [RT_APP_JS]       = h_app_js,
    [RT_APP_CSS]      = h_app_css,
    [RT_FLEET_BIN]    = NULL,         /* fw_srv owns this handler; never registered here */
};

static const char *method_name(int m) {
    switch (m) {
    case HTTP_GET:    return "GET";
    case HTTP_POST:   return "POST";
    case HTTP_PUT:    return "PUT";
    case HTTP_DELETE: return "DELETE";
    default:          return NULL;   /* http_route_find(NULL, ...) -> no match -> 404 */
    }
}

static esp_err_t route_entry(httpd_req_t *req) {
    const http_route_t *row = (const http_route_t *)req->user_ctx;
    const char *m = method_name(req->method);
    int auth = 1;   /* fail closed: never run a handler with auth unset */
    int id = http_route_find(m, req->uri, &auth);

    /* httpd matched one of our registered rows to get here, so the pure
     * matcher must agree with the row it handed us. A disagreement means the
     * table and the registration loop have drifted apart -- refuse rather
     * than run a handler under some other row's auth bit. */
    if (id < 0 || id >= RT_COUNT || !row || id != (int)row->id || !HANDLERS[id]) {
        ESP_LOGW(TAG, "unroutable %s %s (row %d, id %d)", m ? m : "?", req->uri,
                 row ? (int)row->id : -1, id);
        http_srv_error(req, 404, "NOT_FOUND", req->uri);
        return http_srv_done(req, 0);
    }

    if (auth && !http_srv_auth_ok(req)) return http_srv_done(req, 0);   /* 401 already sent */

    return HANDLERS[id](req);
}

/* An unknown path or a wrong method never reaches route_entry -- httpd
 * answers those itself, by default with a plain-text page. These keep every
 * response from the instance in the API's one error shape. */
static esp_err_t err_json(httpd_req_t *req, httpd_err_code_t err) {
    int method_err = (err == HTTPD_405_METHOD_NOT_ALLOWED);
    http_srv_error(req, method_err ? 405 : 404, method_err ? "METHOD_NOT_ALLOWED" : "NOT_FOUND", req->uri);
    /* A wrong URL is not a protocol error, so the connection is kept -- unless
     * the request carried a body nobody read (see http_srv_done). */
    return http_srv_done(req, 0);
}

static httpd_method_t method_id(const char *name) {
    if (!strcmp(name, "GET"))  return HTTP_GET;
    if (!strcmp(name, "POST")) return HTTP_POST;
    if (!strcmp(name, "PUT"))  return HTTP_PUT;
    return HTTP_DELETE;   /* the table holds no other method (test_http_routes pins it) */
}

int http_srv_start(const cmd_core_t *core) {
    if (s_server) return 0;   /* idempotent */
    s_core = core;

    if (http_auth_init() != 0)
        ESP_LOGE(TAG, "web auth unavailable -- every login will fail");
    if (http_cmd_init() != 0) return -1;   /* no /api/cmd without its session claim */

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    /* Hardening, every line of it paid for on an earlier project:
     *   stack 8192        the /api/cmd path runs cmd_dispatch + a ring forward
     *   max_open_sockets  4 with lru_purge_enable, so a browser holding
     *                     keep-alive sockets can never lock the operator out
     *   core_id 0         same core as cmd_task, away from the Wi-Fi driver
     *   max_uri_handlers  19 rows registered here + /fw/zone.bin, room to grow
     *   recv/send 5 s     no single request may pin the one httpd task longer */
    cfg.stack_size        = 8192;
    cfg.max_open_sockets  = 4;
    cfg.lru_purge_enable  = true;
    cfg.core_id           = 0;
    cfg.max_uri_handlers  = 24;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 5;

    esp_err_t rc = httpd_start(&s_server, &cfg);
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(rc));
        s_server = NULL;
        return -1;
    }

    for (int i = 0; i < HTTP_ROUTES_N; i++) {
        /* RT_FLEET_BIN's row exists so the auth table covers every path the
         * instance answers (auth 0 by SP3 design: a zone in rescue has no
         * cookie), but its handler is fw_srv's, registered just below. */
        if (HTTP_ROUTES[i].id == RT_FLEET_BIN) continue;
        httpd_uri_t u = {
            .uri      = HTTP_ROUTES[i].path,
            .method   = method_id(HTTP_ROUTES[i].method),
            .handler  = route_entry,
            .user_ctx = (void *)&HTTP_ROUTES[i],
        };
        rc = httpd_register_uri_handler(s_server, &u);
        if (rc != ESP_OK) {
            ESP_LOGE(TAG, "register %s %s failed: %s", HTTP_ROUTES[i].method, HTTP_ROUTES[i].path,
                     esp_err_to_name(rc));
            httpd_stop(s_server);
            s_server = NULL;
            return -1;
        }
    }

    if (fw_srv_register(s_server) != 0) {
        ESP_LOGE(TAG, "fw_srv_register failed");
        httpd_stop(s_server);
        s_server = NULL;
        return -1;
    }

    httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, err_json);
    httpd_register_err_handler(s_server, HTTPD_405_METHOD_NOT_ALLOWED, err_json);

    ESP_LOGI(TAG, "http server up on :80 (%d routes + /fw/zone.bin)", HTTP_ROUTES_N - 1);
    return 0;
}
