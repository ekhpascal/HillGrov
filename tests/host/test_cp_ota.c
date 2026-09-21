/* The gate, not the transfer. The bring-up spike shipped this ungated and
   re-flashed the C6 on every single boot: pointless flash wear on the radio
   plus ~7 s added to every start-up. It also must not key on a value that is
   structurally constant -- the SP4 final review found exactly that class of bug
   in the HW-plane presence check, where a "generation" was always 0 and the
   guard therefore always took one branch. */
#include "unity.h"
#include "cp_ota.h"
#include "hg_blob.h"   /* hg_crc32 -- same check-value family cp_ota_parse_header()'s
                           body_crc expects (production code computes it the same way,
                           streaming from flash; see components/cp_ota/cp_ota.c) */

void setUp(void) {}
void tearDown(void) {}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void build_header(uint8_t hdr[CP_OTA_HDR_LEN], uint32_t magic, uint32_t len, uint32_t crc) {
    wr32(hdr + 0, magic);
    wr32(hdr + 4, len);
    wr32(hdr + 8, crc);
    wr32(hdr + 12, 0);
}

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

/* Fix round 1: cp_fw carries the same 16-byte HGFW header zone_fw does
   (components/fw_srv/fw_srv.c's FW_HDR_MAGIC/FW_HDR_LEN,
   tools/flash_app.py's build_hgfw_image()) -- a data partition can't
   recover its payload length by parsing esp_image segments, so it has to be
   carried explicitly. cp_ota_parse_header() is the pure half of that check
   (the caller streams the body and computes body_crc via hg_crc32, exactly
   like production code does in cp_ota.c). */

void test_a_good_header_validates_and_yields_the_right_length(void) {
    static const uint8_t body[] = "a co-processor image, or at least some bytes standing in for one";
    uint32_t crc = hg_crc32(0, body, sizeof body);
    uint8_t hdr[CP_OTA_HDR_LEN];
    build_header(hdr, CP_OTA_HDR_MAGIC, (uint32_t)sizeof body, crc);

    uint32_t len_out = 0;
    TEST_ASSERT_EQUAL_INT(0, cp_ota_parse_header(hdr, 0x180000u, crc, &len_out));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof body, len_out);
}

/* Every rejection test below seeds len_out with this sentinel and asserts
   it stayed untouched -- fix round 2 minor: these tests used to leave it
   unchecked (some even seeded with 0 already, which can't distinguish
   "untouched" from "written zero"), so a cp_ota_parse_header() that wrote a
   garbage length on the way to returning -1 would have passed silently. */
#define UNTOUCHED_SENTINEL 0xDEADBEEFu

void test_a_bad_magic_is_rejected(void) {
    /* Also covers the erased-partition case the first cut of this component
       checked for directly (all 0xFF): 0xFFFFFFFF is never CP_OTA_HDR_MAGIC,
       so no separate check is needed for it. */
    uint8_t hdr[CP_OTA_HDR_LEN];
    build_header(hdr, 0xFFFFFFFFu, 100u, 0u);

    uint32_t len_out = UNTOUCHED_SENTINEL;
    TEST_ASSERT_EQUAL_INT(-1, cp_ota_parse_header(hdr, 0x180000u, 0u, &len_out));
    TEST_ASSERT_EQUAL_UINT32(UNTOUCHED_SENTINEL, len_out);
}

void test_len_zero_is_rejected(void) {
    uint8_t hdr[CP_OTA_HDR_LEN];
    build_header(hdr, CP_OTA_HDR_MAGIC, 0u, 0u);

    uint32_t len_out = UNTOUCHED_SENTINEL;
    TEST_ASSERT_EQUAL_INT(-1, cp_ota_parse_header(hdr, 0x180000u, 0u, &len_out));
    TEST_ASSERT_EQUAL_UINT32(UNTOUCHED_SENTINEL, len_out);
}

void test_a_length_beyond_the_partition_is_rejected(void) {
    uint8_t hdr[CP_OTA_HDR_LEN];
    /* len == part_size means header(16) + len overflows part_size by 16. */
    build_header(hdr, CP_OTA_HDR_MAGIC, 0x180000u, 0u);

    uint32_t len_out = UNTOUCHED_SENTINEL;
    TEST_ASSERT_EQUAL_INT(-1, cp_ota_parse_header(hdr, 0x180000u, 0u, &len_out));
    TEST_ASSERT_EQUAL_UINT32(UNTOUCHED_SENTINEL, len_out);
}

void test_the_exact_fit_boundary_is_accepted(void) {
    /* len == part_size - CP_OTA_HDR_LEN is the LARGEST length that still
       fits (CP_OTA_HDR_LEN + len == part_size exactly, not >) and MUST be
       accepted, not off-by-one rejected -- the counterpart to the
       length-beyond-the-partition test above. A small part_size keeps this
       cheap: no need for a real ~1.5 MB body to exercise the boundary. */
    static const uint8_t body[16] = {0};
    const uint32_t part_size = CP_OTA_HDR_LEN + (uint32_t)sizeof body;
    uint32_t crc = hg_crc32(0, body, sizeof body);
    uint8_t hdr[CP_OTA_HDR_LEN];
    build_header(hdr, CP_OTA_HDR_MAGIC, (uint32_t)sizeof body, crc);

    uint32_t len_out = 0;
    TEST_ASSERT_EQUAL_INT(0, cp_ota_parse_header(hdr, part_size, crc, &len_out));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof body, len_out);
}

void test_a_crc_mismatch_is_rejected(void) {
    static const uint8_t body[] = "some other bytes, transferred correctly or not";
    uint32_t crc = hg_crc32(0, body, sizeof body);
    uint8_t hdr[CP_OTA_HDR_LEN];
    build_header(hdr, CP_OTA_HDR_MAGIC, (uint32_t)sizeof body, crc);

    /* The caller's computed body_crc (over what was actually staged/streamed)
       disagrees with the header's claim -- e.g. a truncated or corrupted
       transfer left a different body behind the same header bytes. */
    uint32_t len_out = UNTOUCHED_SENTINEL;
    TEST_ASSERT_EQUAL_INT(-1, cp_ota_parse_header(hdr, 0x180000u, crc ^ 1u, &len_out));
    TEST_ASSERT_EQUAL_UINT32(UNTOUCHED_SENTINEL, len_out);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_matching_version_needs_no_update);
    RUN_TEST(test_a_zero_version_needs_an_update);
    RUN_TEST(test_a_differing_minor_needs_an_update);
    RUN_TEST(test_a_differing_major_needs_an_update);
    RUN_TEST(test_a_differing_patch_alone_does_not);
    RUN_TEST(test_a_good_header_validates_and_yields_the_right_length);
    RUN_TEST(test_a_bad_magic_is_rejected);
    RUN_TEST(test_len_zero_is_rejected);
    RUN_TEST(test_a_length_beyond_the_partition_is_rejected);
    RUN_TEST(test_the_exact_fit_boundary_is_accepted);
    RUN_TEST(test_a_crc_mismatch_is_rejected);
    return UNITY_END();
}
