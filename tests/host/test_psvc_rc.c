/* The refusal vocabulary both faces speak. Two properties matter: every value
   has exactly the token the web already sends (so a refusal reads the same on
   the phone and on the glass), and the legacy mapping gives the CLI rows and
   POST /api/wifi the SAME -1/-2/-3 they answered before panel_svc existed
   (master_cmds.h:46-60) -- including master-config contention reported as
   -2 "could not be stored" (D10). */
#include <string.h>
#include "unity.h"
#include "psvc_rc.h"

void setUp(void) {}
void tearDown(void) {}

static const char *const WANT[] = {
    "OK", "BUSY", "INVALID", "BAD_JSON", "INVALID_FIELD", "VALIDATION",
    "NO_CACHE", "ZONE_UNKNOWN", "ZONE_NOT_ONLINE", "STORAGE", "INTERNAL",
    "FLEET_BUSY", "FLEET_REJECTED", "NOT_ACTIVE",
    "UPLOAD_ACTIVE", "FLEET_ACTIVE", "TRIAL_PENDING", "NO_SLOT", "LOW_HEAP",
    "TOO_LARGE", "IMAGE_MISMATCH", "WRITE_FAILED", "STALLED", "RECV_FAILED",
    "ZONE_FW_BUSY",
};

static void test_count_matches_the_token_list(void) {
    TEST_ASSERT_EQUAL_INT((int)(sizeof WANT / sizeof WANT[0]), (int)PSVC_RC_COUNT);
    TEST_ASSERT_EQUAL_INT(25, (int)PSVC_RC_COUNT);
}

static void test_every_value_has_its_token(void) {
    for (int i = 0; i < (int)PSVC_RC_COUNT; i++)
        TEST_ASSERT_EQUAL_STRING(WANT[i], psvc_rc_token((psvc_rc_t)i));
}

static void test_out_of_range_is_internal(void) {
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token(PSVC_RC_COUNT));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token((psvc_rc_t)200));
    TEST_ASSERT_EQUAL_STRING("INTERNAL", psvc_rc_token((psvc_rc_t)-1));
    TEST_ASSERT_EQUAL_INT(-3, psvc_rc_to_net_legacy(PSVC_RC_COUNT));
    TEST_ASSERT_EQUAL_INT(-3, psvc_rc_to_net_legacy((psvc_rc_t)-1));
}

static void test_legacy_mapping_for_every_value(void) {
    for (int i = 0; i < (int)PSVC_RC_COUNT; i++) {
        psvc_rc_t rc = (psvc_rc_t)i;
        int want;
        switch (rc) {
        case PSVC_OK:                                    want = 0;  break;
        case PSVC_E_INVALID: case PSVC_E_VALIDATION:
        case PSVC_E_INVALID_FIELD: case PSVC_E_BAD_JSON:
        case PSVC_E_ZONE_UNKNOWN:                        want = -1; break;
        case PSVC_E_BUSY: case PSVC_E_STORAGE:           want = -2; break;
        default:                                         want = -3; break;
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, psvc_rc_to_net_legacy(rc), psvc_rc_token(rc));
    }
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_count_matches_the_token_list);
    RUN_TEST(test_every_value_has_its_token);
    RUN_TEST(test_out_of_range_is_internal);
    RUN_TEST(test_legacy_mapping_for_every_value);
    return UNITY_END(); }
