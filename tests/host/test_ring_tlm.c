#include <string.h>
#include <stdint.h>
#include "unity.h"
#include "ring_proto.h"

/* ============================================================================
   HEARTBEAT TESTS
   ============================================================================ */

static void test_hb_pack_n0_returns_62(void) {
    hg_hb_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.mac, "\x11\x22\x33\x44\x55\x66", 6);
    h.fw_maj = 1; h.fw_min = 2; h.fw_patch = 3;
    h.n_shelves = 0;
    h.uptime_s = 0x12345678;
    h.unix_time = 0xABCDEF01;
    h.cfg_gen = 0x11223344;
    h.cfg_crc = 0x55667788;
    h.hw_crc = 0x99AABBCC;
    h.cfg_src = 1;
    h.mode = 2;
    h.reset_reason = 3;
    h.time_quality = 1;
    h.active_faults = 0x0102030405060708ULL;
    h.link_flags = 0x07;
    h.override_mask = 0x0F;
    h.rx_crc_err = 0x0001;
    h.rx_uart_err = 0x0002;
    h.rx_drop = 0x0003;
    h.fwd_count = 0x0004;
    h.min_free_heap_kb = 0x0005;

    uint8_t out[140];
    int result = hg_hb_pack(&h, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(62, result);
}

static void test_hb_pack_n4_returns_118(void) {
    hg_hb_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.mac, "\x11\x22\x33\x44\x55\x66", 6);
    h.fw_maj = 1; h.fw_min = 2; h.fw_patch = 3;
    h.n_shelves = 4;
    h.uptime_s = 0x12345678;
    h.unix_time = 0xABCDEF01;
    h.cfg_gen = 0x11223344;
    h.cfg_crc = 0x55667788;
    h.hw_crc = 0x99AABBCC;
    h.cfg_src = 1;
    h.mode = 2;
    h.reset_reason = 3;
    h.time_quality = 1;
    h.active_faults = 0x0102030405060708ULL;
    h.link_flags = 0x07;
    h.override_mask = 0x0F;
    h.rx_crc_err = 0x0001;
    h.rx_uart_err = 0x0002;
    h.rx_drop = 0x0003;
    h.fwd_count = 0x0004;
    h.min_free_heap_kb = 0x0005;

    for (int i = 0; i < 4; i++) {
        h.shelf[i].raw_a = 0x1000 + i;
        h.shelf[i].raw_b = 0x2000 + i;
        h.shelf[i].pct_a = 10 + i;
        h.shelf[i].pct_b = 20 + i;
        h.shelf[i].white = 30 + i;
        h.shelf[i].red = 40 + i;
        h.shelf[i].out_flags = 50 + i;
        h.shelf[i].light_state = 60 + i;
        h.shelf[i].water_state = 70 + i;
        h.shelf[i].pump_today_s = 0x5000 + i;
    }

    uint8_t out[140];
    int result = hg_hb_pack(&h, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(118, result);
}

static void test_hb_pack_field_offsets(void) {
    hg_hb_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.mac, "\x11\x22\x33\x44\x55\x66", 6);
    h.fw_maj = 0x01;
    h.fw_min = 0x02;
    h.fw_patch = 0x03;
    h.n_shelves = 0;
    h.uptime_s = 0x12345678;
    h.unix_time = 0xABCDEF01;
    h.cfg_gen = 0x11223344;
    h.cfg_crc = 0x55667788;
    h.hw_crc = 0x99AABBCC;
    h.cfg_src = 0x05;
    h.mode = 0x06;
    h.reset_reason = 0x07;
    h.time_quality = 0x08;
    h.active_faults = 0x0102030405060708ULL;
    h.link_flags = 0xAA;
    h.override_mask = 0xBB;
    h.rx_crc_err = 0xCCDD;
    h.rx_uart_err = 0xEEFF;
    h.rx_drop = 0x1122;
    h.fwd_count = 0x3344;
    h.min_free_heap_kb = 0x5566;
    /* shelf_faults at [42..49] */
    h.shelf_faults[0] = 0x2211;
    h.shelf_faults[1] = 0x4433;
    h.shelf_faults[2] = 0x6655;
    h.shelf_faults[3] = 0x8877;

    uint8_t out[140];
    int result = hg_hb_pack(&h, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(62, result);

    /* Check offsets per spec §2.4 */
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x44, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0x66, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[8]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[9]);  /* n_shelves = 0 */

    /* uptime_s at [10..13] LE: 0x12345678 */
    TEST_ASSERT_EQUAL_HEX8(0x78, out[10]);
    TEST_ASSERT_EQUAL_HEX8(0x56, out[11]);
    TEST_ASSERT_EQUAL_HEX8(0x34, out[12]);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[13]);

    /* unix_time at [14..17] LE: 0xABCDEF01 */
    TEST_ASSERT_EQUAL_HEX8(0x01, out[14]);
    TEST_ASSERT_EQUAL_HEX8(0xEF, out[15]);
    TEST_ASSERT_EQUAL_HEX8(0xCD, out[16]);
    TEST_ASSERT_EQUAL_HEX8(0xAB, out[17]);

    /* cfg_gen at [18..21] LE: 0x11223344 */
    TEST_ASSERT_EQUAL_HEX8(0x44, out[18]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[19]);
    TEST_ASSERT_EQUAL_HEX8(0x22, out[20]);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[21]);

    /* cfg_crc at [22..25] LE: 0x55667788 */
    TEST_ASSERT_EQUAL_HEX8(0x88, out[22]);
    TEST_ASSERT_EQUAL_HEX8(0x77, out[23]);
    TEST_ASSERT_EQUAL_HEX8(0x66, out[24]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[25]);

    /* hw_crc at [26..29] LE: 0x99AABBCC */
    TEST_ASSERT_EQUAL_HEX8(0xCC, out[26]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out[27]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[28]);
    TEST_ASSERT_EQUAL_HEX8(0x99, out[29]);

    /* cfg_src at [30], mode at [31], reset_reason at [32], time_quality at [33] */
    TEST_ASSERT_EQUAL_HEX8(0x05, out[30]);
    TEST_ASSERT_EQUAL_HEX8(0x06, out[31]);
    TEST_ASSERT_EQUAL_HEX8(0x07, out[32]);
    TEST_ASSERT_EQUAL_HEX8(0x08, out[33]);

    /* active_faults at [34..41] LE: 0x0102030405060708 */
    TEST_ASSERT_EQUAL_HEX8(0x08, out[34]);
    TEST_ASSERT_EQUAL_HEX8(0x07, out[35]);
    TEST_ASSERT_EQUAL_HEX8(0x06, out[36]);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[37]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[38]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[39]);
    TEST_ASSERT_EQUAL_HEX8(0x02, out[40]);
    TEST_ASSERT_EQUAL_HEX8(0x01, out[41]);

    /* shelf_faults[0..3] at [42..49] LE (each u16) with distinct non-zero values */
    TEST_ASSERT_EQUAL_HEX8(0x11, out[42]);  /* shelf_faults[0] LE */
    TEST_ASSERT_EQUAL_HEX8(0x22, out[43]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[44]);  /* shelf_faults[1] LE */
    TEST_ASSERT_EQUAL_HEX8(0x44, out[45]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[46]);  /* shelf_faults[2] LE */
    TEST_ASSERT_EQUAL_HEX8(0x66, out[47]);
    TEST_ASSERT_EQUAL_HEX8(0x77, out[48]);  /* shelf_faults[3] LE */
    TEST_ASSERT_EQUAL_HEX8(0x88, out[49]);

    /* link_flags at [50], override_mask at [51] */
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[50]);
    TEST_ASSERT_EQUAL_HEX8(0xBB, out[51]);

    /* rx_crc_err at [52..53] LE: 0xCCDD */
    TEST_ASSERT_EQUAL_HEX8(0xDD, out[52]);
    TEST_ASSERT_EQUAL_HEX8(0xCC, out[53]);

    /* rx_uart_err at [54..55] LE: 0xEEFF */
    TEST_ASSERT_EQUAL_HEX8(0xFF, out[54]);
    TEST_ASSERT_EQUAL_HEX8(0xEE, out[55]);

    /* rx_drop at [56..57] LE: 0x1122 */
    TEST_ASSERT_EQUAL_HEX8(0x22, out[56]);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[57]);

    /* fwd_count at [58..59] LE: 0x3344 */
    TEST_ASSERT_EQUAL_HEX8(0x44, out[58]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[59]);

    /* min_free_heap_kb at [60..61] LE: 0x5566 */
    TEST_ASSERT_EQUAL_HEX8(0x66, out[60]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[61]);
}

static void test_hb_pack_shelf_block_offsets(void) {
    hg_hb_t h;
    memset(&h, 0, sizeof h);
    h.n_shelves = 1;
    /* Set shelf[0] with distinct values for offset verification */
    h.shelf[0].raw_a = 0x0102;
    h.shelf[0].raw_b = 0x0304;
    h.shelf[0].pct_a = 0x05;
    h.shelf[0].pct_b = 0x06;
    h.shelf[0].white = 0x07;
    h.shelf[0].red = 0x08;
    h.shelf[0].out_flags = 0x09;
    h.shelf[0].light_state = 0x0A;
    h.shelf[0].water_state = 0x0B;
    h.shelf[0].rsvd = 0x0C;
    h.shelf[0].pump_today_s = 0x0D0E;

    uint8_t out[140];
    int result = hg_hb_pack(&h, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(76, result);  /* 62 + 14 */

    /* Verify shelf[0] at offset 62 with every byte checked */
    TEST_ASSERT_EQUAL_HEX8(0x02, out[62]);  /* raw_a LE */
    TEST_ASSERT_EQUAL_HEX8(0x01, out[63]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[64]);  /* raw_b LE */
    TEST_ASSERT_EQUAL_HEX8(0x03, out[65]);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[66]);  /* pct_a */
    TEST_ASSERT_EQUAL_HEX8(0x06, out[67]);  /* pct_b */
    TEST_ASSERT_EQUAL_HEX8(0x07, out[68]);  /* white */
    TEST_ASSERT_EQUAL_HEX8(0x08, out[69]);  /* red */
    TEST_ASSERT_EQUAL_HEX8(0x09, out[70]);  /* out_flags */
    TEST_ASSERT_EQUAL_HEX8(0x0A, out[71]);  /* light_state */
    TEST_ASSERT_EQUAL_HEX8(0x0B, out[72]);  /* water_state */
    TEST_ASSERT_EQUAL_HEX8(0x0C, out[73]);  /* rsvd */
    TEST_ASSERT_EQUAL_HEX8(0x0E, out[74]);  /* pump_today_s LE */
    TEST_ASSERT_EQUAL_HEX8(0x0D, out[75]);
}

static void test_hb_parse_n0_succeeds(void) {
    uint8_t buf[62];
    memset(buf, 0, sizeof buf);
    memcpy(buf + 0, "\x11\x22\x33\x44\x55\x66", 6);
    buf[6] = 0x01;
    buf[7] = 0x02;
    buf[8] = 0x03;
    buf[9] = 0x00;
    buf[10] = 0x78; buf[11] = 0x56; buf[12] = 0x34; buf[13] = 0x12;
    buf[14] = 0x01; buf[15] = 0xEF; buf[16] = 0xCD; buf[17] = 0xAB;
    buf[18] = 0x44; buf[19] = 0x33; buf[20] = 0x22; buf[21] = 0x11;
    buf[22] = 0x88; buf[23] = 0x77; buf[24] = 0x66; buf[25] = 0x55;
    buf[26] = 0xCC; buf[27] = 0xBB; buf[28] = 0xAA; buf[29] = 0x99;
    buf[30] = 0x05;
    buf[31] = 0x06;
    buf[32] = 0x07;
    buf[33] = 0x08;
    buf[34] = 0x08; buf[35] = 0x07; buf[36] = 0x06; buf[37] = 0x05;
    buf[38] = 0x04; buf[39] = 0x03; buf[40] = 0x02; buf[41] = 0x01;
    buf[50] = 0xAA;
    buf[51] = 0xBB;
    buf[52] = 0xDD; buf[53] = 0xCC;
    buf[54] = 0xFF; buf[55] = 0xEE;
    buf[56] = 0x22; buf[57] = 0x11;
    buf[58] = 0x44; buf[59] = 0x33;
    buf[60] = 0x66; buf[61] = 0x55;

    hg_hb_t out;
    int result = hg_hb_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT8(0, out.n_shelves);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, out.uptime_s);
    TEST_ASSERT_EQUAL_UINT32(0xABCDEF01, out.unix_time);
    TEST_ASSERT_EQUAL_UINT32(0x11223344, out.cfg_gen);
}

static void test_hb_parse_n_gt_4_rejected(void) {
    uint8_t buf[62];
    memset(buf, 0, sizeof buf);
    buf[9] = 5;

    hg_hb_t out;
    int result = hg_hb_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_hb_parse_len_mismatch_61_rejected(void) {
    uint8_t buf[61];
    memset(buf, 0, sizeof buf);
    buf[9] = 0;

    hg_hb_t out;
    int result = hg_hb_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_hb_parse_len_mismatch_61_for_n4_rejected(void) {
    uint8_t buf[62 + 14*4 - 1];
    memset(buf, 0, sizeof buf);
    buf[9] = 4;

    hg_hb_t out;
    int result = hg_hb_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_hb_roundtrip_n0(void) {
    hg_hb_t orig;
    memset(&orig, 0, sizeof orig);
    memcpy(orig.mac, "\xAA\xBB\xCC\xDD\xEE\xFF", 6);
    orig.fw_maj = 1; orig.fw_min = 2; orig.fw_patch = 3;
    orig.n_shelves = 0;
    orig.uptime_s = 12345;
    orig.unix_time = 67890;

    uint8_t buf[140];
    int plen = hg_hb_pack(&orig, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(62, plen);

    hg_hb_t parsed;
    int result = hg_hb_parse(buf, plen, &parsed);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_MEMORY(orig.mac, parsed.mac, 6);
    TEST_ASSERT_EQUAL_UINT8(1, parsed.fw_maj);
    TEST_ASSERT_EQUAL_UINT8(2, parsed.fw_min);
    TEST_ASSERT_EQUAL_UINT8(3, parsed.fw_patch);
    TEST_ASSERT_EQUAL_UINT8(0, parsed.n_shelves);
    TEST_ASSERT_EQUAL_UINT32(12345, parsed.uptime_s);
    TEST_ASSERT_EQUAL_UINT32(67890, parsed.unix_time);
}

static void test_hb_roundtrip_n4(void) {
    hg_hb_t orig;
    memset(&orig, 0, sizeof orig);
    memcpy(orig.mac, "\x11\x22\x33\x44\x55\x66", 6);
    orig.fw_maj = 1; orig.fw_min = 2; orig.fw_patch = 3;
    orig.n_shelves = 4;
    orig.uptime_s = 0x12345678;
    orig.unix_time = 0x9ABCDEF0;
    orig.active_faults = 0x0102030405060708ULL;

    for (int i = 0; i < 4; i++) {
        orig.shelf[i].raw_a = 1000 + i;
        orig.shelf[i].raw_b = 2000 + i;
        orig.shelf[i].pct_a = 10 + i;
        orig.shelf[i].pct_b = 20 + i;
        orig.shelf[i].white = 100 + i;
        orig.shelf[i].red = 110 + i;
        orig.shelf[i].out_flags = 120 + i;
        orig.shelf[i].light_state = 1 + i;
        orig.shelf[i].water_state = 2 + i;
        orig.shelf[i].pump_today_s = 3600 + i;
    }

    uint8_t buf[140];
    int plen = hg_hb_pack(&orig, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(118, plen);

    hg_hb_t parsed;
    int result = hg_hb_parse(buf, plen, &parsed);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT8(4, parsed.n_shelves);
    TEST_ASSERT_EQUAL_MEMORY(orig.mac, parsed.mac, 6);
}

/* ============================================================================
   TIME_SYNC TESTS
   ============================================================================ */

static void test_ts_pack_fixed_13(void) {
    hg_ts_t t;
    memset(&t, 0, sizeof t);
    t.utc = 0x12345678;
    t.utc_offset_s = 0x9ABCDEF0;
    t.flags = 0x03;
    t.ring_size = 4;
    t.online_mask = 0x00AA;
    t.inhibit_mask = 0x05;

    uint8_t out[13];
    int result = hg_ts_pack(&t, out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX8(0x78, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x56, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x34, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0xF0, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0xDE, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0xBC, out[6]);
    TEST_ASSERT_EQUAL_HEX8(0x9A, out[7]);
    TEST_ASSERT_EQUAL_HEX8(0x03, out[8]);
    TEST_ASSERT_EQUAL_HEX8(0x04, out[9]);
    TEST_ASSERT_EQUAL_HEX8(0xAA, out[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[11]);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[12]);
}

static void test_ts_parse_succeeds(void) {
    uint8_t buf[13];
    buf[0] = 0x78; buf[1] = 0x56; buf[2] = 0x34; buf[3] = 0x12;
    buf[4] = 0xF0; buf[5] = 0xDE; buf[6] = 0xBC; buf[7] = 0x9A;
    buf[8] = 0x03;
    buf[9] = 0x04;
    buf[10] = 0xAA; buf[11] = 0x00;
    buf[12] = 0x05;

    hg_ts_t out;
    int result = hg_ts_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT32(0x12345678, out.utc);
    TEST_ASSERT_EQUAL_INT32(0x9ABCDEF0, out.utc_offset_s);
    TEST_ASSERT_EQUAL_UINT8(0x03, out.flags);
    TEST_ASSERT_EQUAL_UINT8(0x04, out.ring_size);
    TEST_ASSERT_EQUAL_UINT16(0x00AA, out.online_mask);
    TEST_ASSERT_EQUAL_UINT8(0x05, out.inhibit_mask);
}

static void test_ts_parse_reject_short(void) {
    uint8_t buf[12];
    hg_ts_t out;
    int result = hg_ts_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_ts_parse_reject_long(void) {
    uint8_t buf[14];
    hg_ts_t out;
    int result = hg_ts_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);  /* Reject if not exact 13 */
}

static void test_ts_roundtrip(void) {
    hg_ts_t orig;
    memset(&orig, 0, sizeof orig);
    orig.utc = 0x11223344;
    orig.utc_offset_s = 0x55667788;
    orig.flags = 0x02;
    orig.ring_size = 3;
    orig.online_mask = 0x0155;
    orig.inhibit_mask = 0x01;

    uint8_t buf[13];
    int pres = hg_ts_pack(&orig, buf);
    TEST_ASSERT_EQUAL_INT(0, pres);

    hg_ts_t parsed;
    int pres2 = hg_ts_parse(buf, sizeof buf, &parsed);
    TEST_ASSERT_EQUAL_INT(0, pres2);
    TEST_ASSERT_EQUAL_UINT32(orig.utc, parsed.utc);
    TEST_ASSERT_EQUAL_INT32(orig.utc_offset_s, parsed.utc_offset_s);
    TEST_ASSERT_EQUAL_UINT8(orig.flags, parsed.flags);
    TEST_ASSERT_EQUAL_UINT8(orig.ring_size, parsed.ring_size);
}

/* ============================================================================
   ASSIGN_ID TESTS
   ============================================================================ */

static void test_assign_pack_fixed_7(void) {
    hg_assign_t a;
    memcpy(a.mac, "\x11\x22\x33\x44\x55\x66", 6);
    a.zone_id = 0x05;

    uint8_t out[7];
    int result = hg_assign_pack(&a, out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX8(0x11, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x33, out[2]);
    TEST_ASSERT_EQUAL_HEX8(0x44, out[3]);
    TEST_ASSERT_EQUAL_HEX8(0x55, out[4]);
    TEST_ASSERT_EQUAL_HEX8(0x66, out[5]);
    TEST_ASSERT_EQUAL_HEX8(0x05, out[6]);
}

static void test_assign_parse_succeeds(void) {
    uint8_t buf[7] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x05 };

    hg_assign_t out;
    int result = hg_assign_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_MEMORY("\x11\x22\x33\x44\x55\x66", out.mac, 6);
    TEST_ASSERT_EQUAL_UINT8(0x05, out.zone_id);
}

static void test_assign_parse_reject_short(void) {
    uint8_t buf[6];
    hg_assign_t out;
    int result = hg_assign_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_assign_parse_reject_long(void) {
    uint8_t buf[8];
    hg_assign_t out;
    int result = hg_assign_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);  /* Reject if not exact 7 */
}

static void test_assign_roundtrip(void) {
    hg_assign_t orig;
    memcpy(orig.mac, "\xAA\xBB\xCC\xDD\xEE\xFF", 6);
    orig.zone_id = 0x02;

    uint8_t buf[7];
    int pres = hg_assign_pack(&orig, buf);
    TEST_ASSERT_EQUAL_INT(0, pres);

    hg_assign_t parsed;
    int pres2 = hg_assign_parse(buf, sizeof buf, &parsed);
    TEST_ASSERT_EQUAL_INT(0, pres2);
    TEST_ASSERT_EQUAL_MEMORY(orig.mac, parsed.mac, 6);
    TEST_ASSERT_EQUAL_UINT8(orig.zone_id, parsed.zone_id);
}

/* ============================================================================
   FW_UPDATE TESTS
   ============================================================================ */

static void test_fwu_pack_basic(void) {
    hg_fwu_t f;
    memset(&f, 0, sizeof f);
    f.reboot_delay_ms = 0x1234;
    strcpy(f.ssid, "TestSSID");
    strcpy(f.pass, "TestPass123");

    uint8_t out[99];
    int result = hg_fwu_pack(&f, out, sizeof out);
    TEST_ASSERT_TRUE(result > 0);
    TEST_ASSERT_TRUE(result <= 99);

    /* Check layout: reboot_delay LE at [0..1] */
    TEST_ASSERT_EQUAL_HEX8(0x34, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[1]);
    /* ssid_len at [2] */
    TEST_ASSERT_EQUAL_HEX8(8, out[2]);
    /* ssid at [3..] */
    TEST_ASSERT_EQUAL_MEMORY("TestSSID", out + 3, 8);
    /* pass_len at [3+8] */
    TEST_ASSERT_EQUAL_HEX8(11, out[3 + 8]);
    /* pass at [3+8+1..] */
    TEST_ASSERT_EQUAL_MEMORY("TestPass123", out + 3 + 8 + 1, 11);
}

static void test_fwu_pack_rejects_ssid_33_or_more(void) {
    hg_fwu_t f;
    memset(&f, 0, sizeof f);
    f.reboot_delay_ms = 0x0000;
    /* Fill SSID with non-NUL bytes throughout entire field */
    memset(f.ssid, 'S', sizeof f.ssid);
    strcpy(f.pass, "short");

    uint8_t out[99];
    int result = hg_fwu_pack(&f, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_fwu_pack_rejects_pass_65_or_more(void) {
    hg_fwu_t f;
    memset(&f, 0, sizeof f);
    f.reboot_delay_ms = 0x0000;
    strcpy(f.ssid, "test");
    /* Create a password that's at least 65 chars (fill rest to test boundary) */
    memset(f.pass, 'x', 64);
    f.pass[64] = '\0';  /* Exactly 64 chars, valid */

    uint8_t out[99];
    int result = hg_fwu_pack(&f, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(2 + 1 + 4 + 1 + 64, result);  /* Should succeed at 64 */

    /* Now test with 65 non-NUL bytes */
    hg_fwu_t f2;
    memset(&f2, 0, sizeof f2);
    f2.reboot_delay_ms = 0x0000;
    strcpy(f2.ssid, "test");
    /* Fill pass entirely with non-NUL bytes */
    memset(f2.pass, 'x', sizeof f2.pass);

    result = hg_fwu_pack(&f2, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(-1, result);  /* Should reject at 65+ */
}

static void test_fwu_pack_sum_cap_rejects_100(void) {
    hg_fwu_t f;
    memset(&f, 0, sizeof f);
    f.reboot_delay_ms = 0x0000;
    /* SSID 32 chars (valid individually) */
    memset(f.ssid, 'A', 32);
    f.ssid[32] = '\0';
    /* Pass 64 chars (valid individually) */
    memset(f.pass, 'B', 64);
    f.pass[64] = '\0';
    /* Total: 2 + 1 + 32 + 1 + 64 = 100 > 99 -> reject */

    uint8_t out[99];
    int result = hg_fwu_pack(&f, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

static void test_fwu_parse_succeeds(void) {
    uint8_t buf[99];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x34;
    buf[1] = 0x12;
    buf[2] = 8;
    memcpy(buf + 3, "TestSSID", 8);
    buf[3 + 8] = 11;
    memcpy(buf + 3 + 8 + 1, "TestPass123", 11);

    hg_fwu_t out;
    int result = hg_fwu_parse(buf, 3 + 8 + 1 + 11, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT16(0x1234, out.reboot_delay_ms);
    TEST_ASSERT_EQUAL_STRING("TestSSID", out.ssid);
    TEST_ASSERT_EQUAL_STRING("TestPass123", out.pass);
}

static void test_fwu_parse_nul_terminates(void) {
    uint8_t buf[99];
    memset(buf, 0, sizeof buf);
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 4;
    memcpy(buf + 3, "test", 4);
    buf[7] = 4;
    memcpy(buf + 8, "pass", 4);

    hg_fwu_t out;
    memset(&out, 0xFF, sizeof out);
    int result = hg_fwu_parse(buf, 3 + 4 + 1 + 4, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_STRING("test", out.ssid);
    TEST_ASSERT_EQUAL_STRING("pass", out.pass);
}

static void test_fwu_roundtrip(void) {
    hg_fwu_t orig;
    memset(&orig, 0, sizeof orig);
    orig.reboot_delay_ms = 5000;
    strcpy(orig.ssid, "MyNetwork");
    strcpy(orig.pass, "SecurePass");

    uint8_t buf[99];
    int plen = hg_fwu_pack(&orig, buf, sizeof buf);
    TEST_ASSERT_TRUE(plen > 0);

    hg_fwu_t parsed;
    int result = hg_fwu_parse(buf, plen, &parsed);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT16(orig.reboot_delay_ms, parsed.reboot_delay_ms);
    TEST_ASSERT_EQUAL_STRING(orig.ssid, parsed.ssid);
    TEST_ASSERT_EQUAL_STRING(orig.pass, parsed.pass);
}

/* ============================================================================
   ACK TESTS
   ============================================================================ */

static void test_ack_pack_basic(void) {
    hg_ack_t a;
    memset(&a, 0, sizeof a);
    a.acked_seq = 0x1234;
    a.status = 0x00;
    strcpy(a.detail, "OK");
    a.detail_len = 2;

    uint8_t out[129];
    int result = hg_ack_pack(&a, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(5, result);  /* 3 + 2 detail bytes */

    /* Check layout */
    TEST_ASSERT_EQUAL_HEX8(0x34, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0x12, out[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, out[2]);
    TEST_ASSERT_EQUAL_MEMORY("OK", out + 3, 2);
}

static void test_ack_pack_max_detail_125(void) {
    hg_ack_t a;
    memset(&a, 0, sizeof a);
    a.acked_seq = 0x0000;
    a.status = 0xFF;
    memset(a.detail, 'X', 125);
    a.detail_len = 125;

    uint8_t out[129];
    int result = hg_ack_pack(&a, out, sizeof out);
    TEST_ASSERT_EQUAL_INT(128, result);  /* 3 + 125 */
}

static void test_ack_parse_succeeds(void) {
    uint8_t buf[5] = { 0x34, 0x12, 0x00, 'O', 'K' };

    hg_ack_t out;
    int result = hg_ack_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT16(0x1234, out.acked_seq);
    TEST_ASSERT_EQUAL_UINT8(0x00, out.status);
    TEST_ASSERT_EQUAL_UINT8(2, out.detail_len);
}

static void test_ack_parse_nul_terminates_detail(void) {
    uint8_t buf[8] = { 0x00, 0x00, 0x00, 'T', 'E', 'S', 'T', 0xFF };

    hg_ack_t out;
    memset(&out, 0, sizeof out);
    int result = hg_ack_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT8(5, out.detail_len);
    /* Verify detail is NUL-terminated */
    TEST_ASSERT_EQUAL_HEX8(0x00, out.detail[5]);
}

static void test_ack_parse_reject_oversized(void) {
    uint8_t buf[129];  /* Total length 129: 3 hdr + 126 detail = too much */
    memset(buf, 0, sizeof buf);
    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;

    hg_ack_t out;
    int result = hg_ack_parse(buf, sizeof buf, &out);
    TEST_ASSERT_EQUAL_INT(-1, result);  /* Reject if total > 128 */
}

static void test_ack_roundtrip(void) {
    hg_ack_t orig;
    memset(&orig, 0, sizeof orig);
    orig.acked_seq = 0xABCD;
    orig.status = 0x42;
    strcpy(orig.detail, "Error");
    orig.detail_len = 5;

    uint8_t buf[129];
    int plen = hg_ack_pack(&orig, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(8, plen);

    hg_ack_t parsed;
    int result = hg_ack_parse(buf, plen, &parsed);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_UINT16(orig.acked_seq, parsed.acked_seq);
    TEST_ASSERT_EQUAL_UINT8(orig.status, parsed.status);
    TEST_ASSERT_EQUAL_UINT8(orig.detail_len, parsed.detail_len);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_hb_pack_n0_returns_62);
    RUN_TEST(test_hb_pack_n4_returns_118);
    RUN_TEST(test_hb_pack_field_offsets);
    RUN_TEST(test_hb_pack_shelf_block_offsets);
    RUN_TEST(test_hb_parse_n0_succeeds);
    RUN_TEST(test_hb_parse_n_gt_4_rejected);
    RUN_TEST(test_hb_parse_len_mismatch_61_rejected);
    RUN_TEST(test_hb_parse_len_mismatch_61_for_n4_rejected);
    RUN_TEST(test_hb_roundtrip_n0);
    RUN_TEST(test_hb_roundtrip_n4);
    RUN_TEST(test_ts_pack_fixed_13);
    RUN_TEST(test_ts_parse_succeeds);
    RUN_TEST(test_ts_parse_reject_short);
    RUN_TEST(test_ts_parse_reject_long);
    RUN_TEST(test_ts_roundtrip);
    RUN_TEST(test_assign_pack_fixed_7);
    RUN_TEST(test_assign_parse_succeeds);
    RUN_TEST(test_assign_parse_reject_short);
    RUN_TEST(test_assign_parse_reject_long);
    RUN_TEST(test_assign_roundtrip);
    RUN_TEST(test_fwu_pack_basic);
    RUN_TEST(test_fwu_pack_rejects_ssid_33_or_more);
    RUN_TEST(test_fwu_pack_rejects_pass_65_or_more);
    RUN_TEST(test_fwu_pack_sum_cap_rejects_100);
    RUN_TEST(test_fwu_parse_succeeds);
    RUN_TEST(test_fwu_parse_nul_terminates);
    RUN_TEST(test_fwu_roundtrip);
    RUN_TEST(test_ack_pack_basic);
    RUN_TEST(test_ack_pack_max_detail_125);
    RUN_TEST(test_ack_parse_succeeds);
    RUN_TEST(test_ack_parse_nul_terminates_detail);
    RUN_TEST(test_ack_parse_reject_oversized);
    RUN_TEST(test_ack_roundtrip);
    return UNITY_END();
}
