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

static void test_cobs_254_boundary(void) {
    uint8_t in[301], enc[304], dec[301];
    int test_cases[] = {253, 254, 255, 300};

    for (size_t tc = 0; tc < 4; tc++) {
        int n = test_cases[tc];

        /* Test 1: n non-zero bytes (no trailing zero) */
        for (int i = 0; i < n; i++) {
            in[i] = (uint8_t)((i % 255) + 1);  /* All non-zero: 1..255 cycling */
        }
        size_t enc_len = ring_cobs_encode(in, n, enc);
        TEST_ASSERT_TRUE(enc_len >= n && enc_len <= n + n / 254 + 1);
        for (size_t i = 0; i < enc_len; i++) {
            TEST_ASSERT_NOT_EQUAL(0, enc[i]);  /* No zeros in encoded output */
        }
        int dec_len = ring_cobs_decode(enc, enc_len, dec);
        TEST_ASSERT_EQUAL_INT(n, dec_len);
        if (n > 0) TEST_ASSERT_EQUAL_MEMORY(in, dec, n);

        /* Test 2: n non-zero bytes + one trailing 0x00 */
        for (int i = 0; i < n; i++) {
            in[i] = (uint8_t)((i % 255) + 1);
        }
        in[n] = 0;
        enc_len = ring_cobs_encode(in, n + 1, enc);
        TEST_ASSERT_TRUE(enc_len >= n + 1 && enc_len <= n + 1 + (n + 1) / 254 + 1);
        for (size_t i = 0; i < enc_len; i++) {
            TEST_ASSERT_NOT_EQUAL(0, enc[i]);
        }
        dec_len = ring_cobs_decode(enc, enc_len, dec);
        TEST_ASSERT_EQUAL_INT(n + 1, dec_len);
        TEST_ASSERT_EQUAL_MEMORY(in, dec, n + 1);

        /* Special: for 254 non-zero + 1 zero, verify exact encoded format: [0xFF, 254B, 0x01, 0x01] */
        if (n == 254) {
            TEST_ASSERT_EQUAL_size_t(257, enc_len);
            TEST_ASSERT_EQUAL_HEX8(0xFF, enc[0]);
            TEST_ASSERT_EQUAL_HEX8(0x01, enc[255]);
            TEST_ASSERT_EQUAL_HEX8(0x01, enc[256]);
        }
    }
}

/* CMD "GET ID", master(0) -> zone 2, seq 1042, ttl 16, ACK_REQ */
static const uint8_t WIRE_CMD_1042[20] = {
    0x00, 0x02, 0xA1, 0x10, 0x02, 0x10, 0x01, 0x10, 0x06, 0x12,
    0x04, 0x47, 0x45, 0x54, 0x20, 0x49, 0x44, 0x5A, 0xE7, 0x00 };
    /* frame = A1 00 02 10 01 10 06 12 04 | "GET ID" | crc 0xE75A (LE 5A E7) */

static void test_encode_worked_example(void) {
    ring_hdr_t h = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ,
                     .ttl = 16, .len = 6, .seq = 1042 };
    uint8_t wire[RING_WIRE_MAX];
    int n = ring_frame_encode(&h, (const uint8_t *)"GET ID", wire, sizeof wire);
    TEST_ASSERT_EQUAL_INT(20, n);
    TEST_ASSERT_EQUAL_MEMORY(WIRE_CMD_1042, wire, 20);
}
static void test_decode_worked_example(void) {
    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t h; uint8_t pay[RING_MAX_PAYLOAD];
    int got = 0;
    for (int i = 0; i < 20; i++) got = ring_dec_feed(&d, WIRE_CMD_1042[i], &h, pay);
    TEST_ASSERT_EQUAL_INT(1, got);
    TEST_ASSERT_EQUAL_UINT8(0, h.src);   TEST_ASSERT_EQUAL_UINT8(2, h.dst);
    TEST_ASSERT_EQUAL_UINT8(RING_T_CMD, h.type);
    TEST_ASSERT_EQUAL_UINT16(1042, h.seq);
    TEST_ASSERT_EQUAL_UINT8(6, h.len);
    TEST_ASSERT_EQUAL_MEMORY("GET ID", pay, 6);
}
static void test_forward_recrc(void) {
    /* same frame after one hop: ttl 15 -> CRC becomes 0x257D */
    ring_hdr_t h = { .src = 0, .dst = 2, .type = RING_T_CMD, .flags = RING_F_ACK_REQ,
                     .ttl = 15, .len = 6, .seq = 1042 };
    uint8_t wire[RING_WIRE_MAX];
    int n = ring_frame_encode(&h, (const uint8_t *)"GET ID", wire, sizeof wire);
    TEST_ASSERT_TRUE(n > 4);
    TEST_ASSERT_EQUAL_HEX8(0x7D, wire[n - 3]);   /* crc LE low byte before trailing 0x00 */
    TEST_ASSERT_EQUAL_HEX8(0x25, wire[n - 2]);
}

static void test_decode_resync_after_garbage(void) {
    /* 50 non-zero garbage bytes (deterministic LCG so the run is reproducible), then the
       full worked wire -> exactly one dropped frame (the garbage) and one good frame. */
    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t h; uint8_t pay[RING_MAX_PAYLOAD];
    int count_minus1 = 0, count_1 = 0;
    uint32_t seed = 12345u;
    for (int i = 0; i < 50; i++) {
        seed = seed * 1103515245u + 12345u;
        uint8_t b = (uint8_t)(seed >> 16);
        if (b == 0) b = 1;
        int r = ring_dec_feed(&d, b, &h, pay);
        if (r == -1) count_minus1++;
        TEST_ASSERT_TRUE(r == 0 || r == -1);
    }
    for (int i = 0; i < 20; i++) {
        int r = ring_dec_feed(&d, WIRE_CMD_1042[i], &h, pay);
        if (r == -1) count_minus1++;
        if (r == 1) count_1++;
    }
    TEST_ASSERT_EQUAL_INT(1, count_minus1);
    TEST_ASSERT_EQUAL_INT(1, count_1);
    TEST_ASSERT_EQUAL_UINT16(1042, h.seq);
}

static void test_decode_bad_crc_dropped(void) {
    uint8_t bad[20];
    memcpy(bad, WIRE_CMD_1042, sizeof bad);
    bad[11] ^= 0xFF;   /* flip the 'G' payload data byte inside the COBS body */

    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t h; uint8_t pay[RING_MAX_PAYLOAD];
    int got = 0;
    for (int i = 0; i < 20; i++) got = ring_dec_feed(&d, bad[i], &h, pay);
    TEST_ASSERT_EQUAL_INT(-1, got);

    /* decoder resynced: the next clean frame still decodes */
    got = 0;
    for (int i = 0; i < 20; i++) got = ring_dec_feed(&d, WIRE_CMD_1042[i], &h, pay);
    TEST_ASSERT_EQUAL_INT(1, got);
    TEST_ASSERT_EQUAL_UINT16(1042, h.seq);
}

static void test_decode_len_mismatch(void) {
    /* worked-example header+payload+crc, but the len field lies (5 instead of 6) while
       6 payload bytes + crc are still present -> decoded length disagrees with hdr.len */
    uint8_t raw[17] = { 0xA1, 0x00, 0x02, 0x10, 0x01, 0x10, 0x05, 0x12, 0x04,
                         'G', 'E', 'T', ' ', 'I', 'D', 0x5A, 0xE7 };
    uint8_t cobs[20];
    size_t clen = ring_cobs_encode(raw, sizeof raw, cobs);
    uint8_t wire[24];
    wire[0] = 0x00;
    memcpy(wire + 1, cobs, clen);
    wire[1 + clen] = 0x00;
    size_t wlen = clen + 2;

    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t h; uint8_t pay[RING_MAX_PAYLOAD];
    int got = 0;
    for (size_t i = 0; i < wlen; i++) got = ring_dec_feed(&d, wire[i], &h, pay);
    TEST_ASSERT_EQUAL_INT(-1, got);
}

static void test_decode_oversize(void) {
    /* buffer overrun: >141 content bytes without a delimiter -> -1, resynced (one drop only) */
    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t h; uint8_t pay[RING_MAX_PAYLOAD];
    int count_minus1 = 0;
    for (int i = 0; i < 142; i++) {
        int r = ring_dec_feed(&d, 0x01, &h, pay);
        if (r == -1) count_minus1++;
        TEST_ASSERT_TRUE(r == 0 || r == -1);
    }
    TEST_ASSERT_EQUAL_INT(1, count_minus1);
}

static void test_encode_rejects_len(void) {
    uint8_t payload[129] = {0};
    ring_hdr_t h = { .src = 0, .dst = 1, .type = RING_T_CMD, .flags = 0, .ttl = 16, .len = 129, .seq = 1 };
    uint8_t wire[RING_WIRE_MAX];
    TEST_ASSERT_EQUAL_INT(-1, ring_frame_encode(&h, payload, wire, sizeof wire));
}

static void test_payload_zero_bytes(void) {
    uint8_t payload[6] = { 0x00, 0x41, 0x00, 0x00, 0x42, 0x00 };
    ring_hdr_t h = { .src = 1, .dst = 2, .type = RING_T_NOTIFY, .flags = 0,
                     .ttl = 16, .len = sizeof payload, .seq = 7 };
    uint8_t wire[RING_WIRE_MAX];
    int n = ring_frame_encode(&h, payload, wire, sizeof wire);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT8(0x00, wire[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, wire[n - 1]);

    ring_dec_t d; ring_dec_init(&d);
    ring_hdr_t rh; uint8_t pay[RING_MAX_PAYLOAD];
    int got = 0;
    for (int i = 0; i < n; i++) got = ring_dec_feed(&d, wire[i], &rh, pay);
    TEST_ASSERT_EQUAL_INT(1, got);
    TEST_ASSERT_EQUAL_UINT8(sizeof payload, rh.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, pay, sizeof payload);
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
    RUN_TEST(test_cobs_254_boundary);
    RUN_TEST(test_encode_worked_example);
    RUN_TEST(test_decode_worked_example);
    RUN_TEST(test_forward_recrc);
    RUN_TEST(test_decode_resync_after_garbage);
    RUN_TEST(test_decode_bad_crc_dropped);
    RUN_TEST(test_decode_len_mismatch);
    RUN_TEST(test_decode_oversize);
    RUN_TEST(test_encode_rejects_len);
    RUN_TEST(test_payload_zero_bytes);
    return UNITY_END();
}
