/* The home screen's maths: the status band divides the width so 1..8 tiles
   all look deliberate (spec "Status band"), and the context line is "the most
   useful sentence a greenhouse clock can carry" -- honest when the clock is
   unset. */
#include <string.h>
#include "unity.h"
#include "hg_cfg.h"
#include "pnl_home.h"

void setUp(void) {}
void tearDown(void) {}

static pnl_local_t at(int h, int m) {
    pnl_local_t t;
    memset(&t, 0, sizeof t);
    t.valid = 1; t.hour = h; t.min = m; t.minute_of_day = h * 60 + m;
    return t;
}

static void test_band_layout_for_every_count(void) {
    static const int W[9]  = { 0, 240, 240, 240, 238, 188, 156, 132, 115 };
    static const int X0[9] = { 0, 368, 244, 120,   0,   2,   0,   2,   0 };
    static const pnl_tile_detail_t D[9] = { PNL_TILE_MIN, PNL_TILE_FULL, PNL_TILE_FULL, PNL_TILE_FULL, PNL_TILE_FULL,
                                            PNL_TILE_MID, PNL_TILE_MID, PNL_TILE_MIN, PNL_TILE_MIN };
    for (int n = 1; n <= 8; n++) {
        pnl_band_t b;
        TEST_ASSERT_EQUAL_INT(0, pnl_band_layout(n, PNL_BAND_WIDTH, &b));
        TEST_ASSERT_EQUAL_INT_MESSAGE(W[n], b.tile_w, "tile_w");
        TEST_ASSERT_EQUAL_INT_MESSAGE(X0[n], b.x0, "x0");
        TEST_ASSERT_EQUAL_INT_MESSAGE(D[n], b.detail, "detail");
        TEST_ASSERT_EQUAL_INT(PNL_BAND_GAP, b.gap);
        TEST_ASSERT_TRUE(b.x0 + n * b.tile_w + (n - 1) * b.gap <= PNL_BAND_WIDTH);
    }
    pnl_band_t b;
    TEST_ASSERT_EQUAL_INT(-1, pnl_band_layout(0, PNL_BAND_WIDTH, &b));
    TEST_ASSERT_EQUAL_INT(-1, pnl_band_layout(9, PNL_BAND_WIDTH, &b));
}

/* Controller ruling C13: pnl_in_daily_window() is the panel's ONE "is time t
   inside a daily on/off window" test (exported from pnl_home.h so Task 27's
   night dimming can reuse it). Host-tested directly here, including the
   midnight-wrap case, not just indirectly through pnl_lights_on_now(). */
static void test_in_daily_window_directly(void) {
    /* normal same-day window */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(6 * 60, 22 * 60, 6 * 60));       /* start inclusive */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(6 * 60, 22 * 60, 12 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(6 * 60, 22 * 60, 22 * 60));      /* end exclusive */
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(6 * 60, 22 * 60, 5 * 59));
    /* wraps past midnight (start > end) */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(20 * 60, 4 * 60, 23 * 60));      /* evening side */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(20 * 60, 4 * 60, 0));            /* midnight itself */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(20 * 60, 4 * 60, 3 * 60 + 59));  /* just before end */
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(20 * 60, 4 * 60, 4 * 60));       /* end exclusive, post-midnight */
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(20 * 60, 4 * 60, 12 * 60));      /* daytime, outside */
    TEST_ASSERT_EQUAL_INT(1, pnl_in_daily_window(20 * 60, 4 * 60, 20 * 60));      /* start inclusive, evening */
    /* equal start/end is never "inside" -- callers handle "always" themselves */
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(9 * 60, 9 * 60, 9 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_in_daily_window(9 * 60, 9 * 60, 0));
}

static pnl_sched_t one_zone(int on_h, int off_h) {
    hg_zone_cfg_t cfg;
    hg_defaults_cfg(&cfg);
    cfg.shelf[0].enabled = 1;
    cfg.shelf[0].light.on_min = (uint16_t)(on_h * 60);
    cfg.shelf[0].light.off_min = (uint16_t)(off_h * 60);
    pnl_sched_t s;
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, NULL);
    return s;
}

static void test_lights_sentences(void) {
    pnl_sched_t s = one_zone(6, 22);
    char b[48];
    pnl_local_t t = at(19, 46);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 2h 14m", b);
    t = at(21, 46);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 14m", b);
    t = at(23, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
    t = at(5, 59);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
}

static void test_wrap_around_midnight(void) {
    pnl_sched_t s = one_zone(20, 4);
    TEST_ASSERT_EQUAL_INT(1, pnl_lights_on_now(&s, 1 * 60));
    TEST_ASSERT_EQUAL_INT(1, pnl_lights_on_now(&s, 21 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_lights_on_now(&s, 12 * 60));
    TEST_ASSERT_EQUAL_INT(0, pnl_lights_on_now(&s, 4 * 60));   /* off is exclusive */
    char b[48];
    pnl_local_t t = at(1, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 3h 0m", b);
}

static void test_earliest_off_and_soonest_on_win(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    s.n_light = 2;
    s.light_on[0] = 6 * 60;  s.light_off[0] = 22 * 60;
    s.light_on[1] = 8 * 60;  s.light_off[1] = 20 * 60;
    char b[48];
    pnl_local_t t = at(10, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights off in 10h 0m", b);
    t = at(23, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Lights on at 06:00", b);
}

static void test_watering_sentences(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    s.n_water = 1;
    s.water_start[0] = 6 * 60; s.water_end[0] = 18 * 60;
    char b[48];
    pnl_local_t t = at(5, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Next watering 06:00", b);
    t = at(10, 0);
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Watering window open until 18:00", b);
    s.water_end[0] = s.water_start[0];   /* equal = always (hg_water_cfg_t) */
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Watering on demand", b);
}

static void test_nothing_to_say_and_unset_clock(void) {
    pnl_sched_t s;
    pnl_sched_reset(&s);
    char b[48] = "x";
    pnl_local_t t = at(10, 0);
    TEST_ASSERT_EQUAL_INT(0, pnl_ctx_line(&s, &t, b, sizeof b));
    TEST_ASSERT_EQUAL_STRING("", b);
    s = one_zone(6, 22);
    t.valid = 0;
    pnl_ctx_line(&s, &t, b, sizeof b);  TEST_ASSERT_EQUAL_STRING("Clock not set", b);
}

static void test_add_zone_respects_enable_mode_and_shelf_count(void) {
    hg_zone_cfg_t cfg;
    hg_defaults_cfg(&cfg);                   /* all shelves disabled; water mode AUTO */
    cfg.shelf[0].enabled = 1;
    cfg.shelf[1].enabled = 1;
    cfg.shelf[1].water.mode = 0;             /* OFF: no watering window */
    hg_zone_hw_t hw;
    hg_defaults_hw(&hw);
    hw.shelf_count = 1;
    pnl_sched_t s;
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, &hw);
    TEST_ASSERT_EQUAL_INT(1, s.n_light);     /* shelf 1 is beyond the hardware */
    TEST_ASSERT_EQUAL_INT(1, s.n_water);
    pnl_sched_reset(&s);
    pnl_sched_add_zone(&s, &cfg, NULL);
    TEST_ASSERT_EQUAL_INT(2, s.n_light);
    TEST_ASSERT_EQUAL_INT(1, s.n_water);     /* shelf 1's water is OFF */
    for (int z = 0; z < 20; z++) pnl_sched_add_zone(&s, &cfg, NULL);
    TEST_ASSERT_EQUAL_INT(PNL_SCHED_MAX, s.n_light);   /* capacity-bounded */
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_band_layout_for_every_count);
    RUN_TEST(test_in_daily_window_directly);
    RUN_TEST(test_lights_sentences);
    RUN_TEST(test_wrap_around_midnight);
    RUN_TEST(test_earliest_off_and_soonest_on_win);
    RUN_TEST(test_watering_sentences);
    RUN_TEST(test_nothing_to_say_and_unset_clock);
    RUN_TEST(test_add_zone_respects_enable_mode_and_shelf_count);
    return UNITY_END(); }
