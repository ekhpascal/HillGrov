/* The panel's own civil-time maths. The master's clock is UTC (nothing calls
   setenv("TZ")/tzset), so local = time(NULL) + time_svc_utc_offset(), done here
   with no libc tz. An unset clock must read as unset, never as a plausible
   wrong time (spec "Error handling"). */
#include <string.h>
#include "unity.h"
#include "pnl_time.h"

void setUp(void) {}
void tearDown(void) {}

static pnl_local_t at_utc(int y, int mo, int d, int h, int mi, int s, int32_t off) {
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(y, mo, d, h, mi, s), off, 1, &t);
    return t;
}

static void test_civil_anchor_points(void) {
    TEST_ASSERT_EQUAL_INT64(0, pnl_utc_from_civil(1970, 1, 1, 0, 0, 0));
    TEST_ASSERT_EQUAL_INT64(946684800, pnl_utc_from_civil(2000, 1, 1, 0, 0, 0));
    pnl_local_t t;
    pnl_local_time(0, 0, 1, &t);
    TEST_ASSERT_EQUAL_INT(1970, t.year); TEST_ASSERT_EQUAL_INT(1, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday);
    TEST_ASSERT_EQUAL_INT(4, t.wday);   /* Thursday */
}

static void test_known_weekday_and_date_text(void) {
    pnl_local_t t = at_utc(2025, 9, 18, 12, 0, 0, 0);
    TEST_ASSERT_EQUAL_INT(4, t.wday);
    char b[40];
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Thursday 18 September", b);
}

static void test_offset_across_midnight_and_year(void) {
    pnl_local_t t = at_utc(2025, 12, 31, 23, 30, 0, 3600);
    TEST_ASSERT_EQUAL_INT(2026, t.year); TEST_ASSERT_EQUAL_INT(1, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday);
    TEST_ASSERT_EQUAL_INT(0, t.hour);    TEST_ASSERT_EQUAL_INT(30, t.min);
    TEST_ASSERT_EQUAL_INT(4, t.wday);    /* 2026-01-01 is a Thursday */
    t = at_utc(2026, 1, 1, 3, 0, 0, -18000);
    TEST_ASSERT_EQUAL_INT(2025, t.year); TEST_ASSERT_EQUAL_INT(12, t.mon); TEST_ASSERT_EQUAL_INT(31, t.mday);
    TEST_ASSERT_EQUAL_INT(22, t.hour);
    TEST_ASSERT_EQUAL_INT(3, t.wday);    /* Wednesday */
}

static void test_month_boundary_and_leap_day(void) {
    pnl_local_t t = at_utc(2026, 4, 30, 23, 59, 0, 60);
    TEST_ASSERT_EQUAL_INT(5, t.mon); TEST_ASSERT_EQUAL_INT(1, t.mday); TEST_ASSERT_EQUAL_INT(0, t.hour);
    t = at_utc(2028, 2, 28, 23, 0, 0, 7200);
    TEST_ASSERT_EQUAL_INT(2, t.mon); TEST_ASSERT_EQUAL_INT(29, t.mday); TEST_ASSERT_EQUAL_INT(1, t.hour);
    TEST_ASSERT_EQUAL_INT(2, t.wday);    /* Tuesday */
    char b[40];
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Tuesday 29 February", b);
    TEST_ASSERT_EQUAL_INT(29, pnl_days_in_month(2024, 2));
    TEST_ASSERT_EQUAL_INT(28, pnl_days_in_month(2100, 2));
    TEST_ASSERT_EQUAL_INT(29, pnl_days_in_month(2000, 2));
    TEST_ASSERT_EQUAL_INT(30, pnl_days_in_month(2026, 4));
    TEST_ASSERT_EQUAL_INT(0, pnl_days_in_month(2026, 13));
}

static void test_clock_text_and_minute_of_day(void) {
    pnl_local_t t = at_utc(2026, 9, 18, 12, 32, 5, 7200);
    char b[16];
    pnl_fmt_clock(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("14:32", b);
    TEST_ASSERT_EQUAL_INT(14 * 60 + 32, t.minute_of_day);
    TEST_ASSERT_EQUAL_INT(5, t.sec);
}

static void test_unset_clock_is_honest(void) {
    pnl_local_t t;
    pnl_local_time(pnl_utc_from_civil(2026, 9, 18, 12, 0, 0), 7200, 0, &t);
    TEST_ASSERT_EQUAL_UINT8(0, t.valid);
    char b[32];
    pnl_fmt_clock(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("--:--", b);
    pnl_fmt_date(&t, b, sizeof b);
    TEST_ASSERT_EQUAL_STRING("Clock not set", b);
}

static void test_set_time_line_converts_local_to_utc(void) {
    char b[40];
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2026, 9, 18, 14, 32, 7200, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2026-09-18 12:32:00", b);
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2026, 12, 31, 21, 0, -18000, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2027-01-01 02:00:00", b);
    TEST_ASSERT_EQUAL_INT(0, pnl_set_time_line(2028, 2, 29, 10, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("SET TIME 2028-02-29 10:00:00", b);
}

static void test_set_time_line_refuses_out_of_range(void) {
    char b[40];
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2019, 12, 31, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2100, 1, 1, 0, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 2, 29, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 13, 1, 12, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 1, 1, 24, 0, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2026, 1, 1, 12, 60, 0, b, sizeof b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2020, 1, 1, 0, 30, 7200, b, sizeof b));    /* UTC lands in 2019 */
    TEST_ASSERT_EQUAL_INT(-1, pnl_set_time_line(2099, 12, 31, 23, 30, -3600, b, sizeof b));/* UTC lands in 2100 */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_civil_anchor_points);
    RUN_TEST(test_known_weekday_and_date_text);
    RUN_TEST(test_offset_across_midnight_and_year);
    RUN_TEST(test_month_boundary_and_leap_day);
    RUN_TEST(test_clock_text_and_minute_of_day);
    RUN_TEST(test_unset_clock_is_honest);
    RUN_TEST(test_set_time_line_converts_local_to_utc);
    RUN_TEST(test_set_time_line_refuses_out_of_range);
    return UNITY_END(); }
