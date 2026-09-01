#include <stddef.h>
#include "unity.h"
#include "hg_cfg.h"

void setUp(void) {}
void tearDown(void) {}

static void test_offsets_pinned(void) {
    TEST_ASSERT_EQUAL_size_t(32,  offsetof(hg_zone_hw_t, shelf));
    TEST_ASSERT_EQUAL_size_t(8,   offsetof(hg_shelf_hw_t, soil_dry_mv));
    TEST_ASSERT_EQUAL_size_t(20,  offsetof(hg_shelf_hw_t, pump_max_run_s));
    TEST_ASSERT_EQUAL_size_t(48,  offsetof(hg_zone_cfg_t, shelf));
    TEST_ASSERT_EQUAL_size_t(24,  offsetof(hg_shelf_cfg_t, light));
    TEST_ASSERT_EQUAL_size_t(36,  offsetof(hg_shelf_cfg_t, water));
    TEST_ASSERT_EQUAL_size_t(52,  offsetof(hg_shelf_cfg_t, fan));
    TEST_ASSERT_EQUAL_size_t(56,  offsetof(hg_shelf_cfg_t, vib));
    TEST_ASSERT_EQUAL_size_t(6,   offsetof(hg_zone_cfg_t, link_loss_timeout_s));
    TEST_ASSERT_EQUAL_size_t(60,  offsetof(hg_daily_t, crc));
}

static void test_defaults_hw(void) {
    hg_zone_hw_t hw;
    hg_defaults_hw(&hw);
    TEST_ASSERT_EQUAL_UINT8(4, hw.shelf_count);
    TEST_ASSERT_EQUAL_HEX8(0x40, hw.pca_addr);
    TEST_ASSERT_EQUAL_HEX8(0x20, hw.pcf_addr);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, hw.pcf_active_low_mask);
    TEST_ASSERT_EQUAL_UINT16(1000, hw.pca_freq_hz);
    TEST_ASSERT_EQUAL_UINT8(32, hw.soil_gpio[0]);
    TEST_ASSERT_EQUAL_UINT8(26, hw.soil_gpio[7]);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(2 * i,     hw.shelf[i].led_ch[0]);
        TEST_ASSERT_EQUAL_UINT8(2 * i + 1, hw.shelf[i].led_ch[1]);
        TEST_ASSERT_EQUAL_UINT8(i,         hw.shelf[i].pump_pin);
        TEST_ASSERT_EQUAL_UINT8(4 + i,     hw.shelf[i].fan_pin);
        TEST_ASSERT_EQUAL_UINT16(60,       hw.shelf[i].pump_max_run_s);
        TEST_ASSERT_EQUAL_UINT16(600,      hw.shelf[i].pump_max_daily_s);
        TEST_ASSERT_EQUAL_UINT16(2800,     hw.shelf[i].soil_dry_mv[0]);
        TEST_ASSERT_EQUAL_UINT16(1300,     hw.shelf[i].soil_wet_mv[1]);
        TEST_ASSERT_EQUAL_UINT8(100,       hw.shelf[i].led_max_pct[0]);
        TEST_ASSERT_EQUAL_UINT8(8 + i,     hw.shelf[i].vib_ch);
    }
    TEST_ASSERT_EQUAL_UINT8(0, hw.aux[0].type);
}

static void test_defaults_cfg_inert(void) {
    hg_zone_cfg_t c;
    hg_defaults_cfg(&c);
    TEST_ASSERT_EQUAL_UINT32(0, c.generation);
    TEST_ASSERT_EQUAL_UINT8(HG_SRC_LOCAL, c.source);
    TEST_ASSERT_EQUAL_UINT16(30, c.link_loss_timeout_s);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(0, c.shelf[i].enabled);      /* nothing energises by default */
        TEST_ASSERT_EQUAL_UINT16(360,  c.shelf[i].light.on_min);   /* 06:00 */
        TEST_ASSERT_EQUAL_UINT16(1320, c.shelf[i].light.off_min);  /* 22:00 */
        TEST_ASSERT_EQUAL_UINT8(1,  c.shelf[i].water.mode);        /* AUTO (but shelf disabled) */
        TEST_ASSERT_EQUAL_UINT8(45, c.shelf[i].water.target_pct);
        TEST_ASSERT_EQUAL_UINT16(20, c.shelf[i].water.dose_s);
        TEST_ASSERT_EQUAL_UINT8(3,  c.shelf[i].fan.mode);          /* CYCLE */
        TEST_ASSERT_EQUAL_UINT8(0,  c.shelf[i].vib.mode);           /* OFF */
        TEST_ASSERT_EQUAL_UINT8(60, c.shelf[i].vib.intensity_pct);
    }
    TEST_ASSERT_EQUAL_UINT8(0, c.aux[0].mode);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_offsets_pinned);
    RUN_TEST(test_defaults_hw);
    RUN_TEST(test_defaults_cfg_inert);
    return UNITY_END();
}
