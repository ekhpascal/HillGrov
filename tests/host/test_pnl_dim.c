#include <string.h>
#include "unity.h"
#include "pnl_dim.h"

/* pnl_dim_eval is the whole night/brightness decision. The clock must be set for any night (an unset clock never dims
   the panel by schedule), FOLLOW_LIGHTS needs at least one scheduled light, and the night level applies only after the
   idle timeout -- a panel being used stays at day brightness. */

static pnl_dim_cfg_t g_c;
static pnl_sched_t   g_s;

void setUp(void) {
    memset(&g_c, 0, sizeof g_c);
    g_c.mode = PNL_DIM_FOLLOW_LIGHTS; g_c.day_pct = 80; g_c.night_pct = 10;
    g_c.fixed_start_min = 22 * 60; g_c.fixed_end_min = 6 * 60; g_c.idle_s = 60;
    pnl_sched_reset(&g_s);
}
void tearDown(void) {}

static pnl_local_t at(int hh, int mm) {
    pnl_local_t t;
    memset(&t, 0, sizeof t);
    t.valid = 1; t.hour = hh; t.min = mm; t.minute_of_day = hh * 60 + mm;
    return t;
}

static void eval(int hh, int mm, uint32_t idle_ms, pnl_dim_out_t *o) {
    pnl_local_t t = at(hh, mm);
    pnl_dim_eval(&g_c, &g_s, &t, idle_ms, o);
}

static void light(uint16_t on, uint16_t off) {
    g_s.light_on[g_s.n_light] = on;
    g_s.light_off[g_s.n_light] = off;
    g_s.n_light++;
}

static void test_off_never_dims(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_OFF;
    light(6 * 60, 22 * 60);
    eval(23, 0, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night);
    TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_fixed_window_wrapping_midnight(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED;
    eval(23, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
    eval(22, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night);                 /* start is inside */
    eval(5, 59, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night);
    eval(6, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);   /* end is not */
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_fixed_window_not_wrapping(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED; g_c.fixed_start_min = 60; g_c.fixed_end_min = 300;
    eval(3, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night);
    eval(5, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night);
    eval(0, 59, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_fixed_empty_window(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED; g_c.fixed_start_min = 600; g_c.fixed_end_min = 600;
    eval(10, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_unset_clock_never_dims(void) {
    pnl_dim_out_t o;
    pnl_local_t t = at(23, 0);
    t.valid = 0;
    g_c.mode = PNL_DIM_FIXED;
    pnl_dim_eval(&g_c, &g_s, &t, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
    g_c.mode = PNL_DIM_FOLLOW_LIGHTS;
    light(6 * 60, 22 * 60);
    pnl_dim_eval(&g_c, &g_s, &t, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night);
}

static void test_follow_lights(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    eval(23, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_follow_lights_any_light_on_keeps_day(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    light(20 * 60, 4 * 60);                              /* wraps midnight: on 20:00-04:00 */
    eval(2, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(0, o.night);
    eval(5, 0, 120000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night);
}

static void test_follow_lights_without_schedules_is_day(void) {
    pnl_dim_out_t o;
    eval(23, 0, 600000, &o);
    TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);
}

static void test_idle_threshold(void) {
    pnl_dim_out_t o;
    light(6 * 60, 22 * 60);
    eval(23, 0, 59999, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(80, o.pct);   /* in use */
    eval(23, 0, 60000, &o);   TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(10, o.pct);
}

/* Carried obligation: the backlight never goes below PNL_DIM_MIN_PCT (5 %), whatever reaches the decision -- a dark
   panel must still be findable. pnl_prefs_clamp() already keeps stored levels in 5..100; this is the second fence. */
static void test_min_duty_clamped(void) {
    pnl_dim_out_t o;
    g_c.mode = PNL_DIM_FIXED; g_c.day_pct = 0; g_c.night_pct = 1;
    eval(23, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(1, o.night); TEST_ASSERT_EQUAL_UINT8(PNL_DIM_MIN_PCT, o.pct);
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(0, o.night); TEST_ASSERT_EQUAL_UINT8(PNL_DIM_MIN_PCT, o.pct);
    g_c.day_pct = 150;
    eval(12, 0, 120000, &o);  TEST_ASSERT_EQUAL_UINT8(100, o.pct);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_off_never_dims);
    RUN_TEST(test_fixed_window_wrapping_midnight);
    RUN_TEST(test_fixed_window_not_wrapping);
    RUN_TEST(test_fixed_empty_window);
    RUN_TEST(test_unset_clock_never_dims);
    RUN_TEST(test_follow_lights);
    RUN_TEST(test_follow_lights_any_light_on_keeps_day);
    RUN_TEST(test_follow_lights_without_schedules_is_day);
    RUN_TEST(test_idle_threshold);
    RUN_TEST(test_min_duty_clamped);
    return UNITY_END();
}
