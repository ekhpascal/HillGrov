/* The panel's touch mapping. NORMAL is the bench-proven passthrough (display
   rotated 180 in the adapter, GT911 already reports in that frame) and only
   clamps; FLIPPED does the 180-degree flip itself. Neither may ever wrap: the
   esp_lcd_touch mirrors do x_max - x on a uint16_t with no clamp, and a point
   outside the panel then becomes ~65000 (what_we_learned 2026-09-18). */
#include "unity.h"
#include "pnl_touch.h"

void setUp(void) {}
void tearDown(void) {}

#define W 1024
#define H 600

static void map(uint8_t o, uint16_t x, uint16_t y, uint16_t *ox, uint16_t *oy) {
    *ox = 0xBEEF; *oy = 0xBEEF;
    pnl_touch_map(o, x, y, W, H, ox, oy);
}

static void test_normal_is_passthrough(void) {
    uint16_t x, y;
    map(PNL_ORIENT_NORMAL, 0, 0, &x, &y);         TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_NORMAL, 1023, 0, &x, &y);      TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_NORMAL, 0, 599, &x, &y);       TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_NORMAL, 1023, 599, &x, &y);    TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_NORMAL, 512, 300, &x, &y);     TEST_ASSERT_EQUAL_UINT16(512, x);  TEST_ASSERT_EQUAL_UINT16(300, y);
}

static void test_flipped_turns_corners_around(void) {
    uint16_t x, y;
    map(PNL_ORIENT_FLIPPED, 0, 0, &x, &y);        TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 1023, 0, &x, &y);     TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 0, 599, &x, &y);      TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 1023, 599, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 512, 300, &x, &y);    TEST_ASSERT_EQUAL_UINT16(511, x);  TEST_ASSERT_EQUAL_UINT16(299, y);
}

static void test_out_of_range_clamps_and_never_wraps(void) {
    uint16_t x, y;
    map(PNL_ORIENT_NORMAL, 1100, 700, &x, &y);    TEST_ASSERT_EQUAL_UINT16(1023, x); TEST_ASSERT_EQUAL_UINT16(599, y);
    map(PNL_ORIENT_FLIPPED, 1100, 700, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 1024, 600, &x, &y);   TEST_ASSERT_EQUAL_UINT16(0, x);    TEST_ASSERT_EQUAL_UINT16(0, y);
    map(PNL_ORIENT_FLIPPED, 65535, 65535, &x, &y);
    TEST_ASSERT_TRUE(x < W && y < H);
}

static void test_unknown_orientation_behaves_as_normal(void) {
    uint16_t x, y;
    map(7, 10, 20, &x, &y);                       TEST_ASSERT_EQUAL_UINT16(10, x);   TEST_ASSERT_EQUAL_UINT16(20, y);
}

static void test_zero_size_panel_yields_origin(void) {
    uint16_t x = 5, y = 5;
    pnl_touch_map(PNL_ORIENT_FLIPPED, 10, 10, 0, 0, &x, &y);
    TEST_ASSERT_EQUAL_UINT16(0, x);
    TEST_ASSERT_EQUAL_UINT16(0, y);
}

int main(void) { UNITY_BEGIN();
    RUN_TEST(test_normal_is_passthrough);
    RUN_TEST(test_flipped_turns_corners_around);
    RUN_TEST(test_out_of_range_clamps_and_never_wraps);
    RUN_TEST(test_unknown_orientation_behaves_as_normal);
    RUN_TEST(test_zero_size_panel_yields_origin);
    return UNITY_END(); }
