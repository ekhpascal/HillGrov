#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"
#include "http_srv_internal.h"

static const char *TAG = "http_login";

/* The HTTP surface over http_auth.c's shared wa_state_t: the cookie gate every
 * auth=1 route passes through, and the three routes that manage the session
 * itself. All state lives behind http_auth.c's locked facade -- nothing here
 * touches a wa_state_t, which is why the lock discipline is impossible to get
 * wrong from this side. */

/* ---- the gate ---- */

/* A browser may send other cookies alongside ours; web_auth_check parses the
 * whole ';'-separated header. A value longer than this is truncated by
 * httpd (still NUL-terminated), which can only make the token unparseable --
 * i.e. it fails closed. */
#define COOKIE_HDR_MAX 320

static int cookie_header(httpd_req_t *req, char *buf, size_t cap) {
    esp_err_t rc = httpd_req_get_hdr_value_str(req, "Cookie", buf, cap);
    if (rc == ESP_OK || rc == ESP_ERR_HTTPD_RESULT_TRUNC) return 0;
    buf[0] = '\0';
    return -1;
}

int http_srv_auth_ok(httpd_req_t *req) {
    char hdr[COOKIE_HDR_MAX];
    if (cookie_header(req, hdr, sizeof hdr) == 0 && http_auth_check(hdr) == 0) return 1;
    http_srv_error(req, 401, "UNAUTHORIZED", NULL);
    return 0;
}

/* ---- handlers ---- */

/* hg_sess=<32 hex>; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax
 * No Secure attribute: the greenhouse AP serves plain HTTP and a Secure
 * cookie would simply never be sent back. HttpOnly keeps it out of reach of
 * any script on the page; SameSite=Lax stops a foreign page from driving
 * /api/cmd with the operator's cookie. */
static void set_session_cookie(httpd_req_t *req, char *out, size_t cap, const char *token) {
    if (token) snprintf(out, cap, WA_COOKIE "=%s; Path=/; Max-Age=2592000; HttpOnly; SameSite=Lax", token);
    else       snprintf(out, cap, WA_COOKIE "=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax");
    httpd_resp_set_hdr(req, "Set-Cookie", out);   /* out must outlive the send: caller's stack */
}

/* Reads a JSON body of at most cap-1 bytes and answers the error itself.
 * Returns the parsed root (caller deletes) or NULL. */
static cJSON *read_json_body(httpd_req_t *req, char *buf, size_t cap) {
    int n = http_srv_body(req, buf, cap);
    if (n == HTTP_BODY_TOO_LONG) { http_srv_error(req, 413, "TOO_LONG", NULL);   return NULL; }
    if (n == HTTP_BODY_CHUNKED)  { http_srv_error(req, 400, "CHUNKED", NULL);    return NULL; }
    if (n < 0)                   { http_srv_error(req, 400, "BAD_REQUEST", NULL); return NULL; }
    cJSON *root = cJSON_ParseWithLength(buf, (size_t)n);
    if (!root) { http_srv_error(req, 400, "BAD_JSON", NULL); return NULL; }
    return root;
}

static const char *json_str(const cJSON *root, const char *key) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, key);
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

esp_err_t h_login(httpd_req_t *req) {
    char body[129];   /* spec: login bodies are <= 128 B */
    cJSON *root = read_json_body(req, body, sizeof body);
    if (!root) return ESP_OK;

    const char *pw = json_str(root, "password");
    if (!pw) {
        cJSON_Delete(root);
        http_srv_error(req, 400, "BAD_REQUEST", NULL);
        return ESP_OK;
    }

    char token[2 * WA_TOKEN_LEN + 1];
    int rc = http_auth_login(pw, token);
    cJSON_Delete(root);

    if (rc == -2) {
        ESP_LOGW(TAG, "login refused: locked out");
        http_srv_error(req, 429, "LOCKED", NULL);
        return ESP_OK;
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "login refused: bad password");
        http_srv_error(req, 401, "BAD_PASSWORD", NULL);
        return ESP_OK;
    }

    char cookie[96];
    set_session_cookie(req, cookie, sizeof cookie, token);
    ESP_LOGI(TAG, "login ok");
    http_srv_json(req, 204, NULL);
    return ESP_OK;
}

esp_err_t h_logout(httpd_req_t *req) {
    char hdr[COOKIE_HDR_MAX];
    if (cookie_header(req, hdr, sizeof hdr) == 0) http_auth_logout_cookie(hdr);
    char cookie[96];
    set_session_cookie(req, cookie, sizeof cookie, NULL);
    http_srv_json(req, 204, NULL);
    return ESP_OK;
}

/* Implemented in master/main/net_ops_master.c, which owns the mcfg
 * snapshot -> commit sequence and its mutex. Declared here rather than
 * through net_ops_master.h because that header lives in the master app and a
 * component cannot include it; this one symbol is the whole contract.
 * 0 ok, -1 length/invalid, -2 could not be stored, -3 crypto unavailable. */
extern int master_web_set_password(const char *pw);

esp_err_t h_password(httpd_req_t *req) {
    char body[256];   /* two <= 63-char passwords plus JSON punctuation */
    cJSON *root = read_json_body(req, body, sizeof body);
    if (!root) return ESP_OK;

    const char *old_pw = json_str(root, "old");
    const char *new_pw = json_str(root, "new");
    if (!old_pw || !new_pw) {
        cJSON_Delete(root);
        http_srv_error(req, 400, "BAD_REQUEST", NULL);
        return ESP_OK;
    }

    /* web_auth_verify creates no session and does not touch the lockout
     * counter: the caller already proved possession of a valid cookie, so the
     * old-password check is a confirmation, not an authentication attempt. */
    if (http_auth_verify_password(old_pw) != 0) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "password change refused: old password wrong");
        http_srv_error(req, 403, "BAD_PASSWORD", NULL);
        return ESP_OK;
    }

    int rc = master_web_set_password(new_pw);
    if (rc != 0) {
        cJSON_Delete(root);
        http_srv_error(req, rc == -1 ? 400 : 500,
                       rc == -1 ? "INVALID" : (rc == -2 ? "STORAGE" : "INTERNAL"), NULL);
        return ESP_OK;
    }

    /* master_web_set_password dropped every session, including this caller's
     * -- the old cookies must not outlive the password they were issued
     * against. Mint a fresh one for the operator who just changed it, so the
     * UI does not bounce them to the login page. */
    char token[2 * WA_TOKEN_LEN + 1];
    int lrc = http_auth_login(new_pw, token);
    cJSON_Delete(root);

    char cookie[96];
    set_session_cookie(req, cookie, sizeof cookie, lrc == 0 ? token : NULL);
    ESP_LOGI(TAG, "web password changed%s", lrc == 0 ? "" : " (re-login required)");
    http_srv_json(req, 204, NULL);
    return ESP_OK;
}
