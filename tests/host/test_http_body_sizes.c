#include <string.h>
#include "unity.h"
#include "cJSON.h"
#include "http_body_sizes.h"

/* SP4 final fix wave, F3. http_login.c sizes its request-body buffers for the
   documents that carry a web password, and http_srv_body() refuses a body
   longer than cap-1. web_auth_set_password() constrains the LENGTH only, so a
   password may be WA_PW_MAX bytes of anything a C string can hold -- and JSON
   escaping makes those bytes cost up to six each. Undersized buffers turn a
   legal password into an unrecoverable lockout: the change is accepted, and the
   login that would use it is then refused 413 forever.

   The escape arithmetic in http_body_sizes.h is checked here against a real
   JSON encoder rather than trusted, because it is the arithmetic that was
   wrong: the buffers were 129 B / 256 B, sized as if a password were plain
   text. */

void setUp(void) {}
void tearDown(void) {}

/* The most expensive password web_auth_set_password() will accept. */
static void worst_pw(char *out, char byte) {
    memset(out, byte, WA_PW_MAX);
    out[WA_PW_MAX] = '\0';
}

static size_t body_len(cJSON *doc) {
    char *s = cJSON_PrintUnformatted(doc);
    TEST_ASSERT_NOT_NULL(s);
    size_t n = strlen(s);
    cJSON_free(s);
    cJSON_Delete(doc);
    return n;
}

static size_t login_body_len(const char *pw) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "password", pw);
    return body_len(o);
}

static size_t password_body_len(const char *old_pw, const char *new_pw) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "old", old_pw);
    cJSON_AddStringToObject(o, "new", new_pw);
    return body_len(o);
}

/* A control byte escapes to \u00XX (6 B) -- the true worst case, and the one
   the bound must equal exactly. */
static void test_login_body_fits_worst_case_password(void) {
    char pw[WA_PW_MAX + 1];
    worst_pw(pw, '\x01');
    TEST_ASSERT_EQUAL_size_t(HTTP_LOGIN_BODY_MAX, login_body_len(pw) + 1);
}

/* '"' and '\' double (2 B) -- the case the old 129 B buffer already failed. */
static void test_login_body_fits_quote_and_backslash_password(void) {
    char pw[WA_PW_MAX + 1];
    worst_pw(pw, '"');
    TEST_ASSERT_TRUE(login_body_len(pw) + 1 <= HTTP_LOGIN_BODY_MAX);
    worst_pw(pw, '\\');
    TEST_ASSERT_TRUE(login_body_len(pw) + 1 <= HTTP_LOGIN_BODY_MAX);
}

static void test_password_change_body_fits_two_worst_case_passwords(void) {
    char old_pw[WA_PW_MAX + 1], new_pw[WA_PW_MAX + 1];
    worst_pw(old_pw, '\x01');
    worst_pw(new_pw, '\x1f');
    TEST_ASSERT_EQUAL_size_t(HTTP_PASSWORD_BODY_MAX, password_body_len(old_pw, new_pw) + 1);

    worst_pw(old_pw, '"');
    worst_pw(new_pw, '\\');
    TEST_ASSERT_TRUE(password_body_len(old_pw, new_pw) + 1 <= HTTP_PASSWORD_BODY_MAX);
}

/* A plain-text password must still be nowhere near the cap -- the bounds are
   for the pathological case, not an excuse to accept unbounded bodies. */
static void test_ordinary_password_is_far_inside_the_bound(void) {
    TEST_ASSERT_TRUE(login_body_len("hillgrow1") < 40);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_login_body_fits_worst_case_password);
    RUN_TEST(test_login_body_fits_quote_and_backslash_password);
    RUN_TEST(test_password_change_body_fits_two_worst_case_passwords);
    RUN_TEST(test_ordinary_password_is_far_inside_the_bound);
    return UNITY_END();
}
