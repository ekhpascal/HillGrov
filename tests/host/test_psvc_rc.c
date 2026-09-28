/* The refusal vocabulary both faces speak. Two properties matter: every value
   has exactly the token the web already sends (so a refusal reads the same on
   the phone and on the glass), and the legacy mapping gives the CLI rows and
   POST /api/wifi the SAME -1/-2/-3 they answered before panel_svc existed
   (master_cmds.h:46-60) -- including master-config contention reported as
   -2 "could not be stored" (D10). */
#include <string.h>
#include "unity.h"
#include "psvc_rc.h"
#include "psvc_fleet.h"

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

/* node_mgr stubs: psvc_fleet.c is linked here so its range check and wiring are pinned, not just the mapping. */
static int     g_zone_rc, g_all_rc, g_abort_rc, g_zone_calls;
static uint8_t g_zone_last;
int node_mgr_fw_zone(uint8_t zone) { g_zone_calls++; g_zone_last = zone; return g_zone_rc; }
int node_mgr_fw_all(void)          { return g_all_rc; }
int node_mgr_fw_abort(void)        { return g_abort_rc; }

static void test_fleet_start_codes(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_rc_from_fleet(0, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_BUSY, psvc_rc_from_fleet(-2, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_rc_from_fleet(-1, 0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_rc_from_fleet(-7, 0));   /* http_fleet.c: anything but -2 */
}

static void test_fleet_abort_codes(void) {
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_rc_from_fleet(0, 1));
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_rc_from_fleet(-1, 1));
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_rc_from_fleet(-2, 1));
}

static void test_fleet_tokens_are_the_webs(void) {
    TEST_ASSERT_EQUAL_STRING("FLEET_BUSY", psvc_rc_token(psvc_rc_from_fleet(-2, 0)));
    TEST_ASSERT_EQUAL_STRING("FLEET_REJECTED", psvc_rc_token(psvc_rc_from_fleet(-1, 0)));
    TEST_ASSERT_EQUAL_STRING("NOT_ACTIVE", psvc_rc_token(psvc_rc_from_fleet(-1, 1)));
}

static void test_fleet_zone_range_and_wiring(void) {
    g_zone_calls = 0; g_zone_rc = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fleet_zone(0));
    TEST_ASSERT_EQUAL_INT(PSVC_E_INVALID, psvc_fleet_zone(9));
    TEST_ASSERT_EQUAL_INT(0, g_zone_calls);                               /* refused before node_mgr is asked */
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_fleet_zone(2));
    TEST_ASSERT_EQUAL_UINT8(2, g_zone_last);
    g_zone_rc = -2;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_BUSY, psvc_fleet_zone(8));
    g_all_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_FLEET_REJECTED, psvc_fleet_all());
    g_abort_rc = -1;
    TEST_ASSERT_EQUAL_INT(PSVC_E_NOT_ACTIVE, psvc_fleet_abort());
    g_abort_rc = 0;
    TEST_ASSERT_EQUAL_INT(PSVC_OK, psvc_fleet_abort());
}

static void test_fleet_idle(void) {
    TEST_ASSERT_EQUAL_INT(1, psvc_fleet_idle("IDLE"));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle("2 UPDATING"));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle("idle"));      /* the token is exact (fleet_seq.c) */
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle(""));
    TEST_ASSERT_EQUAL_INT(0, psvc_fleet_idle(NULL));
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_count_matches_the_token_list);
    RUN_TEST(test_every_value_has_its_token);
    RUN_TEST(test_out_of_range_is_internal);
    RUN_TEST(test_legacy_mapping_for_every_value);
    RUN_TEST(test_fleet_start_codes);
    RUN_TEST(test_fleet_abort_codes);
    RUN_TEST(test_fleet_tokens_are_the_webs);
    RUN_TEST(test_fleet_zone_range_and_wiring);
    RUN_TEST(test_fleet_idle);
    return UNITY_END(); }
