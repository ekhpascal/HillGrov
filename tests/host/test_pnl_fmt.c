/* Every word and number the panel shows about zones, the master and Wi-Fi,
   in the web's vocabulary (map-parity A11/A12) -- and ASCII only: the built-in
   Montserrat renders nothing for U+2014/U+00B7 or a UTF-8 SSID, so every output
   here is checked character by character. */
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "pnl_fmt.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_ascii(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        TEST_ASSERT_TRUE_MESSAGE(*p >= 0x20 && *p <= 0x7E, s);
}

static void test_zone_name(void) {
    hg_node_t n;
    memset(&n, 0, sizeof n);
    n.id = 3;
    char b[17];
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("Z3", b);
    snprintf(n.name, sizeof n.name, "Tomatoes");
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("Tomatoes", b);
    memcpy(n.name, "caf\xC3\xA9", 6);
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_STRING("caf??", b);
    memset(n.name, 'x', sizeof n.name);   /* no terminator in the 16-byte field */
    pnl_zone_name(&n, b);
    TEST_ASSERT_EQUAL_INT(16, (int)strlen(b));
    assert_ascii(b);
}

static void test_age_matches_the_web(void) {
    char b[24];
    pnl_fmt_age(1000, 988, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("12s ago", b);
    pnl_fmt_age(1000, 757, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("4m 3s ago", b);
    pnl_fmt_age(20000, 20000 - 11100, b, sizeof b);            TEST_ASSERT_EQUAL_STRING("3h 5m ago", b);
    pnl_fmt_age(200000, 200000 - (2 * 86400 + 4 * 3600), b, sizeof b);
                                                               TEST_ASSERT_EQUAL_STRING("2d 4h ago", b);
    pnl_fmt_age(1000, 1005, b, sizeof b);                      TEST_ASSERT_EQUAL_STRING("0s ago", b);
    assert_ascii(b);
}

static void test_sta_and_ap_lines(void) {
    wifi_status_t w;
    memset(&w, 0, sizeof w);
    char b[96];
    w.sta_up = 1;
    snprintf(w.sta_ip, sizeof w.sta_ip, "192.168.1.5");
    snprintf(w.sta_ssid, sizeof w.sta_ssid, "house");
    w.rssi = -61;
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("192.168.1.5 | house | -61 dBm", b);
    assert_ascii(b);
    w.sta_up = 0;
    snprintf(w.sta_reason, sizeof w.sta_reason, "AUTH_FAIL");
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("STA down: AUTH_FAIL", b);
    w.sta_reason[0] = '\0';
    pnl_fmt_sta(&w, b, sizeof b);                              TEST_ASSERT_EQUAL_STRING("STA down: --", b);
    snprintf(w.ap_ssid, sizeof w.ap_ssid, "HillGrow");
    w.ap_clients = 2;
    snprintf(w.ap_ip, sizeof w.ap_ip, "192.168.7.7");
    pnl_fmt_ap(&w, b, sizeof b);                               TEST_ASSERT_EQUAL_STRING("HillGrow | 2 client(s) | 192.168.7.7", b);
    memcpy(w.ap_ssid, "Gr\xC3\xBCn", 6);
    pnl_fmt_ap(&w, b, sizeof b);
    assert_ascii(b);
}

static void test_master_time_is_local_and_labelled(void) {
    char b[64];
    pnl_fmt_master_time("2026-09-18 12:32:05", "NTP", 7200, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 14:32:05 (UTC+02:00) NTP", b);
    pnl_fmt_master_time("2026-09-18 12:32:05", "SET", -18000, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 07:32:05 (UTC-05:00) SET", b);
    pnl_fmt_master_time("2026-09-18 12:32:05", "NTP", 19800, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("2026-09-18 18:02:05 (UTC+05:30) NTP", b);
    pnl_fmt_master_time("1970-01-01 00:03:28", "NONE", 7200, 0, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Clock not set (NONE)", b);
    pnl_fmt_master_time("garbage", "SET", 0, 1, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("garbage SET", b);
    assert_ascii(b);
}

static void test_readings_follow_shelf_totals(void) {
    hg_node_t n;
    memset(&n, 0, sizeof n);
    pnl_readings_t r;
    char b[16];
    pnl_node_readings(&n, &r);                                 /* no shelves */
    TEST_ASSERT_EQUAL_UINT8(0, r.any);
    pnl_fmt_reading(&r, 0, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("--", b);
    n.hb.n_shelves = 2;
    pnl_node_readings(&n, &r);                                 /* shelves, all zero */
    TEST_ASSERT_EQUAL_UINT8(0, r.any);
    n.hb.shelf[0].pct_a = 40; n.hb.shelf[0].pct_b = 42;
    n.hb.shelf[1].pump_today_s = 30;
    pnl_node_readings(&n, &r);
    TEST_ASSERT_EQUAL_UINT8(1, r.any);
    TEST_ASSERT_EQUAL_INT(21, r.soil_pct);                     /* round(82/4 = 20.5) = 21, JS Math.round */
    TEST_ASSERT_EQUAL_INT(0, r.light_pct);
    TEST_ASSERT_EQUAL_INT(30, r.pump_s);
    pnl_fmt_reading(&r, 0, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("21%", b);
    pnl_fmt_reading(&r, 1, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("0%", b);
    pnl_fmt_reading(&r, 2, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("30s", b);
    pnl_fmt_reading(&r, 7, b, sizeof b);                       TEST_ASSERT_EQUAL_STRING("--", b);
    n.hb.n_shelves = 9;                                        /* never trust the count past 4 */
    n.hb.shelf[3].white = 100; n.hb.shelf[3].red = 100;
    pnl_node_readings(&n, &r);
    TEST_ASSERT_EQUAL_INT(25, r.light_pct);                    /* 200 / (2 x 4) */
    assert_ascii(b);
}

/* The shared roller builder reproduces the three builders it replaced (sys_time build_opts, wdg_field two_digit_opts,
 * scr_panel's hours loop) byte for byte, in the buffers they used. */
static void test_roller_opts_match_the_old_builders(void) {
    char hh[24 * 3], mm[60 * 3], yy[12 * 5 + 1], ref[256];
    size_t o = 0;
    for (int i = 0; i < 24; i++) o += (size_t)snprintf(ref + o, sizeof ref - o, "%s%02d", i ? "\n" : "", i);
    TEST_ASSERT_EQUAL_UINT32(o, pnl_fmt_roller_opts(hh, sizeof hh, 0, 24, 2));
    TEST_ASSERT_EQUAL_STRING(ref, hh);
    TEST_ASSERT_EQUAL_UINT32(24 * 3 - 1, strlen(hh));         /* the 24 * 3 buffers fit exactly */
    TEST_ASSERT_EQUAL_UINT32(60 * 3 - 1, pnl_fmt_roller_opts(mm, sizeof mm, 0, 60, 2));
    TEST_ASSERT_EQUAL_STRING_LEN("00\n01\n02", mm, 8);
    TEST_ASSERT_EQUAL_STRING("58\n59", mm + strlen(mm) - 5);
    TEST_ASSERT_EQUAL_UINT32(12 * 5 - 1, pnl_fmt_roller_opts(yy, sizeof yy, 2024, 12, 4));
    TEST_ASSERT_EQUAL_STRING_LEN("2024\n2025", yy, 9);
    TEST_ASSERT_EQUAL_UINT32(8, pnl_fmt_roller_opts(ref, 9, 1, 31, 2));   /* too small: whole numbers only */
    TEST_ASSERT_EQUAL_STRING("01\n02\n03", ref);
    TEST_ASSERT_EQUAL_UINT32(0, pnl_fmt_roller_opts(ref, 0, 0, 24, 2));
}

static void test_trim_eol(void) {
    char a[] = "OK SET TIME 2026-09-29 12:00:00\r\n\n", b[] = "ERR BUSY", c[] = "\n";
    pnl_fmt_trim_eol(a); TEST_ASSERT_EQUAL_STRING("OK SET TIME 2026-09-29 12:00:00", a);
    pnl_fmt_trim_eol(b); TEST_ASSERT_EQUAL_STRING("ERR BUSY", b);
    pnl_fmt_trim_eol(c); TEST_ASSERT_EQUAL_STRING("", c);
    pnl_fmt_trim_eol(NULL);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_roller_opts_match_the_old_builders);
    RUN_TEST(test_trim_eol);
    RUN_TEST(test_zone_name);
    RUN_TEST(test_age_matches_the_web);
    RUN_TEST(test_sta_and_ap_lines);
    RUN_TEST(test_master_time_is_local_and_labelled);
    RUN_TEST(test_readings_follow_shelf_totals);
    return UNITY_END(); }
