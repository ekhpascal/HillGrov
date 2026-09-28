/* test_pnl_msg.c -- one refusal vocabulary, one set of sentences: every context x every result is a
 * non-empty printable-ASCII sentence; the web-worded ones match the web exactly. */
#include <string.h>
#include "unity.h"
#include "pnl_msg.h"

void setUp(void) {}
void tearDown(void) {}

static const char *M(pnl_msg_ctx_t c, psvc_rc_t rc, const pnl_msg_arg_t *a) {
    static char b[200];
    int n = pnl_msg(c, rc, a, b, sizeof b);
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(n, (int)strlen(b));
    return b;
}
static void assert_ascii(const char *s) {
    TEST_ASSERT_TRUE(s[0] != '\0');
    for (const char *p = s; *p; p++)
        TEST_ASSERT_TRUE_MESSAGE((unsigned char)*p >= 0x20 && (unsigned char)*p <= 0x7E, s);
}

static void test_every_context_every_result(void) {
    pnl_msg_arg_t a = { .zone = 2, .version = "1.4.2", .slot = "ota_1", .len = 1234 };
    for (int c = 0; c <= PNL_CTX_COUNT; c++)              /* includes one out-of-range context */
        for (int rc = 0; rc <= PSVC_RC_COUNT; rc++) {      /* includes one out-of-range result */
            assert_ascii(M((pnl_msg_ctx_t)c, (psvc_rc_t)rc, &a));
            assert_ascii(M((pnl_msg_ctx_t)c, (psvc_rc_t)rc, NULL));
        }
}
static void test_zone_texts(void) {
    TEST_ASSERT_EQUAL_STRING("Zone config not adopted yet -- the zone must come online and sync at least once before it can be configured.",
                             M(PNL_CTX_ZONE_LOAD, PSVC_E_NO_CACHE, NULL));
    TEST_ASSERT_EQUAL_STRING("Unknown zone.", M(PNL_CTX_ZONE_LOAD, PSVC_E_ZONE_UNKNOWN, NULL));
    TEST_ASSERT_EQUAL_STRING("Zone is offline.", M(PNL_CTX_ZONE_LOAD, PSVC_E_ZONE_NOT_ONLINE, NULL));
    TEST_ASSERT_EQUAL_STRING("STORAGE", M(PNL_CTX_ZONE_LOAD, PSVC_E_STORAGE, NULL));
    TEST_ASSERT_EQUAL_STRING("Queued, pushing to zone", M(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("Zone busy, retry", M(PNL_CTX_ZONE_SAVE, PSVC_E_BUSY, NULL));
    TEST_ASSERT_EQUAL_STRING("Zone is offline -- nothing was saved", M(PNL_CTX_ZONE_SAVE, PSVC_E_ZONE_NOT_ONLINE, NULL));
    TEST_ASSERT_EQUAL_STRING("VALIDATION", M(PNL_CTX_ZONE_SAVE, PSVC_E_VALIDATION, NULL));
    TEST_ASSERT_EQUAL_STRING("NO_CACHE", M(PNL_CTX_ZONE_SAVE, PSVC_E_NO_CACHE, NULL));
}
static void test_master_and_wifi_texts(void) {
    TEST_ASSERT_EQUAL_STRING("Saved.", M(PNL_CTX_MCFG_SAVE, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("Master config busy (another change is being applied), retry", M(PNL_CTX_MCFG_SAVE, PSVC_E_BUSY, NULL));
    TEST_ASSERT_EQUAL_STRING("INVALID_FIELD", M(PNL_CTX_MCFG_SAVE, PSVC_E_INVALID_FIELD, NULL));
    TEST_ASSERT_EQUAL_STRING("Saved -- joining...", M(PNL_CTX_WIFI_JOIN, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("Saved.", M(PNL_CTX_WIFI_AP, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("Master config busy (another change is being applied), retry", M(PNL_CTX_TZ, PSVC_E_BUSY, NULL));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", M(PNL_CTX_SCAN, PSVC_E_INTERNAL, NULL));
}
static void test_password_texts(void) {
    TEST_ASSERT_EQUAL_STRING("Password changed. Every phone and browser was logged out -- log in again with the new password.",
                             M(PNL_CTX_PASSWORD, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("New password invalid (8 to 63 characters)", M(PNL_CTX_PASSWORD, PSVC_E_INVALID, NULL));
    TEST_ASSERT_EQUAL_STRING("Failed (STORAGE)", M(PNL_CTX_PASSWORD, PSVC_E_STORAGE, NULL));
}
static void test_fleet_texts(void) {
    pnl_msg_arg_t a = { .zone = 3 };
    TEST_ASSERT_EQUAL_STRING("Update queued for zone 3.", M(PNL_CTX_FLEET_ZONE, PSVC_OK, &a));
    TEST_ASSERT_EQUAL_STRING("Fleet update queued.", M(PNL_CTX_FLEET_ALL, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("Fleet update aborted.", M(PNL_CTX_FLEET_ABORT, PSVC_OK, NULL));
    TEST_ASSERT_EQUAL_STRING("FLEET_BUSY", M(PNL_CTX_FLEET_ZONE, PSVC_E_FLEET_BUSY, &a));
}
static void test_fw_texts(void) {
    pnl_msg_arg_t a = { .version = "1.4.2", .slot = "ota_1", .len = 123456 };
    TEST_ASSERT_EQUAL_STRING("Uploaded v1.4.2 to ota_1.", M(PNL_CTX_FW_MASTER, PSVC_OK, &a));
    TEST_ASSERT_EQUAL_STRING("Uploaded (123456 bytes).", M(PNL_CTX_FW_ZONE, PSVC_OK, &a));
    TEST_ASSERT_EQUAL_STRING("Not a master image -- nothing was erased", M(PNL_CTX_FW_MASTER, PSVC_E_IMAGE_MISMATCH, &a));
    TEST_ASSERT_EQUAL_STRING("Not a zone image -- nothing was erased", M(PNL_CTX_FW_ZONE, PSVC_E_IMAGE_MISMATCH, &a));
    TEST_ASSERT_EQUAL_STRING("An OTA trial is running -- wait for it to pass or SET OTA CONFIRM", M(PNL_CTX_FW_MASTER, PSVC_E_TRIAL_PENDING, &a));
    TEST_ASSERT_EQUAL_STRING("A fleet update is running", M(PNL_CTX_FW_ZONE, PSVC_E_FLEET_ACTIVE, &a));
    TEST_ASSERT_EQUAL_STRING("Another upload is in progress", M(PNL_CTX_FW_ZONE, PSVC_E_UPLOAD_ACTIVE, &a));
    TEST_ASSERT_EQUAL_STRING("A zone is downloading the image -- retry in a minute", M(PNL_CTX_FW_ZONE, PSVC_E_ZONE_FW_BUSY, &a));
    TEST_ASSERT_EQUAL_STRING("microSD read failed", M(PNL_CTX_FW_ZONE, PSVC_E_RECV_FAILED, &a));
    TEST_ASSERT_EQUAL_STRING("TOO_LARGE", M(PNL_CTX_FW_MASTER, PSVC_E_TOO_LARGE, &a));
    TEST_ASSERT_EQUAL_STRING("Uploaded v? to ?.", M(PNL_CTX_FW_MASTER, PSVC_OK, NULL));
}
static void test_foreign_bytes_sanitised(void) {
    pnl_msg_arg_t a = { .version = "1.\xC3\xA9", .slot = "ota_0" };
    TEST_ASSERT_EQUAL_STRING("Uploaded v1.?? to ota_0.", M(PNL_CTX_FW_MASTER, PSVC_OK, &a));
}
static void test_clipping_and_bad_args(void) {
    char b[8];
    TEST_ASSERT_EQUAL_INT(7, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("Queued,", b);
    TEST_ASSERT_EQUAL_INT(-1, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, NULL, 8));
    TEST_ASSERT_EQUAL_INT(-1, pnl_msg(PNL_CTX_ZONE_SAVE, PSVC_OK, NULL, b, 0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_every_context_every_result);
    RUN_TEST(test_zone_texts);
    RUN_TEST(test_master_and_wifi_texts);
    RUN_TEST(test_password_texts);
    RUN_TEST(test_fleet_texts);
    RUN_TEST(test_fw_texts);
    RUN_TEST(test_foreign_bytes_sanitised);
    RUN_TEST(test_clipping_and_bad_args);
    return UNITY_END();
}
