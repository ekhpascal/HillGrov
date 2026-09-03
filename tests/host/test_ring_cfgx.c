#include <string.h>
#include <stdint.h>
#include "unity.h"
#include "ring_proto.h"

/* Hand-build a raw CFG_CHUNK payload for cases the chunker itself would never produce
   (malformed total/count/idx), so the assembler's reject paths can be exercised directly. */
static size_t build_raw(uint8_t *out, uint8_t kind, uint32_t gen, uint8_t idx, uint8_t count,
                        uint16_t total, const uint8_t *data, size_t data_len) {
    out[0] = kind;
    out[1] = (uint8_t)(gen & 0xFF);
    out[2] = (uint8_t)((gen >> 8) & 0xFF);
    out[3] = (uint8_t)((gen >> 16) & 0xFF);
    out[4] = (uint8_t)((gen >> 24) & 0xFF);
    out[5] = idx;
    out[6] = count;
    out[7] = (uint8_t)(total & 0xFF);
    out[8] = (uint8_t)((total >> 8) & 0xFF);
    if (data_len) memcpy(out + 9, data, data_len);
    return 9 + data_len;
}

static void fill_pattern(uint8_t *blob, size_t n) {
    for (size_t i = 0; i < n; i++) blob[i] = (uint8_t)i;
}

/* ========== ring_cfg_chunk_count ========== */

static void test_chunk_count_ceil_division(void) {
    TEST_ASSERT_EQUAL_INT(0, ring_cfg_chunk_count(0));
    TEST_ASSERT_EQUAL_INT(1, ring_cfg_chunk_count(1));
    TEST_ASSERT_EQUAL_INT(1, ring_cfg_chunk_count(112));
    TEST_ASSERT_EQUAL_INT(2, ring_cfg_chunk_count(113));
    TEST_ASSERT_EQUAL_INT(7, ring_cfg_chunk_count(700));
    TEST_ASSERT_EQUAL_INT(7, ring_cfg_chunk_count(768));
}

/* ========== ring_cfg_chunk_build: 700 B blob, every header field asserted per chunk ========== */

static void test_chunk_build_700_byte_blob_headers_and_data(void) {
    uint8_t blob[700];
    fill_pattern(blob, sizeof blob);
    const uint32_t gen = 0xABCD1234u;
    const uint8_t kind = 1;   /* CFG */

    for (uint8_t idx = 0; idx < 7; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int len = ring_cfg_chunk_build(kind, gen, blob, sizeof blob, idx, payload, sizeof payload);

        size_t expect_data_len = (idx < 6) ? 112 : 28;
        TEST_ASSERT_EQUAL_INT((int)(9 + expect_data_len), len);

        TEST_ASSERT_EQUAL_UINT8(kind, payload[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(gen & 0xFF), payload[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((gen >> 8) & 0xFF), payload[2]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((gen >> 16) & 0xFF), payload[3]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((gen >> 24) & 0xFF), payload[4]);
        TEST_ASSERT_EQUAL_UINT8(idx, payload[5]);
        TEST_ASSERT_EQUAL_UINT8(7, payload[6]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(700 & 0xFF), payload[7]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)((700 >> 8) & 0xFF), payload[8]);

        TEST_ASSERT_EQUAL_MEMORY(blob + (size_t)idx * 112, payload + 9, expect_data_len);
    }
}

static void test_chunk_build_idx_out_of_range_rejected(void) {
    uint8_t blob[700];
    fill_pattern(blob, sizeof blob);
    uint8_t payload[9 + RING_CFG_DATA_MAX];
    TEST_ASSERT_EQUAL_INT(-1, ring_cfg_chunk_build(1, 1, blob, sizeof blob, 7, payload, sizeof payload));
}

/* ========== ring_casm_feed: happy paths ========== */

static void test_feed_in_order_completes_byte_equal(void) {
    uint8_t blob[700];
    fill_pattern(blob, sizeof blob);
    const uint32_t gen = 7;
    const uint8_t kind = 1;

    ring_casm_t a;
    ring_casm_init(&a);

    for (uint8_t idx = 0; idx < 7; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(kind, gen, blob, sizeof blob, idx, payload, sizeof payload);
        int rc = ring_casm_feed(&a, payload, (size_t)plen, 1000);
        if (idx < 6) TEST_ASSERT_EQUAL_INT(0, rc);
        else         TEST_ASSERT_EQUAL_INT(1, rc);
    }
    TEST_ASSERT_EQUAL_MEMORY(blob, a.buf, sizeof blob);
    TEST_ASSERT_EQUAL_UINT16(700, a.total);
}

static void test_feed_out_of_order_idx3_first_completes(void) {
    uint8_t blob[700];
    fill_pattern(blob, sizeof blob);
    const uint32_t gen = 3;
    const uint8_t kind = 1;
    uint8_t order[7] = { 3, 0, 6, 1, 5, 2, 4 };

    ring_casm_t a;
    ring_casm_init(&a);

    int rc = 0;
    for (int i = 0; i < 7; i++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(kind, gen, blob, sizeof blob, order[i], payload, sizeof payload);
        rc = ring_casm_feed(&a, payload, (size_t)plen, 2000);
        if (i < 6) TEST_ASSERT_EQUAL_INT(0, rc);
    }
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_MEMORY(blob, a.buf, sizeof blob);
}

static void test_feed_duplicate_chunk_no_corruption(void) {
    uint8_t blob[700];
    fill_pattern(blob, sizeof blob);
    const uint32_t gen = 9;
    const uint8_t kind = 1;

    ring_casm_t a;
    ring_casm_init(&a);

    uint8_t p0[9 + RING_CFG_DATA_MAX];
    int p0len = ring_cfg_chunk_build(kind, gen, blob, sizeof blob, 0, p0, sizeof p0);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, p0, (size_t)p0len, 0));
    TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, p0, (size_t)p0len, 0));   /* duplicate idx 0 */

    int rc = 0;
    for (uint8_t idx = 1; idx < 7; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(kind, gen, blob, sizeof blob, idx, payload, sizeof payload);
        rc = ring_casm_feed(&a, payload, (size_t)plen, 0);
    }
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_MEMORY(blob, a.buf, sizeof blob);
}

static void test_single_chunk_blob_le112_works(void) {
    uint8_t blob[50];
    fill_pattern(blob, sizeof blob);
    TEST_ASSERT_EQUAL_INT(1, ring_cfg_chunk_count(sizeof blob));

    uint8_t payload[9 + RING_CFG_DATA_MAX];
    int plen = ring_cfg_chunk_build(2 /* HW */, 1, blob, sizeof blob, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT((int)(9 + sizeof blob), plen);
    TEST_ASSERT_EQUAL_UINT8(1, payload[6]);   /* count == 1 */

    ring_casm_t a;
    ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(1, ring_casm_feed(&a, payload, (size_t)plen, 5));
    TEST_ASSERT_EQUAL_MEMORY(blob, a.buf, sizeof blob);
}

/* ========== restart on gen/kind mismatch ========== */

static void test_gen_bump_mid_transfer_restarts(void) {
    uint8_t blob1[700]; fill_pattern(blob1, sizeof blob1);
    uint8_t blob2[700]; for (size_t i = 0; i < sizeof blob2; i++) blob2[i] = (uint8_t)(0xFF - (i & 0xFF));

    ring_casm_t a;
    ring_casm_init(&a);

    /* gen 1: feed two of seven chunks, transfer left incomplete */
    for (uint8_t idx = 0; idx < 2; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(1, 1, blob1, sizeof blob1, idx, payload, sizeof payload);
        TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, payload, (size_t)plen, 100));
    }

    /* gen 2, same kind: first chunk restarts the transfer */
    int rc = 0;
    for (uint8_t idx = 0; idx < 7; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(1, 2, blob2, sizeof blob2, idx, payload, sizeof payload);
        rc = ring_casm_feed(&a, payload, (size_t)plen, 200);
        if (idx < 6) TEST_ASSERT_EQUAL_INT(0, rc);
    }
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT32(2, a.gen);
    TEST_ASSERT_EQUAL_MEMORY(blob2, a.buf, sizeof blob2);   /* gen-1 remnants fully discarded */
}

static void test_kind_flip_mid_transfer_restarts(void) {
    uint8_t blob1[300]; fill_pattern(blob1, sizeof blob1);
    uint8_t blob2[224]; for (size_t i = 0; i < sizeof blob2; i++) blob2[i] = (uint8_t)(i + 1);

    ring_casm_t a;
    ring_casm_init(&a);

    /* kind CFG, gen 5: feed one of three chunks */
    uint8_t p0[9 + RING_CFG_DATA_MAX];
    int p0len = ring_cfg_chunk_build(1, 5, blob1, sizeof blob1, 0, p0, sizeof p0);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, p0, (size_t)p0len, 10));
    TEST_ASSERT_EQUAL_UINT8(1, a.kind);

    /* kind HW, same gen: still restarts (kind differs) */
    int rc = 0;
    for (uint8_t idx = 0; idx < 2; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(2, 5, blob2, sizeof blob2, idx, payload, sizeof payload);
        rc = ring_casm_feed(&a, payload, (size_t)plen, 20);
    }
    TEST_ASSERT_EQUAL_INT(1, rc);
    TEST_ASSERT_EQUAL_UINT8(2, a.kind);
    TEST_ASSERT_EQUAL_MEMORY(blob2, a.buf, sizeof blob2);
}

/* ========== idle expiry: pure query, caller re-inits ========== */

static void test_idle_expiry_boundary(void) {
    uint8_t blob[700]; fill_pattern(blob, sizeof blob);   /* 7 chunks: feed 3, leaving it incomplete */
    ring_casm_t a;
    ring_casm_init(&a);

    for (uint8_t idx = 0; idx < 3; idx++) {
        uint8_t payload[9 + RING_CFG_DATA_MAX];
        int plen = ring_cfg_chunk_build(1, 1, blob, sizeof blob, idx, payload, sizeof payload);
        TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, payload, (size_t)plen, 1000));
    }

    TEST_ASSERT_EQUAL_INT(0, ring_casm_idle_expired(&a, 1000));          /* just fed */
    TEST_ASSERT_EQUAL_INT(0, ring_casm_idle_expired(&a, 1000 + 2000));   /* exactly at threshold: not expired */
    TEST_ASSERT_EQUAL_INT(1, ring_casm_idle_expired(&a, 1000 + 2001));   /* past threshold */

    /* pure query: no state change, feeding still works after a query that reported expiry */
    uint8_t payload[9 + RING_CFG_DATA_MAX];
    int plen = ring_cfg_chunk_build(1, 1, blob, sizeof blob, 2, payload, sizeof payload);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, payload, (size_t)plen, 1000 + 2001));

    /* caller re-inits on expiry */
    ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_idle_expired(&a, 999999));
}

static void test_idle_never_expired_when_inactive(void) {
    ring_casm_t a;
    ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_idle_expired(&a, 999999));
}

/* ========== rejects ========== */

static void test_reject_total_769(void) {
    uint8_t data[112]; memset(data, 0xAA, sizeof data);
    uint8_t payload[9 + 112];
    size_t n = build_raw(payload, 1, 1, 0, 7, 769, data, sizeof data);
    ring_casm_t a; ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, payload, n, 0));
}

static void test_reject_count_8(void) {
    uint8_t data[112]; memset(data, 0xAA, sizeof data);
    uint8_t payload[9 + 112];
    size_t n = build_raw(payload, 1, 1, 0, 8, 768, data, sizeof data);
    ring_casm_t a; ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, payload, n, 0));
}

static void test_reject_idx_ge_count(void) {
    uint8_t data[28]; memset(data, 0xAA, sizeof data);
    uint8_t payload[9 + 28];
    size_t n = build_raw(payload, 1, 1, 3, 3, 700, data, sizeof data);   /* idx == count */
    ring_casm_t a; ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, payload, n, 0));
}

static void test_reject_payload_shorter_than_9(void) {
    uint8_t payload[8]; memset(payload, 0, sizeof payload);
    ring_casm_t a; ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, payload, sizeof payload, 0));
}

static void test_reject_interior_chunk_short_data(void) {
    uint8_t data[100]; memset(data, 0xAA, sizeof data);   /* interior chunk must be exactly 112 */
    uint8_t payload[9 + 100];
    size_t n = build_raw(payload, 1, 1, 0, 7, 700, data, sizeof data);
    ring_casm_t a; ring_casm_init(&a);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, payload, n, 0));
}

static void test_reject_mismatched_count_or_total_same_gen_kind(void) {
    uint8_t blob[700]; fill_pattern(blob, sizeof blob);
    ring_casm_t a; ring_casm_init(&a);

    uint8_t p0[9 + RING_CFG_DATA_MAX];
    int p0len = ring_cfg_chunk_build(1, 1, blob, sizeof blob, 0, p0, sizeof p0);
    TEST_ASSERT_EQUAL_INT(0, ring_casm_feed(&a, p0, (size_t)p0len, 0));

    /* same kind+gen, but total disagrees with the transfer already in progress -> reject, not restart */
    uint8_t data[112]; memset(data, 0xBB, sizeof data);
    uint8_t bad[9 + 112];
    size_t n = build_raw(bad, 1, 1, 1, 7, 700 - 1, data, sizeof data);
    TEST_ASSERT_EQUAL_INT(-1, ring_casm_feed(&a, bad, n, 0));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_chunk_count_ceil_division);
    RUN_TEST(test_chunk_build_700_byte_blob_headers_and_data);
    RUN_TEST(test_chunk_build_idx_out_of_range_rejected);
    RUN_TEST(test_feed_in_order_completes_byte_equal);
    RUN_TEST(test_feed_out_of_order_idx3_first_completes);
    RUN_TEST(test_feed_duplicate_chunk_no_corruption);
    RUN_TEST(test_single_chunk_blob_le112_works);
    RUN_TEST(test_gen_bump_mid_transfer_restarts);
    RUN_TEST(test_kind_flip_mid_transfer_restarts);
    RUN_TEST(test_idle_expiry_boundary);
    RUN_TEST(test_idle_never_expired_when_inactive);
    RUN_TEST(test_reject_total_769);
    RUN_TEST(test_reject_count_8);
    RUN_TEST(test_reject_idx_ge_count);
    RUN_TEST(test_reject_payload_shorter_than_9);
    RUN_TEST(test_reject_interior_chunk_short_data);
    RUN_TEST(test_reject_mismatched_count_or_total_same_gen_kind);
    return UNITY_END();
}
