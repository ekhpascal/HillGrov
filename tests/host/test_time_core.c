#include <string.h>
#include "unity.h"
#include "time_core.h"

/* time_core.c is pure (no IDF headers) -- hand-parses the POSIX TZ subset every
   IANA zone reduces to ("STD[offset][DST[offset],Mm.w.d[/time],Mm.w.d[/time]]")
   and converts an M-rule to the UTC instant of that year's transition. Every
   epoch below was cross-checked with `date -u -d "<civil date>" +%s` (GNU
   date); the two vectors the brief gave as raw epochs (1768478400 / 1784116800)
   matched that tool exactly, so the rest of the edges were derived the same way. */

void setUp(void) {}
void tearDown(void) {}

static const char *const CET = "CET-1CEST,M3.5.0,M10.5.0/3";

static void test_parse_cet_std_dst(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse(CET, &r));
    TEST_ASSERT_EQUAL_INT32(3600, r.std_off_s);
    TEST_ASSERT_EQUAL_INT32(7200, r.dst_off_s);
    TEST_ASSERT_EQUAL_UINT8(1, r.has_dst);
}

static void test_offset_winter(void) {
    tz_rule_t r; TEST_ASSERT_EQUAL_INT(0, tz_parse(CET, &r));
    TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(&r, 1768478400u));   /* 2026-01-15 12:00:00Z */
}

static void test_offset_summer(void) {
    tz_rule_t r; TEST_ASSERT_EQUAL_INT(0, tz_parse(CET, &r));
    TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(&r, 1784116800u));   /* 2026-07-15 12:00:00Z */
}

static void test_spring_forward_edge(void) {
    tz_rule_t r; TEST_ASSERT_EQUAL_INT(0, tz_parse(CET, &r));
    TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(&r, 1774745999u));   /* 2026-03-29 00:59:59Z */
    TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(&r, 1774746000u));   /* 2026-03-29 01:00:00Z */
}

static void test_autumn_back_edge(void) {
    tz_rule_t r; TEST_ASSERT_EQUAL_INT(0, tz_parse(CET, &r));
    TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(&r, 1792889999u));   /* 2026-10-25 00:59:59Z */
    TEST_ASSERT_EQUAL_INT32(3600, tz_offset_at(&r, 1792890000u));   /* 2026-10-25 01:00:00Z */
}

static void test_us_eastern(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse("EST5EDT,M3.2.0,M11.1.0", &r));
    TEST_ASSERT_EQUAL_INT32(-18000, r.std_off_s);
    TEST_ASSERT_EQUAL_INT32(-14400, r.dst_off_s);
    TEST_ASSERT_EQUAL_UINT8(1, r.has_dst);
    TEST_ASSERT_EQUAL_INT32(-14400, tz_offset_at(&r, 1772953200u)); /* 2026-03-08 07:00:00Z (2nd Sun Mar, 02:00 local std default) */
}

static void test_utc0_no_dst(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse("UTC0", &r));
    TEST_ASSERT_EQUAL_INT32(0, r.std_off_s);
    TEST_ASSERT_EQUAL_UINT8(0, r.has_dst);
    TEST_ASSERT_EQUAL_INT32(0, tz_offset_at(&r, 1768478400u));
}

static void test_quoted_name_form(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse("<+03>-3", &r));
    TEST_ASSERT_EQUAL_INT32(10800, r.std_off_s);
    TEST_ASSERT_EQUAL_UINT8(0, r.has_dst);
    TEST_ASSERT_EQUAL_INT32(10800, tz_offset_at(&r, 1768478400u));
}

static void test_garbage_rejected(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("Nope", &r));
}

static void test_julian_rule_unsupported(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("CET-1CEST,J60,J300", &r));      /* Julian, leap-excluded */
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("CET-1CEST,60,300", &r));        /* Julian, leap-included */
}

static void test_tz_check_mirrors_tz_parse(void) {
    TEST_ASSERT_EQUAL_INT(0,  tz_check(CET));
    TEST_ASSERT_EQUAL_INT(0,  tz_check("UTC0"));
    TEST_ASSERT_EQUAL_INT(0,  tz_check("<+03>-3"));
    TEST_ASSERT_EQUAL_INT(-1, tz_check("Nope"));
}

/* --- Fix round 1, item 1: tz_offset_at must evaluate the Mm.w.d rules
   against the LOCAL year, not the raw UTC calendar year -- a late-December
   UTC instant can already be Jan 1 local (positive offset) or an early-
   January UTC instant can still be Dec 31 local (negative offset), and the
   wrong year picks the wrong year's transition dates. */

static void test_year_rollover_forward_offset(void) {
    /* UTC+1 zone, DST window = [Jan 1 local 00:00, 3rd Sun Jan local 02:00).
       Jan 1 2023 is a Sunday (date -u -d "2023-01-01" +%A). Queried at
       2022-12-31 23:30:00Z (UTC calendar day/year: 2022-12-31) -- local time
       is already 2023-01-01 00:30, 30 min into the DST window, so the
       correct calendar year to evaluate the rule against is 2023, not 2022.
       Using the raw UTC year (2022) puts the window a full year in the past
       (nowhere near this instant) and wrongly returns STD. */
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse("STD-1DST,M1.1.0/0,M1.3.0", &r));
    TEST_ASSERT_EQUAL_INT32(7200, tz_offset_at(&r, 1672529400u));   /* 2022-12-31 23:30:00Z */
}

static void test_year_rollover_backward_offset(void) {
    /* Mirror case: UTC-3 zone, same rule shape. Queried at 2023-01-01
       01:00:00Z -- the UTC calendar day is already Jan 1 2023, but local
       time (utc-3) is still 2022-12-31 22:00, before the DST window (which
       opens at local Jan 1 00:00 of *its own* year) has even opened. Must
       stay STD. */
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(0, tz_parse("STD3DST,M1.1.0/0,M1.3.0", &r));
    TEST_ASSERT_EQUAL_INT32(-10800, tz_offset_at(&r, 1672534800u));  /* 2023-01-01 01:00:00Z */
}

/* --- Fix round 1, item 2: parse_hms must reject unbounded magnitudes --
   STD/DST offsets outside -24..24h, an M-rule's /time outside -167..167h. */

static void test_std_offset_out_of_range_rejected(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("STD25", &r));            /* 25h, no DST */
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("STD-25", &r));
}

static void test_mrule_time_out_of_range_rejected(void) {
    tz_rule_t r;
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("CET-1CEST,M3.5.0/200,M10.5.0/3", &r));  /* 200h > 167h */
    TEST_ASSERT_EQUAL_INT(-1, tz_parse("CET-1CEST,M3.5.0,M10.5.0/-200", &r));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parse_cet_std_dst);
    RUN_TEST(test_offset_winter);
    RUN_TEST(test_offset_summer);
    RUN_TEST(test_spring_forward_edge);
    RUN_TEST(test_autumn_back_edge);
    RUN_TEST(test_us_eastern);
    RUN_TEST(test_utc0_no_dst);
    RUN_TEST(test_quoted_name_form);
    RUN_TEST(test_garbage_rejected);
    RUN_TEST(test_julian_rule_unsupported);
    RUN_TEST(test_tz_check_mirrors_tz_parse);
    RUN_TEST(test_year_rollover_forward_offset);
    RUN_TEST(test_year_rollover_backward_offset);
    RUN_TEST(test_std_offset_out_of_range_rejected);
    RUN_TEST(test_mrule_time_out_of_range_rejected);
    return UNITY_END();
}
