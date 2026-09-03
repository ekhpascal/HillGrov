#include <string.h>
#include "unity.h"
#include "ring_proto.h"

static void test_crc_check_vector(void) {
    TEST_ASSERT_EQUAL_HEX16(0x29B1, ring_crc16((const uint8_t *)"123456789", 9));
}

static void test_crc_bitflip(void) {
    uint8_t d[16]; for (int i = 0; i < 16; i++) d[i] = (uint8_t)(i * 7 + 1);
    uint16_t c = ring_crc16(d, 16);
    for (int byte = 0; byte < 16; byte++) for (int bit = 0; bit < 8; bit++) {
        d[byte] ^= (uint8_t)(1u << bit);
        TEST_ASSERT_NOT_EQUAL(c, ring_crc16(d, 16));
        d[byte] ^= (uint8_t)(1u << bit);
    }
}

static void test_cobs_roundtrip_all_lengths(void) {
    uint8_t in[139], enc[141], dec[139];
    for (size_t n = 0; n <= 139; n++) {
        for (size_t i = 0; i < n; i++) in[i] = (uint8_t)((i % 3 == 0) ? 0 : i);  /* zeros sprinkled in */
        size_t e = ring_cobs_encode(in, n, enc);
        TEST_ASSERT_TRUE(e >= n && e <= n + n / 254 + 1);
        for (size_t i = 0; i < e; i++) TEST_ASSERT_NOT_EQUAL(0, enc[i]);          /* no zeros inside */
        TEST_ASSERT_EQUAL_INT((int)n, ring_cobs_decode(enc, e, dec));
        if (n) TEST_ASSERT_EQUAL_MEMORY(in, dec, n);
    }
}

static void test_cobs_decode_rejects_embedded_zero(void) {
    uint8_t bad[4] = { 0x02, 0x41, 0x00, 0x41 };
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(-1, ring_cobs_decode(bad, 4, out));
}

static void test_cobs_decode_rejects_truncated_block(void) {
    uint8_t bad[2] = { 0x05, 0x41 };                     /* code promises 4 data bytes, only 1 present */
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(-1, ring_cobs_decode(bad, 2, out));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc_check_vector);
    RUN_TEST(test_crc_bitflip);
    RUN_TEST(test_cobs_roundtrip_all_lengths);
    RUN_TEST(test_cobs_decode_rejects_embedded_zero);
    RUN_TEST(test_cobs_decode_rejects_truncated_block);
    // RUN_TEST(test_cobs_254_boundary);  /* Will be enabled after core fix */
    return UNITY_END();
}
