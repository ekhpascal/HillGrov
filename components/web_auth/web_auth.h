#pragma once
#include <stdint.h>
#include <stddef.h>
#include "hg_mcfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_SESSIONS   4
#define WA_TOKEN_LEN  16
#define WA_COOKIE     "hg_sess"
#define WA_TTL_S      (30u * 24 * 3600)
#define WA_LOCK_FAILS 5
#define WA_LOCK_S     60

typedef void (*wa_sha256_fn)(const uint8_t *in, size_t n, uint8_t out[32]);
typedef void (*wa_rand_fn)(uint8_t *out, size_t n);

typedef struct { uint8_t token[WA_TOKEN_LEN]; uint32_t expires_s; uint8_t used; } wa_session_t;
typedef struct { wa_session_t s[WA_SESSIONS]; uint8_t fails; uint32_t lock_until_s; wa_sha256_fn sha; wa_rand_fn rnd; } wa_state_t;

void web_auth_init(wa_state_t *st, wa_sha256_fn sha, wa_rand_fn rnd);

int  web_auth_set_password(wa_state_t *st, hg_mcfg_t *m, const char *pw);
/* 8..63 chars else -1; new salt; hash = sha256(salt||pw); clears MCFG_F_WEB_DEFAULT */

int  web_auth_login(wa_state_t *st, const hg_mcfg_t *m, const char *pw, uint32_t now_s, char cookie_val[2 * WA_TOKEN_LEN + 1]);
/* 0 ok (+cookie hex); -1 wrong password (fails++); -2 locked (lock_until_s > now) -- evicts the oldest session when full.
 * "Oldest" = the slot with the smallest expires_s (0 for a never-used or logged-out slot, so an
 * empty slot is always picked over evicting a live one); on an exact tie the lowest-index slot wins. */

int  web_auth_verify(const wa_state_t *st, const hg_mcfg_t *m, const char *pw);
/* 0 correct / -1 wrong; creates no session; does not touch the fail counter or lockout
 * (Task 11's "change password" flow uses this to check the old password) */

int  web_auth_check(wa_state_t *st, const char *cookie_header, uint32_t now_s);
/* 0 valid; -1 missing/unknown/expired. Parses "a=b; hg_sess=<hex>; c=d" */

void web_auth_logout(wa_state_t *st, const char *cookie_header);

int  web_auth_pack(const wa_state_t *st, uint8_t *out, size_t cap);
/* sessions only: 4x(16+4) = 80 B, explicit offsets; returns len, -1 if cap too small */

int  web_auth_unpack(wa_state_t *st, const uint8_t *in, size_t n);
/* 0 / -1 (short input; *st left untouched) */

#ifdef __cplusplus
}
#endif
