#include "unity.h"
#include "rescue_handover.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_handover_pack_unpack_roundtrip(void) {
    hg_handover_t in = {
        .expect_link = 1,
        .ssid = "HillGrow",
        .pass = "hillgrow1",
        .url = "http://192.168.7.7/fw/zone.bin"
    };

    uint8_t buf[HG_HANDOVER_LEN];
    hg_handover_t out;

    int pack_ret = hg_handover_pack(&in, buf);
    TEST_ASSERT_EQUAL_INT(0, pack_ret);

    int unpack_ret = hg_handover_unpack(buf, &out);
    TEST_ASSERT_EQUAL_INT(0, unpack_ret);

    TEST_ASSERT_EQUAL_UINT8(in.expect_link, out.expect_link);
    TEST_ASSERT_EQUAL_STRING(in.ssid, out.ssid);
    TEST_ASSERT_EQUAL_STRING(in.pass, out.pass);
    TEST_ASSERT_EQUAL_STRING(in.url, out.url);
}

static void test_handover_flipped_byte_rejects(void) {
    hg_handover_t in = {
        .expect_link = 1,
        .ssid = "HillGrow",
        .pass = "hillgrow1",
        .url = "http://192.168.7.7/fw/zone.bin"
    };

    uint8_t buf[HG_HANDOVER_LEN];
    hg_handover_t out;

    hg_handover_pack(&in, buf);

    /* Flip a byte in the middle */
    buf[50] ^= 0xFF;

    int unpack_ret = hg_handover_unpack(buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, unpack_ret);
}

static void test_handover_wrong_magic_rejects(void) {
    hg_handover_t in = {
        .expect_link = 1,
        .ssid = "HillGrow",
        .pass = "hillgrow1",
        .url = "http://192.168.7.7/fw/zone.bin"
    };

    uint8_t buf[HG_HANDOVER_LEN];
    hg_handover_t out;

    hg_handover_pack(&in, buf);

    /* Flip magic bytes */
    buf[0] ^= 0xFF;

    int unpack_ret = hg_handover_unpack(buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, unpack_ret);
}

static void test_handover_33char_ssid_rejects(void) {
    hg_handover_t in = {
        .expect_link = 1,
        .ssid = "123456789012345678901234567890123",  /* 33 chars, too long for ssid[33] with NUL */
        .pass = "hillgrow1",
        .url = "http://192.168.7.7/fw/zone.bin"
    };

    uint8_t buf[HG_HANDOVER_LEN];

    int pack_ret = hg_handover_pack(&in, buf);
    TEST_ASSERT_EQUAL_INT(-1, pack_ret);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_handover_pack_unpack_roundtrip);
    RUN_TEST(test_handover_flipped_byte_rejects);
    RUN_TEST(test_handover_wrong_magic_rejects);
    RUN_TEST(test_handover_33char_ssid_rejects);
    return UNITY_END();
}
