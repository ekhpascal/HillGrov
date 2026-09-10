#include <string.h>
#include "web_auth.h"

#define WA_PWBUF 64   /* clamp: set_password enforces 8..63, this just bounds the stack buffer */

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/* Constant-access-pattern compare of two NUL-terminated strings, clamped to
 * WA_PWBUF so the loop always walks the same number of bytes regardless of
 * where (or whether) the strings differ. */
static int ct_eq_str(const char *a, const char *b) {
    uint8_t ba[WA_PWBUF] = {0}, bb[WA_PWBUF] = {0};
    size_t la = strlen(a); if (la > WA_PWBUF) la = WA_PWBUF;
    size_t lb = strlen(b); if (lb > WA_PWBUF) lb = WA_PWBUF;
    memcpy(ba, a, la);
    memcpy(bb, b, lb);
    uint8_t d = (uint8_t)(la ^ lb);
    for (size_t i = 0; i < WA_PWBUF; i++) d |= (uint8_t)(ba[i] ^ bb[i]);
    return d == 0;
}

static int pw_matches(const wa_state_t *st, const hg_mcfg_t *m, const char *pw) {
    if (m->flags & MCFG_F_WEB_DEFAULT) return ct_eq_str(pw, "hillgrow1");

    uint8_t buf[16 + WA_PWBUF];
    size_t plen = strlen(pw); if (plen > WA_PWBUF) plen = WA_PWBUF;
    memcpy(buf, m->web_salt, sizeof m->web_salt);
    memcpy(buf + sizeof m->web_salt, pw, plen);
    uint8_t hash[32];
    st->sha(buf, sizeof m->web_salt + plen, hash);
    return ct_eq(hash, m->web_hash, sizeof m->web_hash);
}

static void hex_encode(const uint8_t *in, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

static int hexval(char c) {   /* lowercase-only: uppercase A-F is rejected */
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* Finds "hg_sess=<32 lower-hex chars>" anywhere in a ';'-separated cookie
 * header ("a=b; hg_sess=...; c=d"), tolerating a space after ';'. Never reads
 * past the NUL terminator. 0 + decoded token on success, -1 otherwise. */
static int extract_cookie_token(const char *hdr, uint8_t out[WA_TOKEN_LEN]) {
    if (!hdr) return -1;
    const size_t nlen = sizeof(WA_COOKIE) - 1;   /* 7, excludes the NUL */
    const char *p = hdr;
    while (*p) {
        while (*p == ' ' || *p == ';') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != ';') p++;   /* p now at ';' or the terminating NUL */
        size_t seglen = (size_t)(p - seg);
        if (seglen > nlen && strncmp(seg, WA_COOKIE, nlen) == 0 && seg[nlen] == '=') {
            const char *val = seg + nlen + 1;
            size_t vlen = (size_t)(p - val);
            if (vlen != 2 * WA_TOKEN_LEN) return -1;
            for (size_t i = 0; i < vlen; i++)
                if (hexval(val[i]) < 0) return -1;
            for (size_t i = 0; i < WA_TOKEN_LEN; i++)
                out[i] = (uint8_t)((hexval(val[2 * i]) << 4) | hexval(val[2 * i + 1]));
            return 0;
        }
        if (*p == ';') p++;
    }
    return -1;
}

void web_auth_init(wa_state_t *st, wa_sha256_fn sha, wa_rand_fn rnd) {
    memset(st, 0, sizeof *st);
    st->sha = sha;
    st->rnd = rnd;
}

int web_auth_set_password(wa_state_t *st, hg_mcfg_t *m, const char *pw) {
    size_t n = strlen(pw);
    if (n < 8 || n > 63) return -1;

    uint8_t salt[sizeof m->web_salt];
    st->rnd(salt, sizeof salt);
    uint8_t buf[sizeof m->web_salt + WA_PWBUF];
    memcpy(buf, salt, sizeof salt);
    memcpy(buf + sizeof salt, pw, n);
    uint8_t hash[sizeof m->web_hash];
    st->sha(buf, sizeof salt + n, hash);

    memcpy(m->web_salt, salt, sizeof salt);
    memcpy(m->web_hash, hash, sizeof hash);
    m->flags = (uint8_t)(m->flags & (uint8_t)~MCFG_F_WEB_DEFAULT);
    return 0;
}

int web_auth_login(wa_state_t *st, const hg_mcfg_t *m, const char *pw, uint32_t now_s, char cookie_val[2 * WA_TOKEN_LEN + 1]) {
    if (now_s < st->lock_until_s) return -2;

    if (!pw_matches(st, m, pw)) {
        st->fails++;
        if (st->fails >= WA_LOCK_FAILS) st->lock_until_s = now_s + WA_LOCK_S;
        return -1;
    }
    st->fails = 0;

    /* Free slots have expires_s == 0 (the invariant web_auth_logout/_init/_unpack
     * maintain), so "oldest = min expires_s" also naturally prefers a free slot
     * over evicting a live one. */
    int idx = 0;
    uint32_t best = st->s[0].expires_s;
    for (int i = 1; i < WA_SESSIONS; i++)
        if (st->s[i].expires_s < best) { best = st->s[i].expires_s; idx = i; }

    st->rnd(st->s[idx].token, WA_TOKEN_LEN);
    st->s[idx].expires_s = now_s + WA_TTL_S;
    st->s[idx].used = 1;
    hex_encode(st->s[idx].token, WA_TOKEN_LEN, cookie_val);
    return 0;
}

int web_auth_verify(const wa_state_t *st, const hg_mcfg_t *m, const char *pw) {
    return pw_matches(st, m, pw) ? 0 : -1;
}

int web_auth_check(wa_state_t *st, const char *cookie_header, uint32_t now_s) {
    uint8_t token[WA_TOKEN_LEN];
    if (extract_cookie_token(cookie_header, token) != 0) return -1;
    for (int i = 0; i < WA_SESSIONS; i++) {
        if (!st->s[i].used) continue;
        if (memcmp(st->s[i].token, token, WA_TOKEN_LEN) == 0)
            return (now_s < st->s[i].expires_s) ? 0 : -1;
    }
    return -1;
}

void web_auth_logout(wa_state_t *st, const char *cookie_header) {
    uint8_t token[WA_TOKEN_LEN];
    if (extract_cookie_token(cookie_header, token) != 0) return;
    for (int i = 0; i < WA_SESSIONS; i++) {
        if (st->s[i].used && memcmp(st->s[i].token, token, WA_TOKEN_LEN) == 0) {
            memset(&st->s[i], 0, sizeof st->s[i]);   /* used=0, expires_s=0: keeps the free-slot invariant */
            return;
        }
    }
}

#define WA_SESS_REC_LEN (WA_TOKEN_LEN + 4)
#define WA_PACK_LEN     (WA_SESSIONS * WA_SESS_REC_LEN)

int web_auth_pack(const wa_state_t *st, uint8_t *out, size_t cap) {
    if (cap < WA_PACK_LEN) return -1;
    for (int i = 0; i < WA_SESSIONS; i++) {
        uint8_t *p = out + i * WA_SESS_REC_LEN;
        memcpy(p, st->s[i].token, WA_TOKEN_LEN);
        wr32(p + WA_TOKEN_LEN, st->s[i].expires_s);
    }
    return WA_PACK_LEN;
}

int web_auth_unpack(wa_state_t *st, const uint8_t *in, size_t n) {
    if (n < WA_PACK_LEN) return -1;
    for (int i = 0; i < WA_SESSIONS; i++) {
        const uint8_t *p = in + i * WA_SESS_REC_LEN;
        memcpy(st->s[i].token, p, WA_TOKEN_LEN);
        st->s[i].expires_s = rd32(p + WA_TOKEN_LEN);
        st->s[i].used = (st->s[i].expires_s != 0) ? 1 : 0;
    }
    return 0;
}
