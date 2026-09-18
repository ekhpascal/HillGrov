#pragma once
#include "web_auth.h"   /* WA_PW_MAX -- the only constraint on a password is its length */

/* Request-body buffer bounds for the two routes that carry a web password.
 *
 * http_srv_body() refuses a body longer than cap-1, so these caps decide which
 * passwords the operator can still LOG IN with -- undersize them and a legal
 * password that was accepted by POST /api/password becomes an unrecoverable
 * lockout, because the only way back in is a body that no longer fits.
 *
 * web_auth_set_password() bounds the LENGTH and nothing else (web_auth.h), so
 * the worst case is a WA_PW_MAX-byte password of the most expensive bytes JSON
 * has: a control byte (0x01..0x1F) escapes to \u00XX, six bytes each. (`"` and
 * `\` only cost two -- the earlier sizing assumed those were the worst case and
 * was still too small even for them.) A NUL cannot appear: set_password takes a
 * C string.
 *
 * Kept in a pure header, away from esp_http_server.h, so test_http_body_sizes
 * can check the arithmetic against a real JSON encoder on the host.
 */

#define HTTP_JSON_ESC_MAX  6                              /* \u00XX, the widest JSON escape */
#define HTTP_PW_ESC_MAX    (HTTP_JSON_ESC_MAX * WA_PW_MAX)

/* {"password":"<esc>"} + NUL  (sizeof the literal already counts the NUL) */
#define HTTP_LOGIN_BODY_MAX     (sizeof("{\"password\":\"\"}") + HTTP_PW_ESC_MAX)

/* {"old":"<esc>","new":"<esc>"} + NUL */
#define HTTP_PASSWORD_BODY_MAX  (sizeof("{\"old\":\"\",\"new\":\"\"}") + 2 * HTTP_PW_ESC_MAX)
