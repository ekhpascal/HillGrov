#include <string.h>
#include "unity.h"
#include "hg_blob.h"

#define MAGIC 0x46434748u /* 'HGCF' LE */

void setUp(void) {}
void tearDown(void) {}

static void test_crc32_vector(void) {
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, hg_crc32(0, "123456789", 9));
    /* incremental == one-shot */
    uint32_t c = hg_crc32(0, "12345", 5);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, hg_crc32(c, "6789", 4));
}

static void test_wrap_unwrap_roundtrip(void) {
    uint8_t payload[32], out[64], back[32];
    for (int i = 0; i < 32; i++) payload[i] = (uint8_t)(i * 7);
    size_t n = hg_blob_wrap(MAGIC, 1, 17, payload, 32, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(48, n);
    uint32_t gen = 0;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_OK, hg_blob_unwrap(MAGIC, 1, 1, out, n, back, 32, &gen));
    TEST_ASSERT_EQUAL_UINT32(17, gen);
    TEST_ASSERT_EQUAL_MEMORY(payload, back, 32);
}

static void test_unwrap_rejects_in_order(void) {
    uint8_t payload[8] = {1,2,3,4,5,6,7,8}, out[24], back[8];
    uint32_t gen;
    size_t n = hg_blob_wrap(MAGIC, 2, 5, payload, 8, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t(24, n);
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_SHORT, hg_blob_unwrap(MAGIC, 2, 1, out, 15, back, 8, &gen));
    out[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_MAGIC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
    out[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_LENGTH, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 4, &gen)); /* cap too small */
    out[20] ^= 0x01; /* flip a payload bit */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_CRC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
    out[20] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_VERSION_NEWER, hg_blob_unwrap(MAGIC, 1, 1, out, n, back, 8, &gen)); /* blob v2 > fw v1 */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_VERSION_OLD, hg_blob_unwrap(MAGIC, 3, 3, out, n, back, 8, &gen));   /* v2 < min 3 */
    /* version byte is CRC-covered */
    out[4] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(HG_BLOB_E_CRC, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 8, &gen));
}

static void test_unwrap_migrated_zero_fills(void) {
    uint8_t payload[8] = {9,9,9,9,9,9,9,9}, out[24], back[16];
    uint32_t gen;
    size_t n = hg_blob_wrap(MAGIC, 1, 3, payload, 8, out, sizeof out);
    memset(back, 0xAA, sizeof back);
    /* shorter old blob into a bigger current struct -> MIGRATED, tail zeroed */
    TEST_ASSERT_EQUAL_INT(HG_BLOB_MIGRATED, hg_blob_unwrap(MAGIC, 2, 1, out, n, back, 16, &gen));
    TEST_ASSERT_EQUAL_UINT8(9, back[7]);
    TEST_ASSERT_EQUAL_UINT8(0, back[8]);
    TEST_ASSERT_EQUAL_UINT8(0, back[15]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_crc32_vector);
    RUN_TEST(test_wrap_unwrap_roundtrip);
    RUN_TEST(test_unwrap_rejects_in_order);
    RUN_TEST(test_unwrap_migrated_zero_fills);
    return UNITY_END();
}
