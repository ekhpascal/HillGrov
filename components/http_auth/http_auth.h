#pragma once
#include <stdint.h>
#include "web_auth.h"   /* WA_TOKEN_LEN */
#include "hg_mcfg.h"    /* hg_mcfg_t */

#ifdef __cplusplus
extern "C" {
#endif

/* The master's web-session store: the ONE wa_state_t in the firmware, holding
 * the live login sessions plus the sha/rand hooks web_auth needs. The httpd
 * task (login/logout/cookie checks, POST /api/password), the CLI task (SET WEB
 * PASSWORD) and the panel (System > Web password, spec Decision 3) all reach
 * it, so every entry point takes this component's mutex internally (3000 ms)
 * and nothing outside http_auth.c ever sees the struct. Sharing it is the
 * point: a password changed from any face drops every web session.
 *
 * Moved out of components/http_srv unchanged in logic (panel plan Task 2), so
 * components/panel_svc can sit beneath http_srv without a dependency cycle. */

/* One-time init: PSA crypto + web_auth_init() over the session array, then the
 * persisted sessions from NVS ("hg"/"sess"). Idempotent. Call it at boot once
 * nvs_flash_init() has run -- app_main does, and so does http_srv_start(),
 * because a boot whose radio never came up still has to be able to hash a
 * console SET WEB PASSWORD. Until it has succeeded every entry point answers
 * as if the board had no crypto (a login fails, a password change reports
 * ERR INTERNAL) rather than touching uninitialised state. 0 ok, -1 if crypto
 * or the mutex is unavailable. */
int  http_auth_init(void);

int  http_auth_check(const char *cookie_hdr);                                   /* 0 valid / -1 not */
int  http_auth_login(const char *pw, char cookie_out[2 * WA_TOKEN_LEN + 1]);    /* 0 / -1 bad / -2 locked */
int  http_auth_verify_password(const char *pw);   /* 0 correct / -1 wrong; no session, no fail count */
void http_auth_logout_cookie(const char *cookie_hdr);

/* Puts a fresh salt + sha256(salt||pw) into *m and clears MCFG_F_WEB_DEFAULT,
 * without touching NVS -- the caller still owns the mcfg_commit(). Does NOT
 * drop sessions (see http_auth_sessions_drop, to be called only after the
 * commit succeeded). 0 ok, -1 pw outside 8..63 (*m untouched), -3 SHA-256
 * unavailable -- the board's crypto is broken, and in that case *m HAS been
 * modified and now holds a fresh salt with an all-zero digest that nothing
 * could ever match, so the caller must discard its copy and commit nothing. */
int  http_auth_hash_password(hg_mcfg_t *m, const char *pw);

/* Invalidates every live web session and rewrites NVS. Call right after a
 * successful password commit: the old cookies must not outlive the password
 * they were issued against. */
void http_auth_sessions_drop(void);

#ifdef __cplusplus
}
#endif
