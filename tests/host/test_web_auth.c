#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "web_auth.h"
#include "hg_mcfg.h"
#include "fake_sha256.h"

/* Counter-fed fake RNG: deterministic and distinct across calls within a test
 * (each call consumes the next N bytes of an incrementing byte stream), reset
 * to a known start in setUp() so tests don't depend on run order. */
static uint32_t s_rand_ctr;
static void fake_rand(uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(s_rand_ctr++);
}

void setUp(void) { s_rand_ctr = 1; }
void tearDown(void) {}

static void hexcookie(char *hdr, size_t hdrcap, const char *tok) {
    snprintf(hdr, hdrcap, "hg_sess=%s", tok);
}

static void test_sha256_matches_standard_vectors(void) {
    uint8_t out[32];
    static const uint8_t exp_empty[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
    };
    static const uint8_t exp_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    fake_sha256((const uint8_t *)"", 0, out);
    TEST_ASSERT_EQUAL_MEMORY(exp_empty, out, 32);
    fake_sha256((const uint8_t *)"abc", 3, out);
    TEST_ASSERT_EQUAL_MEMORY(exp_abc, out, 32);
}

static void test_default_password_logs_in_and_sets_cookie(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];

    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", 1000, cookie));
    TEST_ASSERT_EQUAL_size_t(32, strlen(cookie));
    for (size_t i = 0; i < strlen(cookie); i++)
        TEST_ASSERT_TRUE((cookie[i] >= '0' && cookie[i] <= '9') || (cookie[i] >= 'a' && cookie[i] <= 'f'));

    char hdr[64]; hexcookie(hdr, sizeof hdr, cookie);
    TEST_ASSERT_EQUAL_INT(0, web_auth_check(&st, hdr, 1000));
}

static void test_wrong_password_counts_and_locks(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];
    uint32_t now = 5000;

    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_INT(-1, web_auth_login(&st, &m, "wrong", now, cookie));
    /* 6th attempt is locked even with the *correct* password -- the lock check
     * happens before any password comparison. */
    TEST_ASSERT_EQUAL_INT(-2, web_auth_login(&st, &m, "hillgrow1", now, cookie));
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", now + WA_LOCK_S + 1, cookie));
    TEST_ASSERT_EQUAL_UINT8(0, st.fails);
}

static void test_set_password_replaces_default(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];

    TEST_ASSERT_EQUAL_INT(0, web_auth_set_password(&st, &m, "s3cretpass"));
    TEST_ASSERT_EQUAL_UINT8(0, m.flags & MCFG_F_WEB_DEFAULT);
    TEST_ASSERT_EQUAL_INT(-1, web_auth_login(&st, &m, "hillgrow1", 1, cookie));
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "s3cretpass", 1, cookie));

    hg_mcfg_t before = m;
    TEST_ASSERT_EQUAL_INT(-1, web_auth_set_password(&st, &m, "short"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &m, sizeof m);
}

static void test_expiry_and_eviction(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookies[5][2 * WA_TOKEN_LEN + 1];
    uint32_t base = 10000;

    for (uint32_t i = 0; i < WA_SESSIONS; i++)
        TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", base + i, cookies[i]));

    /* table full -- the 5th login evicts the oldest (slot 0, smallest expires_s) */
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", base + 4, cookies[4]));

    char hdr[64];
    hexcookie(hdr, sizeof hdr, cookies[0]);
    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, hdr, base + 4));   /* evicted */
    hexcookie(hdr, sizeof hdr, cookies[3]);
    TEST_ASSERT_EQUAL_INT(0, web_auth_check(&st, hdr, base + 4));    /* still valid */

    /* expiry: cookies[3] was minted at base+3, so it expires at base+3+WA_TTL_S */
    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, hdr, base + 3 + WA_TTL_S + 1));
}

static void test_cookie_parse(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", 1, cookie));

    char hdr[96];
    snprintf(hdr, sizeof hdr, "foo=1; hg_sess=%s", cookie);
    TEST_ASSERT_EQUAL_INT(0, web_auth_check(&st, hdr, 1));

    snprintf(hdr, sizeof hdr, "hg_sess=%s;x=y", cookie);
    TEST_ASSERT_EQUAL_INT(0, web_auth_check(&st, hdr, 1));

    char upper[2 * WA_TOKEN_LEN + 1];
    strcpy(upper, cookie);
    for (char *p = upper; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(hdr, sizeof hdr, "hg_sess=%s", upper);
    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, hdr, 1));          /* uppercase hex rejected */

    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, "foo=bar", 1));    /* missing */
    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, "hg_sess=", 1));   /* truncated */
}

static void test_pack_unpack(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", 100, cookie));
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", 200, cookie));

    uint8_t buf[80];
    int n = web_auth_pack(&st, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(80, n);

    wa_state_t st2; web_auth_init(&st2, fake_sha256, fake_rand);
    TEST_ASSERT_EQUAL_INT(0, web_auth_unpack(&st2, buf, (size_t)n));
    for (int i = 0; i < WA_SESSIONS; i++) {
        TEST_ASSERT_EQUAL_MEMORY(st.s[i].token, st2.s[i].token, WA_TOKEN_LEN);
        TEST_ASSERT_EQUAL_UINT32(st.s[i].expires_s, st2.s[i].expires_s);
        TEST_ASSERT_EQUAL_UINT8(st.s[i].used, st2.s[i].used);
    }

    TEST_ASSERT_EQUAL_INT(-1, web_auth_unpack(&st2, buf, 79));   /* short input */
}

static void test_logout_invalidates(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);
    char cookie[2 * WA_TOKEN_LEN + 1];
    TEST_ASSERT_EQUAL_INT(0, web_auth_login(&st, &m, "hillgrow1", 1, cookie));

    char hdr[64]; hexcookie(hdr, sizeof hdr, cookie);
    TEST_ASSERT_EQUAL_INT(0, web_auth_check(&st, hdr, 1));
    web_auth_logout(&st, hdr);
    TEST_ASSERT_EQUAL_INT(-1, web_auth_check(&st, hdr, 1));
}

/* Controller pre-flight ruling: web_auth_verify() checks a password without
 * creating a session or touching the fail counter / lockout (Task 11's
 * "change password" flow uses it to check the old password). */
static void test_verify_checks_password_without_side_effects(void) {
    wa_state_t st; web_auth_init(&st, fake_sha256, fake_rand);
    hg_mcfg_t m; hg_mcfg_defaults(&m);

    TEST_ASSERT_EQUAL_INT(0, web_auth_verify(&st, &m, "hillgrow1"));
    TEST_ASSERT_EQUAL_INT(-1, web_auth_verify(&st, &m, "wrong"));
    TEST_ASSERT_EQUAL_UINT8(0, st.fails);
    for (int i = 0; i < WA_SESSIONS; i++) TEST_ASSERT_EQUAL_UINT8(0, st.s[i].used);

    TEST_ASSERT_EQUAL_INT(0, web_auth_set_password(&st, &m, "s3cretpass"));
    TEST_ASSERT_EQUAL_INT(0, web_auth_verify(&st, &m, "s3cretpass"));
    TEST_ASSERT_EQUAL_INT(-1, web_auth_verify(&st, &m, "s3cretpassX"));
    TEST_ASSERT_EQUAL_UINT8(0, st.fails);
    for (int i = 0; i < WA_SESSIONS; i++) TEST_ASSERT_EQUAL_UINT8(0, st.s[i].used);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_matches_standard_vectors);
    RUN_TEST(test_default_password_logs_in_and_sets_cookie);
    RUN_TEST(test_wrong_password_counts_and_locks);
    RUN_TEST(test_set_password_replaces_default);
    RUN_TEST(test_expiry_and_eviction);
    RUN_TEST(test_cookie_parse);
    RUN_TEST(test_pack_unpack);
    RUN_TEST(test_logout_invalidates);
    RUN_TEST(test_verify_checks_password_without_side_effects);
    return UNITY_END();
}
