#include <string.h>
#include "unity.h"
#include "hg_blob.h"
#include "pnl_prefs.h"

/* Panel-local preferences live in NVS "panel"/"prefs" as an hg_blob envelope with explicit byte offsets (the project's
   rule for persisted data). Anything unreadable falls back to the defaults, and every stored value is clamped on the
   way in so a bad blob can never set the backlight to 0 %. */

void setUp(void) {}
void tearDown(void) {}

static void assert_defaults(const pnl_prefs_t *p) {
    TEST_ASSERT_EQUAL_UINT8(1, p->dim.mode);
    TEST_ASSERT_EQUAL_UINT8(80, p->dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(10, p->dim.night_pct);
    TEST_ASSERT_EQUAL_UINT16(22 * 60, p->dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(6 * 60, p->dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(60, p->dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(300, p->wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(0, p->face);
    TEST_ASSERT_EQUAL_UINT8(0, p->orient);
}

static pnl_prefs_t sample(void) {
    pnl_prefs_t a;
    pnl_prefs_defaults(&a);
    a.dim.mode = 2; a.dim.day_pct = 65; a.dim.night_pct = 7;
    a.dim.fixed_start_min = 21 * 60 + 30; a.dim.fixed_end_min = 5 * 60 + 15; a.dim.idle_s = 30;
    a.wipe_idle_s = 600; a.face = 1; a.orient = 1;
    return a;
}

static void test_defaults(void) {
    pnl_prefs_t p;
    memset(&p, 0xAA, sizeof p);
    pnl_prefs_defaults(&p);
    assert_defaults(&p);
}

static void test_round_trip(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    TEST_ASSERT_EQUAL_size_t(HG_BLOB_HDR_LEN + 16, n);
    memset(&b, 0, sizeof b);
    TEST_ASSERT_EQUAL_INT(0, pnl_prefs_unpack(buf, n, &b));
    TEST_ASSERT_EQUAL_UINT8(2, b.dim.mode);
    TEST_ASSERT_EQUAL_UINT8(65, b.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(7, b.dim.night_pct);
    TEST_ASSERT_EQUAL_UINT16(21 * 60 + 30, b.dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(5 * 60 + 15, b.dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(30, b.dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(600, b.wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(1, b.face);
    TEST_ASSERT_EQUAL_UINT8(1, b.orient);
}

static void test_explicit_byte_offsets(void) {
    pnl_prefs_t a = sample();
    uint8_t buf[64];
    TEST_ASSERT_EQUAL_size_t(32, pnl_prefs_pack(&a, buf, sizeof buf));
    const uint8_t *p = buf + HG_BLOB_HDR_LEN;
    TEST_ASSERT_EQUAL_HEX8(2, p[0]);
    TEST_ASSERT_EQUAL_HEX8(65, p[1]);
    TEST_ASSERT_EQUAL_HEX8(7, p[2]);
    TEST_ASSERT_EQUAL_HEX8(1, p[3]);
    TEST_ASSERT_EQUAL_HEX8(1, p[4]);
    TEST_ASSERT_EQUAL_HEX8(0, p[5]);
    TEST_ASSERT_EQUAL_HEX8((21 * 60 + 30) & 0xFF, p[6]); TEST_ASSERT_EQUAL_HEX8((21 * 60 + 30) >> 8, p[7]);
    TEST_ASSERT_EQUAL_HEX8((5 * 60 + 15) & 0xFF, p[8]);  TEST_ASSERT_EQUAL_HEX8((5 * 60 + 15) >> 8, p[9]);
    TEST_ASSERT_EQUAL_HEX8(30, p[10]);  TEST_ASSERT_EQUAL_HEX8(0, p[11]);
    TEST_ASSERT_EQUAL_HEX8(600 & 0xFF, p[12]); TEST_ASSERT_EQUAL_HEX8(600 >> 8, p[13]);
    TEST_ASSERT_EQUAL_HEX8(0x48, buf[0]);   /* 'HGPN' little-endian magic */
    TEST_ASSERT_EQUAL_HEX8(0x47, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x50, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x4E, buf[3]);
}

static void test_corrupt_crc_gives_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    buf[HG_BLOB_HDR_LEN + 1] ^= 0x01;
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, n, &b));
    assert_defaults(&b);
}

static void test_newer_version_gives_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64], newer[64];
    pnl_prefs_pack(&a, buf, sizeof buf);
    size_t n = hg_blob_wrap(PNL_MAGIC_PREFS, PNL_PREFS_VER + 1, 0, buf + HG_BLOB_HDR_LEN, 16, newer, sizeof newer);
    TEST_ASSERT_EQUAL_size_t(32, n);
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(newer, n, &b));
    assert_defaults(&b);
}

static void test_short_and_wrong_magic_give_defaults(void) {
    pnl_prefs_t a = sample(), b;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, 10, &b));
    assert_defaults(&b);
    buf[0] ^= 0xFF;
    TEST_ASSERT_EQUAL_INT(-1, pnl_prefs_unpack(buf, n, &b));
    assert_defaults(&b);
}

static void test_clamps_on_unpack(void) {
    pnl_prefs_t a, b;
    pnl_prefs_defaults(&a);
    a.dim.mode = 9; a.dim.day_pct = 0; a.dim.night_pct = 0;
    a.dim.fixed_start_min = 2000; a.dim.fixed_end_min = 1440; a.dim.idle_s = 0;
    a.wipe_idle_s = 5; a.face = 7; a.orient = 3;
    uint8_t buf[64];
    size_t n = pnl_prefs_pack(&a, buf, sizeof buf);      /* pack writes what it is given */
    TEST_ASSERT_EQUAL_INT(0, pnl_prefs_unpack(buf, n, &b));
    TEST_ASSERT_EQUAL_UINT8(1, b.dim.mode);
    TEST_ASSERT_EQUAL_UINT8(5, b.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(5, b.dim.night_pct);          /* never a dark panel (D4: minimum duty >= 5 %) */
    TEST_ASSERT_EQUAL_UINT16(22 * 60, b.dim.fixed_start_min);
    TEST_ASSERT_EQUAL_UINT16(6 * 60, b.dim.fixed_end_min);
    TEST_ASSERT_EQUAL_UINT16(10, b.dim.idle_s);
    TEST_ASSERT_EQUAL_UINT16(60, b.wipe_idle_s);
    TEST_ASSERT_EQUAL_UINT8(0, b.face);
    TEST_ASSERT_EQUAL_UINT8(0, b.orient);
}

static void test_clamp_upper_bounds(void) {
    pnl_prefs_t p;
    pnl_prefs_defaults(&p);
    p.dim.day_pct = 150; p.dim.night_pct = 101;
    pnl_prefs_clamp(&p);
    TEST_ASSERT_EQUAL_UINT8(100, p.dim.day_pct);
    TEST_ASSERT_EQUAL_UINT8(100, p.dim.night_pct);
}

static void test_pack_cap_too_small(void) {
    pnl_prefs_t a = sample();
    uint8_t buf[20];
    TEST_ASSERT_EQUAL_size_t(0, pnl_prefs_pack(&a, buf, sizeof buf));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_explicit_byte_offsets);
    RUN_TEST(test_corrupt_crc_gives_defaults);
    RUN_TEST(test_newer_version_gives_defaults);
    RUN_TEST(test_short_and_wrong_magic_give_defaults);
    RUN_TEST(test_clamps_on_unpack);
    RUN_TEST(test_clamp_upper_bounds);
    RUN_TEST(test_pack_cap_too_small);
    return UNITY_END();
}
