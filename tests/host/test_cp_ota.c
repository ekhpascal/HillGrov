/* The gate, not the transfer. The bring-up spike shipped this ungated and
   re-flashed the C6 on every single boot: pointless flash wear on the radio
   plus ~7 s added to every start-up. It also must not key on a value that is
   structurally constant -- the SP4 final review found exactly that class of bug
   in the HW-plane presence check, where a "generation" was always 0 and the
   guard therefore always took one branch. */
#include "unity.h"
#include "cp_ota.h"

void setUp(void) {}
void tearDown(void) {}

void test_matching_version_needs_no_update(void) {
    TEST_ASSERT_EQUAL_INT(0, cp_ota_needed(CP_OTA_HOST_VERSION));
}

void test_a_zero_version_needs_an_update(void) {
    /* The factory Waveshare co-processor reports 0.0.0. */
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00000000u));
}

void test_a_differing_minor_needs_an_update(void) {
    /* 3.1.7 vs 3.0.7. The version word is EH_VERSION_VAL(major, minor, patch)
       == (major << 16) | (minor << 8) | patch, so the MINOR byte is bits 8-15 --
       0x00030006 would be 3.0.6, a patch difference, not a minor one. */
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00030107u));
}

void test_a_differing_major_needs_an_update(void) {
    TEST_ASSERT_EQUAL_INT(1, cp_ota_needed(0x00020007u));   /* 2.0.7 vs 3.0.7 */
}

void test_a_differing_patch_alone_does_not(void) {
    /* Read from esp_hosted's own implementation, not inferred: after an exact
       compare, eh_host_mcu_transport_verify_fw_compat() returns +-1 when the
       major or the minor byte differs, and for a patch-only difference it logs
       "patch version differs (compatible)" and returns 0. cp_ota_needed() must
       agree with it, or the host would re-flash the radio for a patch bump that
       esp_hosted itself considers compatible. */
    TEST_ASSERT_EQUAL_INT(0, cp_ota_needed(0x00030008u));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_matching_version_needs_no_update);
    RUN_TEST(test_a_zero_version_needs_an_update);
    RUN_TEST(test_a_differing_minor_needs_an_update);
    RUN_TEST(test_a_differing_major_needs_an_update);
    RUN_TEST(test_a_differing_patch_alone_does_not);
    return UNITY_END();
}
